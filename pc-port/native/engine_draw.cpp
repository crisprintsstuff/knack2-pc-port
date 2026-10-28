// SPDX-License-Identifier: GPL-2.0-or-later
// ISS_ENGINE_MARKERS=1: the engine's draw routines (CUSA00004) tell the command processor what
// they draw. Each hooked routine writes a NOP packet carrying the engine objects (pass
// technique, material, geometry, draw item) into the command buffer, then runs unchanged. The
// native renderer keys its prepared draws by these objects instead of the decoded commands.
//
// Routines (all `push rbp; mov rbp,rsp; push r15..r12; push rbx` prologues):
//   0x1a1960 DrawItem(technique, context, item)                 item+0x18 -> {geometry, material}
//   0x1a1770 DrawInstanced(technique, context, instances, count, geometry)   main passes
//   0x43d480 DrawInstancedDepth(technique, context, instances, count, geometry)  depth passes
//   0x448640 BindMaterial(technique, context, material)
// The context holds the draw command buffer at +8: {+8 end, +0x10 cursor, +0x18 reserve
// callback(buffer, dwords, user), +0x20 user}.
#include "geometry.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <map>

namespace {
constexpr std::uint32_t MarkerMagic = 0x49535300u; // 'ISS' and the marker kind
constexpr std::uint32_t MarkerDwords = 10;         // header and nine payload dwords
enum MarkerKind : std::uint32_t { Item = 1, Instanced = 2, InstancedDepth = 3, Material = 4, ItemEnd = 5 };
bool item_ranges{};
bool material_resource_census{};
unsigned char* engine_image{};
std::uint32_t material_start_frame{1200};

struct CommandBuffer {
    std::uint64_t reserved;
    std::uint32_t* end;
    std::uint32_t* cursor;
    bool (ISS_SYSV* reserve)(CommandBuffer*, std::uint32_t, void*);
    void* user;
};

void Emit(void* context, std::uint32_t kind, std::uint64_t a, std::uint64_t b, std::uint64_t c,
          std::uint64_t d) {
    auto* dcb = reinterpret_cast<CommandBuffer*>(static_cast<unsigned char*>(context) + 8);
    if (static_cast<std::uint64_t>(dcb->end - dcb->cursor) < MarkerDwords &&
        !dcb->reserve(dcb, MarkerDwords, dcb->user)) {
        return;
    }
    std::uint32_t* p = dcb->cursor;
    // Type 3 NOP (opcode 0x10), nine payload dwords.
    p[0] = (3u << 30) | ((MarkerDwords - 2) << 16) | (0x10u << 8);
    p[1] = MarkerMagic | kind;
    std::memcpy(p + 2, &a, 8);
    std::memcpy(p + 4, &b, 8);
    std::memcpy(p + 6, &c, 8);
    std::memcpy(p + 8, &d, 8);
    dcb->cursor = p + MarkerDwords;
}

using Draw3 = void(ISS_SYSV*)(std::uint64_t, std::uint64_t, std::uint64_t);
using Draw5 = void(ISS_SYSV*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint32_t,
                              std::uint64_t);
Draw3 original_item{}, original_material{};
Draw5 original_instanced{}, original_depth{};
using RenderJobFn = std::uint64_t(ISS_SYSV*)(std::uint64_t, std::uint64_t, double);
RenderJobFn original_render_job{};
unsigned char* render_job_image{};
std::uint32_t render_job_start_frame{1200};

std::uint64_t HashBytes(std::uint64_t hash, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i)
        hash = (hash ^ bytes[i]) * 0x100000001b3ull;
    return hash;
}

void NoteMaterialResources(std::uint64_t technique, std::uint64_t material) {
    if (!material_resource_census || !material || !engine_image) return;
    const auto frame = *reinterpret_cast<const std::uint32_t*>(engine_image + 0x1d20384);
    if (frame < material_start_frame || frame - material_start_frame >= 3) return;
    const auto* block = *reinterpret_cast<const unsigned char* const*>(material + 0x28);
    if (!block) return;
    const auto image_count = *reinterpret_cast<const std::uint32_t*>(technique + 0x6c);
    const auto sampler_count = *reinterpret_cast<const std::uint32_t*>(technique + 0x80);
    const auto sampler_base = *reinterpret_cast<const std::int32_t*>(technique + 0x84);
    if (image_count > 128 || sampler_count > 128 || sampler_base < 0 || sampler_base > 1024) {
        std::fprintf(stderr, "MATERIAL_BIND frame=%u technique=0x%llx material=0x%llx rejected_counts=%u/%u\n",
                     frame, (unsigned long long)technique, (unsigned long long)material,
                     image_count, sampler_count);
        return;
    }
    std::uint64_t hash = HashBytes(0xcbf29ce484222325ull, block, 16);
    for (std::uint32_t i = 0; i < image_count; ++i) {
        const auto* entry = reinterpret_cast<const unsigned char*>(technique + 0x2c + 4ull * i);
        const auto index = *reinterpret_cast<const std::uint16_t*>(entry + 2);
        if (index > 1024) {
            std::fprintf(stderr, "MATERIAL_BIND frame=%u technique=0x%llx material=0x%llx rejected_image_index=%u\n",
                         frame, (unsigned long long)technique, (unsigned long long)material, index);
            return;
        }
        hash = HashBytes(hash, entry, 4);
        hash = HashBytes(hash, block + 32ull + 32ull * index, 32);
    }
    for (std::uint32_t i = 0; i < sampler_count; ++i) {
        const auto* entry = reinterpret_cast<const unsigned char*>(technique + 0x70 + 4ull * i);
        const auto index = *reinterpret_cast<const std::uint16_t*>(entry + 2);
        if (index > 1024) {
            std::fprintf(stderr, "MATERIAL_BIND frame=%u technique=0x%llx material=0x%llx rejected_sampler_index=%u\n",
                         frame, (unsigned long long)technique, (unsigned long long)material, index);
            return;
        }
        hash = HashBytes(hash, entry, 4);
        const auto offset = 32ll + 32ll * sampler_base + 16ll * index;
        hash = HashBytes(hash, block + offset, 16);
    }
    std::fprintf(stderr,
                 "MATERIAL_BIND frame=%u technique=0x%llx material=0x%llx images=%u samplers=%u sharp_hash=0x%llx\n",
                 frame, (unsigned long long)technique, (unsigned long long)material,
                 image_count, sampler_count, (unsigned long long)hash);
}

void ISS_SYSV DrawItem(std::uint64_t technique, std::uint64_t context, std::uint64_t item) {
    const auto* part = *reinterpret_cast<const std::uint64_t* const*>(item + 0x18);
    const auto geometry = part[0], material = part[1];
    Emit(reinterpret_cast<void*>(context), Item, technique, geometry, material, item);
    original_item(technique, context, item);
    if (item_ranges) Emit(reinterpret_cast<void*>(context), ItemEnd, technique, geometry, material, item);
}
void ISS_SYSV DrawInstanced(std::uint64_t technique, std::uint64_t context,
                            std::uint64_t instances, std::uint32_t count, std::uint64_t geometry) {
    Emit(reinterpret_cast<void*>(context), Instanced, technique, geometry, instances, count);
    original_instanced(technique, context, instances, count, geometry);
}
void ISS_SYSV DrawInstancedDepth(std::uint64_t technique, std::uint64_t context,
                                 std::uint64_t instances, std::uint32_t count,
                                 std::uint64_t geometry) {
    Emit(reinterpret_cast<void*>(context), InstancedDepth, technique, geometry, instances, count);
    original_depth(technique, context, instances, count, geometry);
}
void ISS_SYSV BindMaterial(std::uint64_t technique, std::uint64_t context, std::uint64_t material) {
    NoteMaterialResources(technique, material);
    Emit(reinterpret_cast<void*>(context), Material, technique, material, 0, 0);
    original_material(technique, context, material);
}

std::uint64_t ISS_SYSV RenderJob(std::uint64_t context, std::uint64_t token, double xmm0) {
    const auto frame = *reinterpret_cast<const std::uint32_t*>(render_job_image + 0x1d20384);
    if (frame < render_job_start_frame || frame - render_job_start_frame >= 3)
        return original_render_job(context, token, xmm0);
    const auto index = *reinterpret_cast<const std::uint32_t*>(context + 0xde90);
    const auto list = *reinterpret_cast<const std::uint64_t*>(context + 0xdea0);
    if (index >= 41 || list == 0)
        return original_render_job(context, token, xmm0);
    const auto bounds = *reinterpret_cast<const std::uint64_t*>(list + 0x4dcc + 8 * index);
    const auto lo = static_cast<std::uint32_t>(bounds);
    const auto hi = static_cast<std::uint32_t>(bounds >> 32);
    std::map<std::uint64_t, std::uint32_t> vtables;
    if (hi >= lo && hi - lo <= 20000) {
        const auto items = *reinterpret_cast<const std::uint64_t*>(list + 0x4db8);
        const auto keys = *reinterpret_cast<const std::uint64_t*>(list + 0x4dc0);
        if (items && keys) {
            for (auto i = lo; i < hi; ++i) {
                const auto key = *reinterpret_cast<const std::uint64_t*>(keys + 8ull * i);
                const auto item = items + ((key & 0xffffu) << 7);
                const auto technique = *reinterpret_cast<const std::uint64_t*>(item + 8);
                if (technique)
                    ++vtables[*reinterpret_cast<const std::uint64_t*>(technique) -
                              reinterpret_cast<std::uint64_t>(render_job_image)];
            }
        }
    }
    auto* dcb = reinterpret_cast<const CommandBuffer*>(context + 8);
    const auto before = reinterpret_cast<std::uintptr_t>(dcb->cursor);
    const auto result = original_render_job(context, token, xmm0);
    const auto after = reinterpret_cast<std::uintptr_t>(dcb->cursor);
    std::fprintf(stderr,
                 "RENDER_JOB frame=%u id=%u lo=%u hi=%u thread=%lu dcb_before=0x%llx dcb_after=0x%llx\n",
                 frame, index, lo, hi, GetCurrentThreadId(),
                 static_cast<unsigned long long>(before),
                 static_cast<unsigned long long>(after));
    for (const auto& [vtable, count] : vtables)
        std::fprintf(stderr, "RENDER_JOB_VTABLE frame=%u id=%u vtable=0x%llx items=%u\n",
                     frame, index, static_cast<unsigned long long>(vtable), count);
    return result;
}

// The 13 prologue bytes run from a trampoline that jumps back after them; the entry becomes
// `movabs rax, hook; jmp rax` (RAX carries no argument in these routines).
constexpr unsigned char Prologue[13] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41,
                                        0x56, 0x41, 0x55, 0x41, 0x54, 0x53};
unsigned char* trampolines{};

void* Hook(unsigned char* image, std::size_t entry, std::uintptr_t hook, std::size_t slot) {
    unsigned char* code = image + entry;
    if (std::memcmp(code, Prologue, sizeof(Prologue)) != 0) {
        std::fprintf(stderr, "ENGINE_MARKERS rejected: eboot+0x%zx prologue mismatch\n", entry);
        return nullptr;
    }
    unsigned char* t = trampolines + slot * 32;
    std::memcpy(t, Prologue, sizeof(Prologue));
    const auto back = reinterpret_cast<std::uintptr_t>(code + sizeof(Prologue));
    const unsigned char jump_back[6] = {0xff, 0x25, 0, 0, 0, 0};
    std::memcpy(t + 13, jump_back, 6);
    std::memcpy(t + 19, &back, 8);
    unsigned char patch[13] = {0x48, 0xb8};
    const auto target = hook;
    std::memcpy(patch + 2, &target, 8);
    patch[10] = 0xff;
    patch[11] = 0xe0;
    patch[12] = 0x90;
    DWORD old, ignored;
    if (!VirtualProtect(code, sizeof(patch), PAGE_EXECUTE_READWRITE, &old)) return nullptr;
    std::memcpy(code, patch, sizeof(patch));
    if (!VirtualProtect(code, sizeof(patch), old, &ignored) ||
        !FlushInstructionCache(GetCurrentProcess(), code, sizeof(patch))) {
        return nullptr;
    }
    return t;
}
} // namespace

namespace iss::native {
int InstallEngineMarkers(unsigned char* image, std::size_t image_size) {
    const char* enabled = std::getenv("ISS_ENGINE_MARKERS");
    if (!enabled || enabled[0] != '1') return 0;
    const char* ranges = std::getenv("ISS_NATIVE_ITEM_RANGES");
    item_ranges = ranges && std::strcmp(ranges, "1") == 0;
    const char* material_flag = std::getenv("ISS_MATERIAL_RESOURCE_CENSUS");
    material_resource_census = material_flag && material_flag[0] == '1';
    if (const char* start = std::getenv("ISS_MATERIAL_RESOURCE_CENSUS_START_FRAME"))
        material_start_frame = static_cast<std::uint32_t>(std::strtoul(start, nullptr, 10));
    engine_image = image;
    if (image_size <= 0x448640 + 16) return -1;
    trampolines = static_cast<unsigned char*>(
        VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampolines) return -1;
    // All identities are checked before the first patch.
    for (const std::size_t entry : {0x1a1960u, 0x1a1770u, 0x43d480u, 0x448640u}) {
        if (std::memcmp(image + entry, Prologue, sizeof(Prologue)) != 0) {
            std::fprintf(stderr, "ENGINE_MARKERS rejected: eboot+0x%zx prologue mismatch\n",
                         entry);
            return -1;
        }
    }
    original_item = reinterpret_cast<Draw3>(Hook(image, 0x1a1960, reinterpret_cast<std::uintptr_t>(&DrawItem), 0));
    original_instanced = reinterpret_cast<Draw5>(Hook(image, 0x1a1770, reinterpret_cast<std::uintptr_t>(&DrawInstanced), 1));
    original_depth = reinterpret_cast<Draw5>(Hook(image, 0x43d480, reinterpret_cast<std::uintptr_t>(&DrawInstancedDepth), 2));
    original_material = reinterpret_cast<Draw3>(Hook(image, 0x448640, reinterpret_cast<std::uintptr_t>(&BindMaterial), 3));
    if (!original_item || !original_instanced || !original_depth || !original_material) {
        return -1;
    }
    std::fputs("ENGINE_MARKERS installed at eboot+0x1a1960/0x1a1770/0x43d480/0x448640\n", stderr);
    return 1;
}
int InstallRenderJobCensus(unsigned char* image, std::size_t image_size) {
    const char* enabled = std::getenv("ISS_RENDER_JOB_CENSUS");
    if (!enabled || enabled[0] != '1') return 0;
    if (image_size <= 0x1d20384 + sizeof(std::uint32_t) ||
        std::memcmp(image + 0x198720, Prologue, sizeof(Prologue)) != 0) {
        std::fputs("RENDER_JOB rejected: eboot identity mismatch\n", stderr);
        return -1;
    }
    if (const char* start = std::getenv("ISS_RENDER_JOB_CENSUS_START_FRAME"))
        render_job_start_frame = static_cast<std::uint32_t>(std::strtoul(start, nullptr, 10));
    if (!trampolines)
        trampolines = static_cast<unsigned char*>(
            VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampolines) return -1;
    render_job_image = image;
    original_render_job = reinterpret_cast<RenderJobFn>(
        Hook(image, 0x198720, reinterpret_cast<std::uintptr_t>(&RenderJob), 4));
    if (!original_render_job) return -1;
    std::fprintf(stderr, "RENDER_JOB installed at eboot+0x198720 start_frame=%u\n",
                 render_job_start_frame);
    return 1;
}
} // namespace iss::native
