// SPDX-License-Identifier: GPL-2.0-or-later
// Compare with isolated, executed AVX instructions extracted from the original eboot.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../native/reduction.h"
#include <vector>
#include <array>
#include <string>
#include <fstream>
#include <iterator>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <limits>
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"NATIVE REDUCTION FAIL line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
using iss::native::ReductionBindings;
static std::vector<float> values;
static std::string events;
static std::uint32_t frame;
static std::uintptr_t current,next,expected_current;
static std::uint8_t reset;
static std::uint32_t width=1920,height=1080;
static void ISS_SYSV Enter(void *ctx){events+='E';std::uint32_t one=1;std::memcpy(static_cast<char*>(ctx)+0xde84,&one,4);}
static void ISS_SYSV Leave(void *ctx){events+='L';std::uint32_t zero=0;std::memcpy(static_cast<char*>(ctx)+0xde84,&zero,4);}
static void ISS_SYSV Dispatch(void *state,const void *,std::uint64_t argument,std::uint32_t w,std::uint32_t h,std::uint32_t x,std::uint32_t y,void *){
    events+='D';CHECK(state==&reset&&argument==0x12345678&&w==width&&h==height&&x==(width+127)/128&&y==(height+71)/72);CHECK(current==expected_current);
}
static float ISS_SYSV Exp(float sum){events+='X';return sum+123.0f;}
static const float *Read(std::uint64_t addr,std::size_t bytes){events+='R';CHECK(addr==0x12300003456ULL&&bytes==values.size()*4);return values.data();}
int main(){
    std::ifstream input("reduction-original.bin",std::ios::binary);std::vector<unsigned char> original((std::istreambuf_iterator<char>(input)),{});CHECK(original.size()==644);
    std::uint64_t hash=14695981039346656037ULL;for(auto byte:original)hash=(hash^byte)*1099511628211ULL;CHECK(hash==0x41698c67351a07aeULL);
    std::vector<unsigned char> code={0x57,0x56,0x53,0x41,0x54,0x41,0x55,0x48,0x89,0xcf,0x89,0xd3,0x41,0x89,0xd4,0x41,0x83,0xe4,0x0f,0x89,0xd6,0x44,0x29,0xe6,
        0xc5,0xfc,0x57,0xc0,0xc5,0xf4,0x57,0xc9,0x85,0xf6,0x0f,0x84,0,0,0,0};
    const auto skip_bulk=code.size()-4;
    const unsigned char setup[]={0x4c,0x8d,0x6f,0x20,0x89,0xf2};code.insert(code.end(),std::begin(setup),std::end(setup));
    code.insert(code.end(),original.begin()+0x1b0,original.begin()+0x1e5);
    auto relative=static_cast<std::int32_t>(code.size()-skip_bulk-4);std::memcpy(code.data()+skip_bulk,&relative,4);
    const unsigned char check[]={0x39,0xde,0x0f,0x83,0,0,0,0};code.insert(code.end(),std::begin(check),std::end(check));const auto skip_scalar=code.size()-4;
    code.insert(code.end(),original.begin()+0x1f0,original.begin()+0x1fd);
    relative=static_cast<std::int32_t>(code.size()-skip_scalar-4);std::memcpy(code.data()+skip_scalar,&relative,4);
    const unsigned char done[]={0xc5,0xf8,0x77,0x41,0x5d,0x41,0x5c,0x5b,0x5e,0x5f,0xc3};code.insert(code.end(),std::begin(done),std::end(done));
    void *memory=VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);CHECK(memory);std::memcpy(memory,code.data(),code.size());DWORD old;CHECK(VirtualProtect(memory,4096,PAGE_EXECUTE_READ,&old));CHECK(FlushInstructionCache(GetCurrentProcess(),memory,code.size()));
    auto oracle=reinterpret_cast<float(*)(const float*,std::uint32_t)>(memory);
    std::uint32_t random=0x19781234;unsigned comparisons=0;
    for(auto n:{0u,1u,2u,15u,16u,17u,31u,32u,33u,225u,512u,29184u})for(int pattern=0;pattern<20;++pattern){
        values.resize(n);for(auto& v:values){random=random*1664525+1013904223;v=static_cast<float>(static_cast<int>(random%2000001)-1000000)/37.0f;}
        if(pattern==0)for(auto& v:values)v=-0.0f;
        if(pattern==1)for(unsigned i=0;i<n;++i)values[i]=i%3==0?1e20f:i%3==1?-1e20f:1.0f;
        if(pattern==2)for(auto& v:values)v=std::numeric_limits<float>::denorm_min();
        float expected=oracle(values.data(),n),actual=iss::native::ReduceOrdered(values.data(),n);
        CHECK(!std::memcmp(&expected,&actual,4));++comparisons;
    }
    std::array<std::uint8_t,12> descriptor{},descriptor2{},info{};std::uint32_t lo=0x3456,hi=0xfff00123;
    std::memcpy(descriptor.data()+4,&lo,4);std::memcpy(descriptor.data()+8,&hi,4);descriptor2=descriptor;
    current=reinterpret_cast<std::uintptr_t>(descriptor.data());next=reinterpret_cast<std::uintptr_t>(descriptor2.data());
    std::uint32_t packed=(width-1)|((height-1)<<14)|0xa0000000;std::memcpy(info.data()+8,&packed,4);
    std::vector<std::uint8_t> context(0xde88);values.assign(225,.25f);
    ReductionBindings bindings{&reset,&frame,&current,&next,&reset,Enter,Leave,Dispatch,Exp,Read};float result=-1;
    for(unsigned i=0;i<3;++i){events.clear();expected_current=next;CHECK(iss::native::Reduce(bindings,info.data(),0x12345678,context.data(),&result)==(i==2));CHECK(frame==i+1);CHECK(events==(i==2?"ERDLX":"EDLX"));CHECK(result==(i==2?oracle(values.data(),225)+123.0f:123.0f));}
    reset=1;events.clear();expected_current=next;CHECK(!iss::native::Reduce(bindings,info.data(),0x12345678,context.data(),&result));CHECK(frame==1&&events=="EDLX");
    reset=0;frame=2;std::uint32_t mode=1;std::memcpy(context.data()+0xde84,&mode,4);events.clear();expected_current=next;
    CHECK(iss::native::Reduce(bindings,info.data(),0x12345678,context.data(),&result)==1);CHECK(events=="RDX");
    CHECK(VirtualFree(memory,0,MEM_RELEASE));std::printf("PASS: %u original-machine-code parity cases, warm-up, reset, buffer swap, dispatch ordering and context preservation.\n",comparisons);
}
