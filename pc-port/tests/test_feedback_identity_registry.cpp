#include <cassert>
#include <cstdint>
#include <thread>
#include <vector>

#include "infamous_renderer/feedback_identity_registry.h"

int main() {
    auto& registry = iss::game::feedback::IdentityRegistry::Instance();
    registry.Publish(1, 2, 0);
    assert(!registry.Latest(0));

    constexpr std::uint64_t address = 0x206258300ULL;
    registry.Publish(0x100, 0x200, address);
    auto observed = registry.Latest(address);
    assert(observed && observed->owner_object == 0x100);
    assert(observed->shader_object == 0x200 && observed->address_reuses == 0);
    registry.Publish(0x300, 0x400, address);
    observed = registry.Latest(address);
    assert(observed && observed->owner_object == 0x300);
    assert(observed->address_reuses == 1);

    // A direct-map collision must produce a miss for the evicted address.
    std::uint64_t collision{};
    for (std::uint64_t offset = 256; offset < (1ULL << 24); offset += 256) {
        const auto candidate = address + offset;
        registry.Publish(0x500, 0x600, candidate);
        if (!registry.Latest(address)) {
            collision = candidate;
            break;
        }
    }
    assert(collision != 0);
    assert(!registry.Latest(address));
    assert(registry.Latest(collision));
    registry.Invalidate(collision - 4, 8);
    assert(!registry.Latest(collision));

    std::vector<std::thread> workers;
    for (std::uint64_t i = 0; i < 8; ++i) {
        workers.emplace_back([&, i] {
            const auto addr = 0x400000000ULL + (i << 8);
            for (std::uint64_t n = 0; n < 100; ++n) {
                registry.Publish(i + 1, n + 1, addr);
                const auto latest = registry.Latest(addr);
                assert(latest && latest->record_address == addr);
            }
        });
    }
    for (auto& worker : workers) worker.join();
}
