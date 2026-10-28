// SPDX-License-Identifier: GPL-2.0-or-later
// Differential test against the complete original function, with only its external calls
// and RIP-relative globals redirected to controlled fixtures.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "../native/geometry.h"
#include <array>
#include <vector>
#include <fstream>
#include <iterator>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"GEOMETRY FAIL %d: %s\n",__LINE__,#x); std::exit(1); } } while (0)
using Byte = unsigned char;
using U32 = std::uint32_t;
using U64 = std::uint64_t;
template<class T> void Put(void* p, std::size_t offset, T value) {
    std::memcpy(static_cast<Byte*>(p) + offset, &value, sizeof(value));
}
static std::vector<Byte> events;
static Byte* context;
static Byte* geometry;
static bool mutate_flush;
template<class T> void Event(T value) {
    const auto* p = reinterpret_cast<const Byte*>(&value);
    events.insert(events.end(), p, p + sizeof(value));
}
static void ISS_SYSV Buffers(void* p, U32 stage, U32 slot, U32 count, const void* data) {
    CHECK(p == context + 0x128); Event(U32{1}); Event(stage); Event(slot); Event(count);
    auto* bytes = static_cast<const Byte*>(data);
    events.insert(events.end(), bytes, bytes + std::size_t(count) * 16);
}
static void ISS_SYSV Instance(void* p, U32 stage, U32 slot, U32 count, const void* data) {
    CHECK(p == context + 0x128); Event(U32{2}); Event(stage); Event(slot); Event(count);
    auto* bytes = static_cast<const Byte*>(data);
    events.insert(events.end(), bytes, bytes + 16);
}
static void ISS_SYSV Primitive(void* p, U32 type) {
    CHECK(p == context + 8); Event(U32{3}); Event(type);
}
static void ISS_SYSV IndexType(void* p, U32 type, U32 source) {
    CHECK(p == context + 8); Event(U32{4}); Event(type); Event(source);
}
static void ISS_SYSV Marker(void* p, U32 marker) {
    CHECK(p == context + 8); Event(U32{5}); Event(marker);
}
static void ISS_SYSV Flush(void* p) {
    CHECK(p == context + 0x128); Event(U32{6});
    if (mutate_flush) { Put(geometry, 0x58, U64{0xdeadbeef}); Put(geometry, 0x68, U32{77}); }
}
static void ISS_SYSV Draw(void* p, U32 count, U64 address, U32 flags) {
    CHECK(p == context + 8); Event(U32{7}); Event(count); Event(address); Event(flags);
}
static void* ISS_SYSV Copy(void* dst, const void* src, std::size_t n) { return std::memcpy(dst, src, n); }
static void ISS_SYSV Fail() { CHECK(false); }
static U64 Hash(const std::vector<Byte>& data) {
    U64 h = 14695981039346656037ULL;
    for (auto b : data) h = (h ^ b) * 1099511628211ULL;
    return h;
}
int main() {
    std::ifstream file("geometry-original.bin", std::ios::binary);
    std::vector<Byte> original((std::istreambuf_iterator<char>(file)), {});
    CHECK(original.size() == 549 && Hash(original) == 0xea1442f922a0a0ecULL);
    auto* code = static_cast<Byte*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    CHECK(code); std::memcpy(code, original.data(), original.size());
    const auto relative = [&](std::size_t disp, std::size_t next, const void* target) {
        const auto delta = reinterpret_cast<std::intptr_t>(target) - reinterpret_cast<std::intptr_t>(code + next);
        CHECK(delta >= INT32_MIN && delta <= INT32_MAX);
        Put(code, disp, static_cast<std::int32_t>(delta));
    };
    const auto call = [&](std::size_t at, const void* target, std::size_t stub) {
        CHECK(code[at] == 0xe8); relative(at + 1, at + 5, code + stub);
        const Byte jump[]{0xff,0x25,0,0,0,0}; std::memcpy(code + stub, jump, 6);
        Put(code, stub + 6, reinterpret_cast<std::uintptr_t>(target));
    };
    call(0x67, reinterpret_cast<void*>(&Copy), 0x700);
    call(0xf3, reinterpret_cast<void*>(&Buffers), 0x710);
    call(0x134, reinterpret_cast<void*>(&Instance), 0x720);
    call(0x194, reinterpret_cast<void*>(&Buffers), 0x730);
    call(0x1a5, reinterpret_cast<void*>(&Primitive), 0x740);
    call(0x1b4, reinterpret_cast<void*>(&IndexType), 0x750);
    call(0x1d9, reinterpret_cast<void*>(&Marker), 0x760);
    call(0x1e9, reinterpret_cast<void*>(&Flush), 0x770);
    call(0x1f9, reinterpret_cast<void*>(&Draw), 0x780);
    call(0x220, reinterpret_cast<void*>(&Fail), 0x790);
    relative(0x17, 0x1b, code + 0x608);
    relative(0x201, 0x205, code + 0x608);
    Put(code, 0x608, reinterpret_cast<std::uintptr_t>(code + 0x610));
    Put(code, 0x610, U64{0x1234abcdef});
    for (auto pair : {std::pair{0x129,0x12d}, std::pair{0x17a,0x17e}, std::pair{0x183,0x187}})
        relative(pair.first, pair.second, code + 0x600);
    // Original LEA establishes the base for CMP [base + 0xdf0f05].
    relative(0x1bc, 0x1c0, reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(code + 0x500) - 0xdf0f05));
    DWORD old; CHECK(VirtualProtect(code, 4096, PAGE_EXECUTE_READWRITE, &old));
    CHECK(FlushInstructionCache(GetCurrentProcess(), code, 4096));
    using Oracle = void (ISS_SYSV *)(const void*, void*, const void*);
    const auto oracle = reinterpret_cast<Oracle>(code);

    std::array<Byte, 0x100> material{}, geom{}, deformation{}, deformation_layout{}, extra_layout{}, flags{};
    std::array<Byte, 0x40> object{};
    std::array<Byte, 0x300> ctx{};
    std::array<Byte, 512> vertex_data{};
    std::array<Byte, 0x2000> defaults{};
    for (std::size_t i = 0; i < vertex_data.size(); ++i) vertex_data[i] = Byte(i * 71 + 17);
    for (std::size_t i = 0; i < deformation.size(); ++i) deformation[i] = Byte(i * 11 + 33);
    for (std::size_t i = 0; i < defaults.size(); ++i) defaults[i] = Byte(i * 3 + 7);
    context = ctx.data(); geometry = geom.data();
    const void* geom_ptr = geom.data();
    Put(object.data(), 0x18, &geom_ptr);
    Put(geom.data(), 0x60, vertex_data.data());
    Put(material.data(), 0xf0, flags.data());
    Put(material.data(), 0x10, extra_layout.data());
    Put(extra_layout.data(), 0x58, extra_layout.data());
    Put(deformation.data(), 8, deformation_layout.data());
    Put(deformation_layout.data(), 0x58, deformation_layout.data());
    Put(code, 0x600, reinterpret_cast<std::uintptr_t>(defaults.data()));
    iss::native::GeometryBindings b{reinterpret_cast<const std::uintptr_t*>(code + 0x600),
        code + 0x500, Buffers, Instance, Primitive, IndexType, Marker, Flush, Draw};
    unsigned cases = 0;
    for (unsigned count : {1u,3u,16u,32u}) for (unsigned mode = 0; mode < 9; ++mode)
    for (unsigned inst : {0u,128u,254u,255u}) for (unsigned extra = 0; extra < 2; ++extra)
    for (unsigned marker = 0; marker < 2; ++marker) for (unsigned pattern = 0; pattern < 3; ++pattern)
    for (unsigned mutation = 0; mutation < 2; ++mutation) {
        Put(geom.data(), 0x6c, Byte(count));
        Put(object.data(), 0x20, mode == 0 ? static_cast<void*>(nullptr) : deformation.data());
        deformation[0] = Byte(mode == 0 ? 0 : mode - 1);
        deformation_layout[0] = Byte(count - 1);
        deformation_layout[1] = pattern == 0 ? 0xff : Byte(0);
        deformation_layout[2] = pattern == 2 ? Byte(count - 1) : Byte(0xff);
        Put(deformation.data(), 0x20, U64(pattern ? 0x100 : 0));
        material[0xd8] = Byte(inst); flags[0xf] = Byte(extra << 1);
        extra_layout[0x22] = pattern == 0 ? Byte(0xff) : Byte(31);
        code[0x500] = Byte(marker); mutate_flush = mutation != 0;
        const U64 address = 0x26abcdef00ULL + cases * 16;
        Put(geom.data(), 0x58, address); Put(geom.data(), 0x68, U32(cases * 3));
        events.clear(); oracle(material.data(), ctx.data(), object.data());
        const auto expected = events;
        Put(geom.data(), 0x58, address); Put(geom.data(), 0x68, U32(cases * 3));
        events.clear(); CHECK(iss::native::EmitIndexedGeometry(b, material.data(), ctx.data(), object.data()));
        CHECK(events == expected); ++cases;
    }
    // Unsupported scratch layouts fail before producing any side effect.
    deformation[0] = 1; Put(object.data(), 0x20, deformation.data());
    for (auto bad_count : {0u,33u,255u}) {
        Put(geom.data(), 0x6c, Byte(bad_count)); deformation_layout[0] = 0;
        events.clear(); CHECK(!iss::native::EmitIndexedGeometry(b, material.data(), ctx.data(), object.data()));
        CHECK(events.empty());
    }
    // Exercise the installed SysV entry and the original fallback trampoline.
    constexpr std::size_t image_size = 0x1d0d000;
    auto* image = static_cast<Byte*>(VirtualAlloc(nullptr, image_size,
        MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    CHECK(image);
    std::memcpy(image + 0x1a1960, original.data(), original.size());
    const auto external = [&](std::size_t offset, const void* target) {
        const Byte jump[]{0xff,0x25,0,0,0,0}; std::memcpy(image + offset, jump, 6);
        Put(image, offset + 6, reinterpret_cast<std::uintptr_t>(target));
    };
    external(0x984430, reinterpret_cast<void*>(&Copy));
    external(0x984360, reinterpret_cast<void*>(&Fail));
    external(0x16aa0, reinterpret_cast<void*>(&Buffers));
    external(0x16b70, reinterpret_cast<void*>(&Instance));
    external(0x623740, reinterpret_cast<void*>(&Primitive));
    external(0x624f80, reinterpret_cast<void*>(&IndexType));
    external(0x628ae0, reinterpret_cast<void*>(&Marker));
    external(0x14ad0, reinterpret_cast<void*>(&Flush));
    external(0x623c60, reinterpret_cast<void*>(&Draw));
    Put(image, 0xd9dec8, reinterpret_cast<std::uintptr_t>(defaults.data()));
    Put(image, 0xd9de00, reinterpret_cast<std::uintptr_t>(code + 0x610));
    CHECK(_putenv_s("ISS_NATIVE_GEOMETRY", "1") == 0);
    CHECK(_putenv_s("ISS_NATIVE_GEOMETRY_STATS", "1") == 0);
    image[0x1a1960] ^= 1;
    CHECK(iss::native::InstallIndexedGeometry(image, image_size) == -1);
    image[0x1a1960] ^= 1;
    CHECK(iss::native::InstallIndexedGeometry(image, image_size) == 1);
    CHECK(FlushInstructionCache(GetCurrentProcess(), image, image_size));
    auto installed = reinterpret_cast<Oracle>(image + 0x1a1960);
    for (bool fallback : {false, true}) {
        Put(geom.data(), 0x6c, Byte{1});
        deformation_layout[0] = fallback ? Byte{2} : Byte{0};
        deformation_layout[1] = 0xff; deformation_layout[2] = 0xff;
        code[0x500] = image[0x1d0cbd5] = 1;
        Put(geom.data(), 0x58, U64{0x264f001122}); Put(geom.data(), 0x68, U32{123});
        events.clear(); oracle(material.data(), ctx.data(), object.data());
        const auto expected = events;
        Put(geom.data(), 0x58, U64{0x264f001122}); Put(geom.data(), 0x68, U32{123});
        events.clear(); installed(material.data(), ctx.data(), object.data());
        CHECK(events == expected);
    }
    CHECK(VirtualFree(image, 0, MEM_RELEASE));
    CHECK(VirtualFree(code, 0, MEM_RELEASE));
    std::printf("PASS: %u full original-machine-code geometry parity cases; hook identity, SysV entry and original fallback.\n", cases);
}
