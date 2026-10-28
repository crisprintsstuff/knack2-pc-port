// SPDX-License-Identifier: GPL-2.0-or-later
#include "../gpu/readback_hint.h"
#include <cstdio>
#include <cstdlib>
#include <thread>
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"Readback hint FAIL: %s\n",#x);std::exit(1);}}while(0)
int main(){
    ReadbackHints hints;constexpr std::uint64_t fallback=512*1024;
    CHECK(hints.WindowFor(0,fallback)==fallback);
    hints.Remember(0x1234);CHECK(hints.WindowFor(0x1000,fallback)==4096);
    CHECK(hints.WindowFor(0x1fff,fallback)==4096);CHECK(hints.WindowFor(0x2000,fallback)==fallback);
    for(int i=0;i<100;++i)hints.Remember(0x1234);
    for(std::uint64_t i=0;i<16;++i)hints.Remember(0x100000+i*4096);
    CHECK(hints.WindowFor(0x1234,fallback)==fallback);
    for(std::uint64_t i=8;i<16;++i)CHECK(hints.WindowFor(0x100000+i*4096+4095,fallback)==4096);
    auto worker=[&](std::uint64_t base){for(unsigned i=0;i<10000;++i){auto address=base+(i%16)*4096;hints.Remember(address);auto size=hints.WindowFor(address,fallback);CHECK(size==4096||size==fallback);}};
    std::thread a(worker,0x200000),b(worker,0x400000);a.join();b.join();
    CHECK(hints.WindowFor(0x800000,fallback)==fallback);
    std::puts("PASS: readback hints page boundaries, fallback, bounded eviction and concurrent access.");
}
