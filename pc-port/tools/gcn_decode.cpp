// SPDX-License-Identifier: GPL-2.0-or-later
// Offline inspection of bounded shader extracts using the port's existing decoder.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>
#include "shader_recompiler/frontend/decode.h"

void assert_fail_impl() { std::abort(); }
[[noreturn]] void unreachable_impl() { std::abort(); }
namespace Common::Log {
std::array<Level, NUM_LOG_CLASSES> g_class_levels{};
void VLog(Class, Level, const char* file, int line, const char* function,
          fmt::string_view, fmt::format_args) {
    std::fprintf(stderr, "Decoder diagnostic at %s:%d (%s)\n", file, line, function);
}
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    std::ifstream file(argv[1], std::ios::binary | std::ios::ate);
    if (!file) return 3;
    const auto size = file.tellg();
    if (size <= 0 || size > 65536 || size % 4) return 4;
    // Padding bounds decoder lookahead; instructions crossing the real end are rejected.
    std::vector<u32> words(size_t(size) / 4 + 16);
    file.seekg(0); file.read(reinterpret_cast<char*>(words.data()), size);
    if (!file) return 5;
    Shader::Gcn::GcnDecodeContext decoder;
    Shader::Gcn::GcnCodeSlice code(words.data(), words.data() + size_t(size) / 4);
    u32 pc = 0;
    while (pc < size_t(size)) {
        const auto inst = decoder.decodeInstruction(code);
        if (!inst.length || pc + inst.length > size_t(size)) return 6;
        const auto name = magic_enum::enum_name(inst.opcode);
        std::printf("%04x %-30.*s", pc, int(name.size()), name.data());
        for (u32 i = 0; i < inst.dst_count; ++i) {
            const auto& op = inst.dst[i]; const auto field = magic_enum::enum_name(op.field);
            std::printf(" dst%u=%.*s:%u", i, int(field.size()), field.data(), op.code);
        }
        for (u32 i = 0; i < inst.src_count; ++i) {
            const auto& op = inst.src[i]; const auto field = magic_enum::enum_name(op.field);
            std::printf(" src%u=%.*s:%u", i, int(field.size()), field.data(), op.code);
        }
        if (inst.encoding == Shader::Gcn::InstEncoding::MUBUF) {
            const auto c = inst.control.mubuf;
            std::printf(" [offset=%llu idxen=%llu offen=%llu glc=%llu]", u64(c.offset), u64(c.idxen), u64(c.offen), u64(c.glc));
        }
        if (inst.encoding == Shader::Gcn::InstEncoding::SMRD) {
            const auto c = inst.control.smrd;
            std::printf(" [offset=%u imm=%u]", c.offset, c.imm);
        }
        std::printf(" raw=");
        for (u32 i = 0; i < inst.length / 4; ++i) std::printf("%08x ", words[pc / 4 + i]);
        std::printf("\n");
        pc += inst.length;
        if (inst.opcode == Shader::Gcn::Opcode::S_ENDPGM) return 0;
    }
    return 7; // No terminator: reject an incomplete extract.
}
