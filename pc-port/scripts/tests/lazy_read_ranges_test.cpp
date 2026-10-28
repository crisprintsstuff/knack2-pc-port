// Equivalence of Runtime::LazyReadList (ISS_LAZY_READ_RANGES=1) with IntervalList: random
// Add/Overlaps/Clear sequences must give the same Overlaps answers and the same final set.
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>
#include "common/interval_set.h"

static bool g_lazy = true;
struct LazyReadList {  // Copy of the vk_runtime.h/.cpp implementation.
    IntervalList<Interval> merged;
    std::vector<Interval> pending;
    static bool Lazy() { return g_lazy; }
    void Add(Interval range) {
        if (Lazy()) {
            if (range.start < range.end) pending.push_back(range);
        } else {
            merged.Add(range);
        }
    }
    bool Overlaps(u64 start, u64 end) { Merge(); return merged.Overlaps(start, end); }
    void Clear() { merged.Clear(); pending.clear(); }
    void Merge() {
        if (pending.empty()) return;
        pending.insert(pending.end(), merged.begin(), merged.end());
        std::ranges::sort(pending, {}, &Interval::start);
        merged.Clear();
        Interval current = pending.front();
        for (size_t i = 1; i < pending.size(); ++i) {
            const auto& next = pending[i];
            if (next.start <= current.end) current.end = std::max(current.end, next.end);
            else { merged.Add(current); current = next; }
        }
        merged.Add(current);
        pending.clear();
    }
};

int main() {
    std::mt19937_64 rng(12345);
    u64 checks = 0;
    for (int trial = 0; trial < 2000; ++trial) {
        IntervalList<Interval> ref;
        LazyReadList lazy;
        const u64 space = trial % 3 == 0 ? 64 : trial % 3 == 1 ? 4096 : (1ull << 40);
        for (int op = 0; op < 400; ++op) {
            const u64 a = rng() % space, len = rng() % (space / 8 + 2);
            const u64 s = a, e = a + len;  // includes empty ranges (len 0)
            const int kind = static_cast<int>(rng() % 10);
            if (kind < 6) {
                ref.Add({s, e}); lazy.Add({s, e});
            } else if (kind < 9) {
                ++checks;
                if (ref.Overlaps(s, e) != lazy.Overlaps(s, e)) {
                    std::printf("FAIL overlaps trial=%d op=%d [%llu,%llu)\n", trial, op,
                                (unsigned long long)s, (unsigned long long)e);
                    return 1;
                }
            } else if (rng() % 8 == 0) {
                ref.Clear(); lazy.Clear();
            }
        }
        lazy.Merge();
        std::vector<Interval> x(ref.begin(), ref.end()), y(lazy.merged.begin(), lazy.merged.end());
        if (x.size() != y.size()) { std::printf("FAIL set size trial=%d\n", trial); return 1; }
        for (size_t i = 0; i < x.size(); ++i)
            if (x[i].start != y[i].start || x[i].end != y[i].end) {
                std::printf("FAIL set trial=%d i=%zu\n", trial, i); return 1;
            }
    }
    std::printf("PASS lazy_read_ranges checks=%llu\n", (unsigned long long)checks);
    return 0;
}
