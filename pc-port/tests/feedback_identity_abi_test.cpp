// SPDX-License-Identifier: GPL-2.0-or-later
#include "../native/feedback_identity.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <immintrin.h>

extern "C" {
std::uint64_t iss_feedback_test_call(std::uint64_t, std::uint64_t, std::uint64_t, void*);
void iss_feedback_test_original();
struct Observed {
    std::uint64_t owner, shader, record, arg1, arg2, arg4, arg5, arg6;
    std::uint64_t arg7, arg8, arg9, flags;
};
Observed iss_feedback_test_observed{};
alignas(32) extern const unsigned char iss_feedback_test_ymm_seed[32]{
    1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,
    17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,
};
alignas(32) unsigned char iss_feedback_test_seen_ymm[32]{};
}

namespace {
std::array<std::uint64_t,3> published{};
unsigned publication_count{};
void Sink(std::uint64_t owner,std::uint64_t shader,std::uint64_t record){
    published={owner,shader,record};
    ++publication_count;
    // The bridge must restore upper AVX lanes even if a Windows callback
    // applies the standard vzeroupper transition.
    _mm256_zeroupper();
}
void Require(bool okay,const char* message){
    if(!okay){std::fprintf(stderr,"feedback ABI test failed: %s\n",message);std::abort();}
}
}

int main(){
    constexpr std::size_t size=0x3FAB70+4096;
    auto* image=static_cast<unsigned char*>(VirtualAlloc(nullptr,size,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    Require(image!=nullptr,"synthetic image allocation");
    constexpr unsigned char site[]{
        0x4C,0x89,0xFF,0x4D,0x89,0xE8,0x49,0x89,0xD9,0x48,0x89,0x44,0x24,0x10,
        0x48,0x89,0x4C,0x24,0x08,0x48,0x8D,0x4D,0xA0,0x4C,0x89,0x34,0x24,
        0xE8,0x5F,0xA4,0x00,0x00,
    };
    constexpr unsigned char callee[]{
        0x55,0x48,0x89,0xE5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,
        0x48,0x83,0xEC,0x58,
    };
    std::memcpy(image+0x3F06F1,site,sizeof(site));
    // An otherwise matching CALL with the wrong callee must leave guest bytes
    // untouched and publish nothing.
    Require(!iss::native::InstallFeedbackIdentityCall(image,size,Sink),"signature rejection");
    Require(!std::memcmp(image+0x3F06F1,site,sizeof(site)),"unchanged mismatch callsite");
    std::memcpy(image+0x3FAB70,callee,sizeof(callee));
    Require(iss::native::InstallFeedbackIdentityCall(image,size,Sink),"install near bridge");
    auto* call=image+0x3F070C;
    Require(call[0]==0xE8,"patched call opcode");
    std::int32_t displacement{};
    std::memcpy(&displacement,call+1,sizeof(displacement));
    auto* bridge=call+5+displacement;
    Require(bridge[0]==0xFF&&bridge[1]==0x25,"near bridge indirect jump");
    std::uintptr_t destination{};
    std::memcpy(&destination,bridge+6,sizeof(destination));
    Require(destination==reinterpret_cast<std::uintptr_t>(&iss_feedback_identity_stub),
            "bridge stub target");
    Require(iss_feedback_identity_target==reinterpret_cast<std::uintptr_t>(image+0x3FAB70),
            "original target captured");
    // Execute the installed bridge against a synthetic original. It records
    // all SysV arguments, stack slots, carry flag and 256-bit YMM0.
    iss_feedback_identity_target=reinterpret_cast<std::uintptr_t>(&iss_feedback_test_original);
    const auto result=iss_feedback_test_call(0xAABBCCDD,0x11223344,0x55667788,bridge);
    Require(result==0x4f,"original return");
    Require(publication_count==1&&published==std::array<std::uint64_t,3>{0xAABBCCDD,0x11223344,0x55667788},
            "identity callback arguments");
    const auto& seen=iss_feedback_test_observed;
    Require(seen.owner==published[0]&&seen.shader==published[1]&&seen.record==published[2],
            "callee saved identity registers");
    Require(seen.arg1==0x11111111&&seen.arg2==0x22222222&&seen.arg4==0x44444444&&
            seen.arg5==0x55555555&&seen.arg6==0x66666666,"SysV GP arguments");
    Require(seen.arg7==0x77777777&&seen.arg8==0x88888888&&seen.arg9==0x99999999,
            "SysV stack arguments");
    Require((seen.flags&1)!=0,"carry flag preserved");
    Require(!std::memcmp(iss_feedback_test_seen_ymm,iss_feedback_test_ymm_seed,32),
            "AVX register preserved");
    std::puts("feedback identity ABI test passed");
    return 0;
}
