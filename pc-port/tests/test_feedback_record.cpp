#include <algorithm>
#include <array>
#include <cassert>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <thread>
#include <vector>

#include "infamous_renderer/feedback_record.h"

int main() {
    using iss::game::feedback::Record;
    std::array<std::byte, Record::ByteSize> raw{};
    raw.fill(std::byte{0xC3});
    auto preserved = Record::FromRawBytes(raw);
    std::array<std::byte, Record::ByteSize> copied{};
    preserved.CopyRawBytes(copied);
    assert(copied == raw);
    preserved.InitializeKnownWords(37, {90, 80, 70});
    preserved.CopyRawBytes(copied);
    assert(copied[44] == std::byte{0xC3} && copied[47] == std::byte{0xC3});

    Record eligibility{};
    eligibility.words[0] = 4;
    const auto one = std::bit_cast<std::uint32_t>(1.0f);
    const auto nan = std::bit_cast<std::uint32_t>(std::numeric_limits<float>::quiet_NaN());
    assert(eligibility.MainLaneEligible(3, 2, 2000, 1000, nan));
    assert(eligibility.MainLaneEligible(4, 2, 2000, 1000, one));
    assert(!eligibility.MainLaneEligible(4, 2, 3000, 1000, one));
    assert(!eligibility.MainLaneEligible(5, 2, 2000, 1000, nan));
    assert(!eligibility.MainLaneEligible(6, 2, 2000, 1000, one));
    // Unsigned subtraction wraps, then the shader reinterprets as signed.
    assert(eligibility.MainLaneEligible(4, 2, 0, 1, one));
    eligibility.words[0] = 0xFFFFFFFFu;
    assert(!eligibility.MainLaneEligible(0xFFFFFFFFu, 2, 2000, 1000, one));

    Record record{};
    record.words[11] = 0xA5A5A5A5;
    record.InitializeKnownWords(37, {90, 80, 70});
    assert(record.words[0] == 0 && record.words[1] == 0);
    assert(record.words[2] == 37 && record.words[3] == 0);
    assert(record.words[4] == 90 && record.words[5] == 80 && record.words[6] == 70);
    assert(record.words[7] == 0 && record.words[8] == 0 && record.words[9] == 0);
    assert(record.words[10] == 0 && record.words[11] == 0xA5A5A5A5);

    constexpr std::uint32_t workers = 4;
    constexpr std::uint32_t per_worker = 1000;
    std::array<std::vector<std::uint32_t>, workers> indices;
    std::array<std::thread, workers> threads;
    for (std::uint32_t worker = 0; worker < workers; ++worker) {
        threads[worker] = std::thread([&, worker] {
            indices[worker].reserve(per_worker);
            for (std::uint32_t i = 0; i < per_worker; ++i) {
                indices[worker].push_back(record.ApplyMainBranch(false, 0,
                    {90 - worker, 80 - worker, 70 - worker},
                    {10 + worker, 20 + worker, 30 + worker}));
            }
        });
    }
    for (auto& thread : threads) thread.join();
    std::vector<std::uint32_t> all;
    for (const auto& part : indices) all.insert(all.end(), part.begin(), part.end());
    std::sort(all.begin(), all.end());
    assert(all.size() == workers * per_worker);
    for (std::uint32_t i = 0; i < all.size(); ++i) assert(all[i] == i);
    assert(record.words[1] == workers * per_worker);
    assert(record.words[4] == 87 && record.words[5] == 77 && record.words[6] == 67);
    assert(record.words[7] == 13 && record.words[8] == 23 && record.words[9] == 33);
    const auto existing = record.ApplyMainBranch(true, 42, {2, 3, 4}, {22, 23, 24});
    assert(existing == 42 && record.words[1] == workers * per_worker);
    assert(record.words[4] == 2 && record.words[5] == 3 && record.words[6] == 4);
    assert(record.words[7] == 22 && record.words[8] == 23 && record.words[9] == 33);
    assert(record.ReserveSecondaryIndex() == 0);
    assert(record.ReserveSecondaryIndex() == 1);
    assert(record.words[3] == 2);
    assert(record.words[11] == 0xA5A5A5A5);
}
