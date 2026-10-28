// SPDX-License-Identifier: GPL-2.0-or-later
#include "descriptor_delta.h"
#include <cassert>
#include <cstdio>
#include <random>

int main() {
    using iss::DescriptorValue;
    iss::DescriptorDelta<64> cache;
    std::array<DescriptorValue, 16> writes;
    std::array<DescriptorValue, 16> device;
    std::array<std::uint8_t, 64> selected;
    for (unsigned i=0; i<writes.size(); ++i) writes[i]={i,0,1,i%7,100+i,200+i,300+i};
    auto apply = [&](std::uint64_t layout, bool full) {
        auto n=cache.Select(layout,writes,selected); assert(n);
        if(full) assert(*n==writes.size());
        for(std::size_t i=0;i<*n;++i) device[selected[i]]=writes[selected[i]];
        for(std::size_t i=0;i<writes.size();++i)
            assert(device[i].SameShape(writes[i]) && device[i].SameValue(writes[i]));
        return *n;
    };
    assert(apply(1,true)==16); assert(apply(1,false)==0);
    writes[3].a++; assert(apply(1,false)==1);
    assert(apply(2,true)==16); // incompatible layout, all descriptors required
    cache.Reset(); assert(apply(2,true)==16); // command buffer/descriptor set invalidation
    writes[4].type++; assert(apply(2,true)==16); // shape change
    std::mt19937 rng(73105);
    unsigned full=0, reduced=0;
    for(unsigned step=0;step<100000;++step) {
        const auto i=rng()%writes.size();
        switch(rng()%4) {
        case 0: writes[i].a=rng(); break;
        case 1: writes[i].b=rng(); break;
        case 2: writes[i].c=rng(); break;
        default: break;
        }
        const bool invalidated=step%193==0;
        if(invalidated) {cache.Reset(); ++full;}
        auto n=apply(2,invalidated);
        if(!invalidated && n<16) ++reduced;
    }
    // Overlapping writes, arrays and nonzero array offsets must not be filtered.
    auto original=writes;
    writes[2].binding=writes[1].binding;
    assert(!cache.Select(2,writes,selected));
    writes=original; assert(apply(2,true)==16);
    writes[2].count=2; assert(!cache.Select(2,writes,selected));
    writes=original; assert(apply(2,true)==16);
    writes[2].array_element=1; assert(!cache.Select(2,writes,selected));
    writes=original; assert(apply(2,true)==16);
    assert(!cache.Select(0,writes,selected));
    assert(!cache.Select(2,writes,std::span<std::uint8_t>{selected.data(),1}));
    assert(apply(2,true)==16);
    // Graphics and compute use distinct caches: switching one cannot warm the other.
    iss::DescriptorDelta<64> compute;
    assert(*compute.Select(2,writes,selected)==16);
    assert(apply(2,false)==0);
    std::printf("PASS descriptor delta: 100000 replay parity cases, %u invalidations, %u partial updates\n",full,reduced);
}
