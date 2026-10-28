// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <cassert>
#include <cstdint>
#include <vector>

#include "sparse_copy.h"

namespace {
constexpr std::uint64_t Page = 16384;
constexpr std::uint64_t Pages = 768;
std::vector<std::uint8_t> source(Pages * Page);
std::array<bool, Pages> mapped{};

std::uint8_t* Backing(std::uint64_t address, std::uint64_t size) {
    if (!size || address > source.size() || size > source.size() - address) return nullptr;
    const auto first = address / Page;
    const auto last = (address + size - 1) / Page;
    for (auto page = first; page <= last; ++page) {
        if (!mapped[page]) return nullptr;
    }
    return source.data() + address;
}

void Check(std::uint64_t address, std::uint64_t size) {
    std::vector<std::uint8_t> actual(size, 0xBD);
    iss_copy_sparse(address, actual.data(), size, Backing);
    for (std::uint64_t i = 0; i < size; ++i) {
        const auto expected = mapped[(address + i) / Page] ? source[address + i] : 0;
        assert(actual[i] == expected);
    }
}
} // namespace

int main() {
    for (std::uint64_t i = 0; i < source.size(); ++i) source[i] = (i * 37 + i / Page) & 0xFF;
    mapped.fill(true);
    Check(0, source.size());
    mapped[320] = false;
    mapped[321] = false;
    mapped[580] = false;
    Check(3, source.size() - 11);
    Check(319 * Page + 7, 4 * Page + 71);
    mapped.fill(false);
    mapped[0] = true;
    mapped[Pages - 1] = true;
    Check(0, source.size());
}
