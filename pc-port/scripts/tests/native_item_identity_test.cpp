// Standalone C++20 test; no renderer, game process, or Vulkan required.
#include "../../gpu/native_item_identity.h"
#include <cassert>
#include <vector>
using namespace iss::native_item;
using Words = std::vector<std::uint32_t>;
static void Marker(Words& w, unsigned kind, unsigned identity = 7) {
    const std::uint32_t p[] = {0xc0081000u, 0x49535300u | kind, identity, 0, 8, 0, 9, 0, 10, 0};
    w.insert(w.end(), std::begin(p), std::end(p));
}
static void Draw(Words& w) {
    const std::uint32_t p[] = {0xc0042700u, 3, 0x1000, 0, 3, 0};
    w.insert(w.end(), std::begin(p), std::end(p));
}
int main() {
    Words w; Marker(w, 1); Draw(w); Marker(w, 5);
    const auto ok = Scan(w, true);
    assert(ok.reject == Reject::None && ok.draw == w.data() + 10 && ok.identity.technique == 7);
    assert(Scan(w, false).reject == Reject::Disabled);
    assert(current == nullptr);
    { Scope scope(&ok.identity); assert(current == &ok.identity); }
    assert(current == nullptr);
    auto truncated = w; truncated.pop_back();
    assert(Scan(truncated, true).reject == Reject::Truncated);
    w.resize(16); assert(Scan(w, true).reject == Reject::MissingEnd);
    Draw(w); Marker(w, 5); assert(Scan(w, true).reject == Reject::MultipleDraws);
    w.clear(); Marker(w, 1); Marker(w, 5); assert(Scan(w, true).reject == Reject::NoDraw);
    w.clear(); Marker(w, 1); Draw(w); Marker(w, 5, 42);
    assert(Scan(w, true).reject == Reject::IdentityMismatch);
    for (unsigned kind : {1, 2, 3}) {
        w.clear(); Marker(w, 1); Marker(w, kind); Draw(w); Marker(w, 5);
        assert(Scan(w, true).reject == Reject::Nested);
    }
    w.clear(); Marker(w, 1); Marker(w, 4); Draw(w); Marker(w, 5);
    assert(Scan(w, true).reject == Reject::None);
    for (unsigned op : {0x15, 0x20, 0x22, 0x23, 0x24, 0x2d, 0x33, 0x35, 0x3f, 0x59, 0xff}) {
        w.clear(); Marker(w, 1); w.push_back(0xc0000000u | (op << 8)); w.push_back(0);
        Draw(w); Marker(w, 5); assert(Scan(w, true).reject == Reject::Unsupported);
    }
    w.clear(); Marker(w, 1); Draw(w); Marker(w, 5); w[10] |= 1;
    assert(Scan(w, true).reject == Reject::Unsupported);
    w.clear(); Marker(w, 1); w.insert(w.end(), 4096, 0x80000000u); Draw(w); Marker(w, 5);
    assert(Scan(w, true).reject == Reject::Budget);
    w.clear(); Marker(w, 1); w.insert(w.end(), {0xc0016900u, 1, 2}); Draw(w); Marker(w, 5);
    assert(Scan(w, true).reject == Reject::None);
    // A close from another DCB cannot complete the first span.
    assert(Scan(std::span(w).first(w.size() - 10), true).reject == Reject::MissingEnd);
}
