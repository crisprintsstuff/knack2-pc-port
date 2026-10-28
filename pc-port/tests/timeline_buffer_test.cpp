// SPDX-License-Identifier: GPL-2.0-or-later
#include "../gpu/timeline_buffer.h"
#include <thread>
#include <cstdio>
#include <cstdlib>
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"TIMELINE FAIL %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
int main(){
    iss::timeline::Buffer<unsigned,4> tiny;
    for(unsigned i=0;i<4;++i)CHECK(tiny.Push(i));
    CHECK(!tiny.Push(4)&&tiny.Dropped()==1);
    std::array<unsigned,2> part{};CHECK(tiny.Take(part)==2&&part[0]==0&&part[1]==1);
    CHECK(tiny.Push(4)&&tiny.Push(5));
    std::array<unsigned,8> all{};CHECK(tiny.Take(all)==4);
    for(unsigned i=0;i<4;++i)CHECK(all[i]==i+2);
    CHECK(tiny.Take(all)==0);
    iss::timeline::Buffer<unsigned,1024> concurrent;
    std::atomic<unsigned> finished{},accepted{};
    auto produce=[&](unsigned base){for(unsigned i=0;i<10000;++i)accepted+=concurrent.Push(base+i);++finished;};
    std::thread first(produce,0),second(produce,10000);
    std::array<unsigned,256> batch{};std::array<bool,20000> seen{};unsigned received=0;
    while(finished<2){for(auto n=concurrent.Take(batch),i=std::size_t{0};i<n;++i){CHECK(batch[i]<seen.size()&&!seen[batch[i]]);seen[batch[i]]=true;++received;}std::this_thread::yield();}
    first.join();second.join();
    for(;;){auto n=concurrent.Take(batch);if(!n)break;for(std::size_t i=0;i<n;++i){CHECK(batch[i]<seen.size()&&!seen[batch[i]]);seen[batch[i]]=true;++received;}}
    CHECK(received==accepted);CHECK(received+concurrent.Dropped()==20000);
    std::puts("Timeline buffer: wrap, bounded overflow, concurrent producer loss accounting passed");
}
