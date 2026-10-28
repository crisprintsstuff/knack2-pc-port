// SPDX-License-Identifier: GPL-2.0-or-later
#include "geometry.h"
#include <array>
#include <cstring>

namespace iss::native {
namespace {
template<class T> T Load(const void* p, std::size_t offset) {
    T value;
    std::memcpy(&value, static_cast<const std::uint8_t*>(p) + offset, sizeof(value));
    return value;
}
const std::uint8_t* Bytes(const void* p) { return static_cast<const std::uint8_t*>(p); }
}

bool EmitIndexedGeometry(const GeometryBindings& b, const void* material, void* context,
                         const void* object) {
    const auto* geometry = Load<const void*>(Load<const void*>(object, 0x18), 0);
    const auto* deformation = Load<const void*>(object, 0x20);
    const auto count = Load<std::uint8_t>(geometry, 0x6c);
    const void* vertices = Load<const void*>(geometry, 0x60);
    alignas(16) std::array<std::uint8_t, 512> scratch;
    if (deformation && (Load<std::uint8_t>(deformation, 0) & 7)) {
        const auto* slots = Load<const void*>(Load<const void*>(deformation, 8), 0x58);
        const std::array<std::int8_t, 3> indices{
            Load<std::int8_t>(slots, 0), Load<std::int8_t>(slots, 1),
            Load<std::int8_t>(slots, 2)};
        if (count > 32 || indices[0] < 0) return false;
        for (auto index : indices) if (index >= 0 && index >= count) return false;
        std::memcpy(scratch.data(), vertices, std::size_t(count) * 16);
        for (std::size_t i = 0; i < indices.size(); ++i) {
            if (indices[i] >= 0) {
                std::memcpy(scratch.data() + std::size_t(indices[i]) * 16,
                            Bytes(deformation) + 0x28 + i * 16, 16);
            }
        }
        vertices = scratch.data();
    }
    auto* bindings = static_cast<std::uint8_t*>(context) + 0x128;
    auto* commands = static_cast<std::uint8_t*>(context) + 8;
    b.vertex_buffers(bindings, 2, 0, count, vertices);

    const auto instance_slot = Load<std::uint8_t>(material, 0xd8);
    if (instance_slot != 0xff) {
        const auto* data = deformation ? Bytes(deformation) + 0x58
            : reinterpret_cast<const std::uint8_t*>(*b.default_resource_base) + 0x1f6c;
        b.instance_data(bindings, 2, instance_slot, 1, data);
    }
    if (Load<std::uint8_t>(Load<const void*>(material, 0xf0), 0xf) & 2) {
        const auto* layout = Load<const void*>(Load<const void*>(material, 0x10), 0x58);
        const auto slot = static_cast<std::uint32_t>(
            static_cast<std::int32_t>(Load<std::int8_t>(layout, 0x22)));
        const auto* data = deformation && Load<std::uintptr_t>(deformation, 0x20)
            ? Bytes(deformation) + 0xc0
            : reinterpret_cast<const std::uint8_t*>(*b.default_resource_base) + 0x1f6c;
        b.vertex_buffers(bindings, 2, slot, 1, data);
    }
    b.primitive(commands, 4);
    b.index_type(commands, 0, 2);
    if (*b.index_marker_enabled) {
        b.index_marker(commands, Load<std::uint32_t>(geometry, 0x58) << 16);
    }
    // Capture these before the binding flush, matching the original instruction order.
    const auto index_count = Load<std::uint32_t>(geometry, 0x68);
    const auto index_address = Load<std::uint64_t>(geometry, 0x58);
    b.flush_bindings(bindings);
    b.draw_indexed(commands, index_count, index_address, 0);
    return true;
}
}
