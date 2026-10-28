// SPDX-License-Identifier: GPL-2.0-or-later
#include <atomic>
#include <cassert>
#include <latch>
#include <shared_mutex>
#include <thread>

#include "g4_conditional_backing_write.h"

int main() {
    std::shared_mutex mapping_mutex;
    std::uint64_t epoch = 1;
    int value = 0;
    const auto with_lock = [&](auto&& action) {
        std::shared_lock lock{mapping_mutex};
        return action(epoch);
    };
    assert(!iss::g4::ConditionalBackingWrite(with_lock, 2, [] { return true; },
                                              [&] { value = 1; }));
    assert(!iss::g4::ConditionalBackingWrite(with_lock, 1, [] { return false; },
                                              [&] { value = 1; }));
    assert(value == 0);

    std::latch write_entered{1}, finish_write{1}, remap_attempted{1};
    std::atomic<bool> committed{};
    std::jthread copy{[&] {
        committed.store(iss::g4::ConditionalBackingWrite(
                            with_lock, 1, [] { return true; }, [&] {
                                write_entered.count_down();
                                finish_write.wait();
                                value = 7;
                            }),
                        std::memory_order_release);
    }};
    write_entered.wait();
    std::jthread remap{[&] {
        remap_attempted.count_down();
        std::unique_lock lock{mapping_mutex};
        epoch = 2;
    }};
    remap_attempted.wait();
    // The write owns a shared mapping lease; the remap cannot take its
    // exclusive lease until the write finishes.
    assert(epoch == 1 && value == 0);
    finish_write.count_down();
    copy.join();
    remap.join();
    assert(committed.load(std::memory_order_acquire));
    assert(value == 7 && epoch == 2);
}
