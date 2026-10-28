// SPDX-License-Identifier: GPL-2.0-or-later
#include "../gpu/texture_lookup_memo.h"
#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "Texture memo failure line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)
struct Key {
    std::uint64_t address{}, revision{};
    bool operator==(const Key&) const = default;
};
struct ViewKey {
    std::uint64_t uid{}, registry{}, content{}, tick{}, backing{}, mip{}, layer{}, format{}, storage{};
    bool operator==(const ViewKey&) const = default;
};

int main() {
    iss::texture_lookup::Memo<Key, std::optional<unsigned>, 8> memo;
    std::map<std::uint64_t, unsigned> reference;
    std::uint64_t revision = 1;
    std::uint32_t random = 0x56b391d9;
    unsigned hits = 0, misses = 0;
    for (unsigned step = 0; step < 100000; ++step) {
        random = random * 1664525 + 1013904223;
        const auto address = std::uint64_t((random >> 8) % 61) * 256;
        if (step % 11 == 0) {
            ++revision; reference[address] = random % 3; ++revision;
        } else if (step % 13 == 0) {
            ++revision; reference.erase(address); ++revision;
        }
        const auto it = reference.find(address);
        const auto expected = it == reference.end() ? std::optional<unsigned>{}
                                                    : std::optional<unsigned>{it->second};
        const Key key{address, revision};
        const auto* cached = memo.Find(address >> 8, key);
        if (cached) { CHECK(*cached == expected); ++hits; }
        else { memo.Store(address >> 8, key, expected); ++misses; }
        CHECK(memo.Find(address >> 8, key));
        CHECK(*memo.Find(address >> 8, key) == expected);
        CHECK(!memo.Find(address >> 8, {address, revision + 1}));
    }
    CHECK(hits && misses);
    // Negative entries are successful observations, distinct from an empty memo.
    memo.Store(0, {0, revision}, {});
    CHECK(memo.Find(0, {0, revision}) && !memo.Find(0, {0, revision})->has_value());
    memo.Store(0, {0, revision + 2}, 2);
    CHECK(!memo.Find(0, {0, revision}));
    CHECK(*memo.Find(0, {0, revision + 2}) == 2);

    // Deliberate slot reuse/backing and view changes must invalidate an identity.
    iss::texture_lookup::Memo<ViewKey, unsigned, 4> views;
    const ViewKey original{17, 2, 3, 8, 900, 0, 1, 4, 0};
    views.Store(3, original, 42);
    CHECK(views.Find(3, original) && *views.Find(3, original) == 42);
    for (unsigned field = 0; field < 9; ++field) {
        auto changed = original;
        switch (field) {
        case 0: ++changed.uid; break;
        case 1: ++changed.registry; break;
        case 2: ++changed.content; break;
        case 3: ++changed.tick; break;
        case 4: ++changed.backing; break;
        case 5: ++changed.mip; break;
        case 6: ++changed.layer; break;
        case 7: ++changed.format; break;
        case 8: ++changed.storage; break;
        }
        CHECK(!views.Find(3, changed));
    }
    for (unsigned i = 0; i < 1024; ++i) {
        auto changed = original; changed.uid = 1000 + i;
        views.Store(3, changed, i);
        CHECK(!views.Find(3, original));
        CHECK(*views.Find(3, changed) == i);
    }
    std::printf("Texture lookup memo: 100000 differential mutations/queries, %u hits, %u misses; negative entries, slot reuse and view/backing invalidation passed.\n", hits, misses);
}
