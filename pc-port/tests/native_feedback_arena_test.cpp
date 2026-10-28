// SPDX-License-Identifier: GPL-2.0-or-later
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "../native/feedback_arena.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#define CHECK(x) do { if(!(x)){std::fprintf(stderr,"ARENA FAIL %d: %s\n",__LINE__,#x);std::exit(1);} } while(0)
using U64=std::uint64_t;
using Original=void (__attribute__((sysv_abi)) *)(U64*,U64,std::int32_t);
constexpr unsigned char code[]{0x48,0x83,0x7f,0x08,0x00,0x75,0x11,0x48,0x63,0xc2,0x48,0x89,0x37,0x48,0x89,0x77,0x08,0x48,0x01,0xf0,0x48,0x89,0x47,0x10,0xc3};
static U64 seen{}, last_base{}, last_epoch{};
void Sink(U64 base,U64 size,U64 generation){CHECK(size==0x1e00000);CHECK(generation>last_epoch);++seen;last_base=base;last_epoch=generation;}
int main(){
    auto* page=static_cast<unsigned char*>(VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));CHECK(page);
    std::memcpy(page,code,sizeof(code));DWORD old{};CHECK(VirtualProtect(page,4096,PAGE_EXECUTE_READ,&old));
    CHECK(FlushInstructionCache(GetCurrentProcess(),page,sizeof(code)));
    const auto original=reinterpret_cast<Original>(page);
    unsigned count=0;
    for(U64 base:{U64{0},U64{1},U64{0x208000000},~U64{0},~U64{0}-31})
      for(U64 cursor:{U64{0},U64{1},U64{0x208000000},~U64{0}})
        for(std::int32_t length:{0,1,-1,0x1e00000,std::numeric_limits<std::int32_t>::min(),std::numeric_limits<std::int32_t>::max()}){
            std::array<U64,5> expected{77,cursor,88,99,100},actual=expected;
            original(expected.data(),base,length);
            CHECK(iss::native::InitializeArena(actual.data(),base,length)==(cursor==0));
            CHECK(actual==expected);++count;
        }
    constexpr std::size_t image_size=0x7d3eb40;
    auto* image=static_cast<unsigned char*>(VirtualAlloc(nullptr,image_size,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));CHECK(image);
    _putenv_s("ISS_FEEDBACK_ARENA","1");
    CHECK(iss::native::InstallFeedbackArena(image,image_size,Sink)==-1);
    std::memcpy(image+0x577cb0,code,sizeof(code));
    CHECK(VirtualProtect(image+0x577cb0,sizeof(code),PAGE_EXECUTE_READ,&old));
    CHECK(iss::native::InstallFeedbackArena(image,image_size,Sink)==1);
    const auto hooked=reinterpret_cast<Original>(image+0x577cb0);
    auto* arena=reinterpret_cast<U64*>(image+0x7d3eb18);
    hooked(arena,0x200000000,0x1e00000);CHECK(seen==1&&last_base==0x200000000);
    hooked(arena,0x210000000,0x1e00000);CHECK(seen==1&&arena[0]==0x200000000);
    arena[1]=0;hooked(arena,0x210000000,0x1e00000);CHECK(seen==2&&last_base==0x210000000);
    arena[1]=0;hooked(arena,0x200000000,0x1e00000);CHECK(seen==3&&last_base==0x200000000);
    std::array<U64,3> other{};hooked(other.data(),9,0x1e00000);CHECK(seen==3&&other[0]==9);
    VirtualFree(image,0,MEM_RELEASE);VirtualFree(page,0,MEM_RELEASE);
    std::printf("Native arena: %u original-machine-code differential cases + identity/epoch hook passed\n",count);
}
