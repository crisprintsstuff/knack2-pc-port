// SPDX-License-Identifier: GPL-2.0-or-later
#include "../gpu/native_backing_alias_cache.h"
#include <cstdio>
#include <cstdlib>
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"ALIAS CACHE FAIL %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
int main(){
    iss::native_geometry::BackingAliasCache<4> cache;
    cache.Store({1,0x1000,0x2000,7,9,0x9000,true});
    const auto* hit=cache.Find(1,0x1000,0x2000,7,9);CHECK(hit&&hit->unique&&hit->physical==0x9000);
    CHECK(!cache.Find(1,0x1000,0x2000,8,9)); // Native remap.
    CHECK(!cache.Find(1,0x1000,0x2000,7,10)); // GPU registration/unmap.
    CHECK(!cache.Find(2,0x1000,0x2000,7,9)); // Different owner.
    CHECK(!cache.Find(1,0x1000,0x3000,7,9)); // Different extent.
    CHECK(!cache.Find(1,0x1000,0x2000,0,9));
    for(unsigned i=0;i<10000;++i){
        const auto start=std::uint64_t(i+1)*4096,end=start+4096,physical=0x100000+start;
        cache.Store({1,start,end,7,9,physical,true});
        const auto* current=cache.Find(1,start,end,7,9);CHECK(current&&current->physical==physical);
        for(unsigned distance=1;distance<=8&&distance<=i;++distance){
            const auto old=std::uint64_t(i+1-distance)*4096;
            const auto* e=cache.Find(1,old,old+4096,7,9);
            CHECK(!e||e->physical==0x100000+old); // Collision can only evict.
        }
    }
    cache.Store({1,0x1000,0x2000,7,9,0,false});
    hit=cache.Find(1,0x1000,0x2000,7,9);CHECK(hit&&!hit->unique);
    cache.Store({1,0x1000,0x2000,7,9,0x9000,true,17,true});
    hit=cache.Find(1,0x1000,0x2000,7,9);
    CHECK(hit&&hit->image_epoch==17&&hit->no_images);
    std::puts("Alias cache: mapping generations, owner, bounds and 10000 collision cases passed");
}
