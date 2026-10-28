// Local phase-0 accounting verification; no game, Vulkan, saves or cache access.
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include "common/types.h"
namespace Common::Perf {
static u64 NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
}
namespace Vulkan { std::atomic<u64> g_iss_frame{}; }
#include "hybrid_family_profile.inl"
using namespace iss::hybrid_profile;
static void Marker(Segment& s, u32 kind, u64 technique, u64 geometry, u64 material) {
    u32 p[9] = {0x49535300u | kind};
    u64 v[4] = {technique, geometry, material, 0};
    std::memcpy(p + 1, v, sizeof(v));
    s.Marker(p);
}
int main() {
    _putenv_s("ISS_HYBRID_FAMILY_PROFILE", "1");
    {
        Segment s;
        for (int i = 0; i < 2; ++i) {
            Marker(s, 1, 11, 22, 33); s.BeginDraw(); s.EndDraw();
        }
        auto& f = Stats().families[1];
        assert(f.draws == 2 && f.repeat_tm == 1 && f.repeat_tmg == 1);
        assert(f.materials.size() == 1 && f.geometries.size() == 1);
        const auto clean = f.ticks;
        Marker(s, 1, 11, 22, 33); s.BeginDraw(); ++switches; s.EndDraw();
        assert(f.contaminated == 1 && f.ticks == clean);
        s.BeginDraw(); s.EndDraw(); // second draw must be unmarked
        assert(Stats().families[0].draws == 1);
        Marker(s, 2, 11, 22, 999); s.BeginDraw(); s.EndDraw();
        assert(Stats().families[2].unknown == 1); // instances != material
        Marker(s, 4, 11, 33, 0);
        Marker(s, 2, 11, 22, 999); s.BeginDraw(); s.EndDraw();
        assert(Stats().families[2].materials.count(33) == 1);
        Marker(s, 3, 12, 22, 999); s.BeginDraw(); s.EndDraw();
        assert(Stats().families[3].unknown == 1); // different technique
        Marker(s, 1, 11, 22, 33); // abandoned at DCB end
    }
    assert(Stats().abandoned == 1);
    {
        Segment s; s.BeginDraw(); s.EndDraw();
        assert(Stats().families[0].draws == 2); // no cross-DCB association
    }
    Vulkan::g_iss_frame = 1;
    {
        Segment s;
        Marker(s, 1, 11, 22, 33); s.BeginDraw(); s.EndDraw();
        const auto& f = Stats().families[1];
        assert(f.draws == 1 && f.ticks > 0 && f.materials.empty());
    }
    Vulkan::g_iss_frame = 16;
    {
        Segment s;
        Marker(s, 2, 11, 22, 999);
        Marker(s, 4, 11, 55, 0); // material bound inside the marked routine
        s.BeginDraw(); s.EndDraw(12, 0);
        assert(Stats().families[2].materials.count(55) == 1);
        s.BeginDraw(); s.EndDraw(0, 1);
        s.BeginDraw(); s.EndDraw(~u32{0}, 2);
        assert(Stats().families[0].empty == 1);
        assert(Stats().families[0].automatic == 1 && Stats().families[0].indirect == 1);
        Marker(s, 1, 11, 22, 33);
        Marker(s, 3, 11, 22, 999);
        assert(Stats().families[1].overwritten == 1);
    }
    assert(Stats().families[3].unfinished == 1);
    std::puts("HYBRID_PROFILE_TEST PASS");
}
