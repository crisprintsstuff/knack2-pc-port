// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cstdint>
#include <vector>

#include "video_core/buffer_cache/upload_parts.h"

struct Range {
    std::uint64_t dstOffset;
    std::uint64_t size;
};

void CheckParts(const std::vector<Range>& ranges, std::uint64_t limit) {
    std::uint64_t total = 0;
    for (const auto& range : ranges) total += range.size;

    std::uint64_t copied = 0;
    std::uint64_t used = 0;
    std::size_t source = 0;
    std::uint64_t source_offset = 0;
    std::uint64_t parts = 0;
    VideoCore::ForEachUploadPart(
        ranges, limit,
        [&] { assert(used == 0); },
        [&](std::uint64_t address, std::uint64_t offset, std::uint64_t bytes) {
            assert(source < ranges.size());
            assert(address == ranges[source].dstOffset + source_offset);
            assert(offset == used && bytes > 0 && used + bytes <= limit);
            used += bytes;
            copied += bytes;
            source_offset += bytes;
            if (source_offset == ranges[source].size) {
                ++source;
                source_offset = 0;
            }
        },
        [&](std::uint64_t bytes) {
            assert(bytes == used && bytes <= limit);
            ++parts;
            used = 0;
        });
    assert(copied == total && used == 0 && source == ranges.size());
    assert(parts == (total + limit - 1) / limit);
}

int main() {
    constexpr std::uint64_t MiB = 1024 * 1024;
    CheckParts({{0x1000, 2'619'408'384}, {0xA000'0000, 536'436'736}}, 256 * MiB);
    CheckParts({{0x1000, 256 * MiB}, {0x2000'0000, 256 * MiB}}, 256 * MiB);
    CheckParts({{0x1000, 1}, {0x2000, 256 * MiB + 1}}, 256 * MiB);
}
