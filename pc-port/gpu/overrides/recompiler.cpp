// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/perf_stats.h"
#include <string>
#include <cstdlib>
#include <cstdio>
#include <atomic>
#include <array>
#include <vector>
#include "common/logging/classes.h"
#include "shader_recompiler/frontend/control_flow_graph.h"
#include "shader_recompiler/frontend/decode.h"
#include "shader_recompiler/frontend/structured_control_flow.h"
#include "shader_recompiler/frontend/translate/translate.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/ir/post_order.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/recompiler.h"

namespace Shader {

// A wave64 v_readlane of a lane past a smaller host subgroup (lanes 32-63 on a 32-wide GPU) is an
// OpGroupNonUniformBroadcast of a lane that does not exist: undefined. LowerWave64BallotPass
// rebuilds those through shared memory in compute shaders; the ones left, in every other stage,
// read the same lane of the host subgroup instead. A reduction then spans the host subgroup, which
// its shuffles already do, so a loop waiting for the reduced value still ends (a GPU hang before).
static void ClampReadLanesToSubgroup(IR::Program& program, const Profile& profile) {
    const u32 size = profile.subgroup_size;
    if (size == 0 || size >= 64) {
        return;
    }
    for (IR::Block* block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() != IR::Opcode::ReadLane) {
                continue;
            }
            const IR::Value lane = inst.Arg(1);
            if (lane.IsImmediate()) {
                if (lane.U32() >= size) {
                    inst.SetArg(1, IR::Value{lane.U32() & (size - 1)});
                }
            } else {
                IR::IREmitter ir{*block, IR::Block::InstructionList::s_iterator_to(inst)};
                inst.SetArg(1, ir.BitwiseAnd(IR::U32{lane}, ir.Imm32(size - 1)));
            }
        }
    }
}

// A shuffle or lane read whose source lane is inactive (not launched, or outside the primitive,
// as in the partial quads and warps along the odd 513-pixel edge of Ghost of Tsushima's loading
// screen tiles) returns an undefined value in Vulkan. ps 361a48f5 takes a wave minimum with
// ShuffleXor and ReadLane and loops until it reaches 255; one stale value below 255 kept it at
// that value forever and the GPU hung. Outside compute shaders the source lane is now tested in
// the ballot of active lanes: a shuffle from an inactive lane keeps the lane's own value and a
// lane read takes the first active lane's, which leaves a reduction over the active lanes intact.
static IR::U1 LaneIsActive(IR::IREmitter& ir, const IR::U32& lane) {
    const IR::U64 active = ir.Ballot(ir.Imm1(true));
    const IR::U64 bit = IR::U64{ir.BitwiseAnd(IR::U64{ir.ShiftRightLogical(active, lane)}, ir.Imm64(u64{1}))};
    return ir.INotEqual(bit, ir.Imm64(u64{0}));
}

static void GuardLaneReadsToActiveLanes(IR::Program& program) {
    if (program.info.hw_stage == HwStage::Compute) {
        return;
    }
    std::vector<IR::Inst*> worklist;
    for (IR::Block* block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            const auto opcode = inst.GetOpcode();
            if (opcode == IR::Opcode::ShuffleXor || opcode == IR::Opcode::ReadLane) {
                worklist.push_back(&inst);
            }
        }
    }
    for (IR::Inst* inst : worklist) {
        IR::IREmitter ir{*inst->GetParent(), IR::Block::InstructionList::s_iterator_to(*inst)};
        const IR::U32 value{inst->Arg(0)};
        IR::U32 result;
        if (inst->GetOpcode() == IR::Opcode::ShuffleXor) {
            const IR::U32 mask{inst->Arg(1)};
            const IR::U1 active = LaneIsActive(ir, IR::U32{ir.BitwiseXor(ir.LaneId(), mask)});
            result = IR::U32{ir.Select(active, ir.ShuffleXor(value, mask), value)};
        } else {
            const IR::U32 lane{inst->Arg(1)};
            const IR::U1 active = LaneIsActive(ir, lane);
            result = IR::U32{ir.Select(active, ir.ReadLane(value, lane), ir.ReadFirstLane(value))};
        }
        inst->ReplaceUsesWithAndRemove(result);
    }
}

// Vulkan allows OpBitFieldInsert on 32-bit integers only (VUID-StandaloneSpirv-Base-04781), but
// S_BITSET0_B64/S_BITSET1_B64 became a 64-bit one. Ghost of Tsushima's compute shader b5500bd4
// clears the handled lane's bit of a 64-bit mask that way in its waterfall loop over per-lane
// textures; the GPU hung (DEVICE LOST) once it ran. The insert is rebuilt from 64-bit shifts,
// AND and OR, which Vulkan allows.
static void LowerBitFieldInsert64(IR::Program& program) {
    std::vector<IR::Inst*> worklist;
    for (IR::Block* block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() == IR::Opcode::BitFieldInsert64) {
                worklist.push_back(&inst);
            }
        }
    }
    for (IR::Inst* inst : worklist) {
        IR::IREmitter ir{*inst->GetParent(), IR::Block::InstructionList::s_iterator_to(*inst)};
        const IR::U64 base{inst->Arg(0)};
        const IR::U64 insert{inst->Arg(1)};
        const IR::U32 offset{inst->Arg(2)};
        const IR::Value count = inst->Arg(3);
        // The field of `count` ones; a count of 64 or more is the whole register.
        IR::U64 field;
        if (count.IsImmediate()) {
            field = ir.Imm64(count.U32() >= 64 ? ~u64{0} : (u64{1} << count.U32()) - 1);
        } else {
            field = IR::U64{ir.ISub(IR::U64{ir.ShiftLeftLogical(ir.Imm64(u64{1}), IR::U32{count})},
                                    ir.Imm64(u64{1}))};
        }
        const IR::U64 mask{ir.ShiftLeftLogical(field, offset)};
        const IR::U64 kept{ir.BitwiseAnd(base, IR::U64{ir.BitwiseNot(mask)})};
        const IR::U64 placed{ir.BitwiseAnd(IR::U64{ir.ShiftLeftLogical(insert, offset)}, mask)};
        inst->ReplaceUsesWithAndRemove(IR::U64{ir.BitwiseOr(kept, placed)});
    }
}

IR::BlockList GenerateBlocks(const IR::AbstractSyntaxList& syntax_list) {
    size_t num_syntax_blocks{};
    for (const auto& [_, type] : syntax_list) {
        if (type == IR::AbstractSyntaxNode::Type::Block) {
            ++num_syntax_blocks;
        }
    }
    IR::BlockList blocks{};
    blocks.reserve(num_syntax_blocks);
    for (const auto& [data, type] : syntax_list) {
        if (type == IR::AbstractSyntaxNode::Type::Block) {
            blocks.push_back(data.block);
        }
    }
    return blocks;
}

void EmitControlFlowGraph(IR::Program& program, Pools& pools, Gcn::CFG& cfg,
                          RuntimeInfo& runtime_info, const Profile& profile) {
    Gcn::Translator translator{program.info, runtime_info, profile};
    for (auto& block : cfg) {
        const u32 start = block.begin_index;
        const u32 size = block.end_index - start + 1;
        auto* ir_block = pools.block_pool.Create(pools.inst_pool);
        ir_block->cfg_block = &block;
        block.ir_block = ir_block;
        translator.Translate(ir_block, block.begin, block.cond,
                             std::span{program.ins_list}.subspan(start, size));
        program.blocks.push_back(ir_block);
    }
    translator.EmitPrologue(program.blocks.front());
    ASSERT_MSG(!program.info.translation_failed, "Shader translation has failed");
    for (auto& block : cfg) {
        auto* ir_block = block.ir_block;
        if (block.branch_true) {
            auto* true_block = block.branch_true->ir_block;
            ir_block->AddBranch(true_block);
        }
        if (block.branch_false) {
            auto* false_block = block.branch_false->ir_block;
            ir_block->AddBranch(false_block);
        }
    }
    program.post_order_blocks = Shader::IR::PostOrder(program.blocks.front());
}

// ISS_SHADER_PASSES=1: milliseconds per step of TranslateProgram (averaged, printed every 64).
namespace {
struct PassTimes {
    static constexpr u32 Max = 32;
    std::array<std::atomic<u64>, Max> ns{};
    std::array<const char*, Max> names{};
    std::atomic<u64> programs{};
};
PassTimes g_pass_times;
bool PassTimingEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("ISS_SHADER_PASSES");
        return value && value[0] == '1';
    }();
    return enabled;
}
struct PassClock {
    bool on = PassTimingEnabled();
    u64 last = on ? Common::Perf::NowNs() : 0;
    u32 index = 0;
    void Mark(const char* name) {
        if (!on) {
            return;
        }
        const u64 now = Common::Perf::NowNs();
        if (index < PassTimes::Max) {
            g_pass_times.names[index] = name;
            g_pass_times.ns[index].fetch_add(now - last, std::memory_order_relaxed);
        }
        ++index;
        last = now;
    }
    ~PassClock() {
        if (!on) {
            return;
        }
        const u64 n = g_pass_times.programs.fetch_add(1) + 1;
        if ((n & 63) != 0) {
            return;
        }
        std::string line = fmt::format("SHADER_PASSES n={}", n);
        for (u32 i = 0; i < PassTimes::Max && g_pass_times.names[i]; ++i) {
            line += fmt::format(" {}={:.3f}", g_pass_times.names[i],
                                double(g_pass_times.ns[i].load()) / 1e6 / double(n));
        }
        std::fputs(line.c_str(), stderr);
        std::fputc(10, stderr);
    }
};
} // namespace

IR::Program TranslateProgram(const std::span<const u32>& code, Pools& pools, Info& info,
                             RuntimeInfo& runtime_info, const Profile& profile) {
    PassClock clock;
    // Ensure first instruction is expected.
    constexpr u32 token_mov_vcchi = 0xBEEB03FF;
    if (code[0] != token_mov_vcchi) {
        LOG_WARNING(Render_Recompiler, "First instruction is not s_mov_b32 vcc_hi, #imm");
    }

    Gcn::GcnCodeSlice slice(code.data(), code.data() + code.size());
    Gcn::GcnDecodeContext decoder;

    // Decode and save instructions
    IR::Program program{info};
    program.ins_list.reserve(code.size());
    while (!slice.atEnd()) {
        program.ins_list.emplace_back(decoder.decodeInstruction(slice));
    }
    clock.Mark("decode");

    // Clear any previous pooled data.
    pools.ReleaseContents();

    // Create control flow graph
    Common::ObjectPool<Gcn::Block> gcn_block_pool{64};
    Gcn::CFG cfg{gcn_block_pool, program.ins_list};
    EmitControlFlowGraph(program, pools, cfg, runtime_info, profile);
    clock.Mark("cfg_emit");

    // On NVIDIA GPUs HW interpolation of clip distance values seems broken, and we need to emulate
    // it with expensive discard in PS.
    Shader::InjectClipDistanceAttributes(program, runtime_info);
    clock.Mark("clipdist");

    // Run optimization passes on unstructured graph
    if (!profile.support_float64) {
        Shader::Optimization::LowerFp64ToFp32(program);
    }
    Shader::Optimization::SsaRewritePass(program);
    Shader::Optimization::ConstantPropagationPass(program.post_order_blocks);
    clock.Mark("ssa_cp1");
    Shader::Optimization::ReadLaneEliminationPass(program);
    clock.Mark("readlane");
    if (info.sw_stage == SwStage::TessellationControl) {
        Shader::Optimization::TessellationPreprocess(program, runtime_info);
        Shader::Optimization::HullShaderTransform(program, runtime_info);
    } else if (info.sw_stage == SwStage::TessellationEval) {
        Shader::Optimization::TessellationPreprocess(program, runtime_info);
        Shader::Optimization::DomainShaderTransform(program, runtime_info);
    }
    Shader::Optimization::RingAccessElimination(program, runtime_info);
    clock.Mark("ring");
    Shader::IR::DumpProgram(program, info, "pre-res-discover.");
    auto resources = Shader::Optimization::ResourceDiscoverPass(program, profile);
    clock.Mark("res_discover");
    Shader::Optimization::FlattenExtendedUserdataPass(program);
    clock.Mark("flatten_ud");
    Shader::IR::DumpProgram(program, info, "pre-res-patch.");
    Shader::Optimization::ResourcePatchingPass(program.info, resources, profile);
    clock.Mark("res_patch");
    Shader::Optimization::LowerBufferFormatToRaw(program);
    clock.Mark("lower_buf");
    Shader::Optimization::SharedMemorySimplifyPass(program, profile);
    Shader::Optimization::SharedMemoryToStoragePass(program, runtime_info, profile);
    clock.Mark("shared_mem");
    Shader::Optimization::LowerUserClipPlanes(program, runtime_info);
    clock.Mark("clip_planes");
    Shader::Optimization::PhiSimplificationPass(program);
    Shader::Optimization::InverseBallotEliminationPass(program);
    clock.Mark("phi_ballot");
    Shader::IR::DumpProgram(program, info, "pre-lower-phi.");

    // Prepare for structurization by clearing flow graph and lowering phis
    for (auto* ir_block : program.blocks) {
        ir_block->imm_predecessors.clear();
        ir_block->imm_successors.clear();
        ir_block->ssa_state.Reset();
    }
    Shader::Optimization::LowerPhisToRegsPass(program);
    clock.Mark("lower_phis");

    // Structurize control flow graph and create program.
    program.syntax_list = Shader::Gcn::BuildASL(pools, cfg, info);
    program.blocks = GenerateBlocks(program.syntax_list);
    program.post_order_blocks = Shader::IR::PostOrder(program.syntax_list.front().data.block);
    clock.Mark("structurize");

    // Run optimization passes on structured graph
    Shader::Optimization::SsaRepairPass(program);
    Shader::Optimization::SsaRewritePass(program);
    clock.Mark("ssa2");
    Shader::Optimization::SharedMemoryBarrierPass(program, runtime_info, profile);
    Shader::Optimization::WaveMemoryBarrierPass(program, runtime_info, profile);
    clock.Mark("barriers");
    Shader::Optimization::DeadCodeEliminationPass(program);
    Shader::Optimization::LowerWave64BallotPass(program, runtime_info, profile);
    ClampReadLanesToSubgroup(program, profile);
    GuardLaneReadsToActiveLanes(program);
    LowerBitFieldInsert64(program);
    Shader::Optimization::LowerHardwareIntrinsics(program);
    clock.Mark("dce_wave_intrin");
    Shader::Optimization::ConstantPropagationPass(program.post_order_blocks);
    Shader::Optimization::DeadCodeEliminationPass(program);
    Shader::Optimization::CollectShaderInfoPass(program, profile);
    clock.Mark("cp2_dce_collect");
    Shader::IR::DumpProgram(program, info);

    return program;
}

} // namespace Shader
