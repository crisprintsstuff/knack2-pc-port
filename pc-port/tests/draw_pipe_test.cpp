// SPDX-License-Identifier: GPL-2.0-or-later
// Exercise the real queue and immutable shader views without Vulkan or the game.
#define FMT_HEADER_ONLY
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "draw_pipe.h"
#include "shader_recompiler/info.h"
void assert_fail_impl(){std::abort();}
[[noreturn]] void unreachable_impl(){std::abort();}
namespace Common {void SetCurrentThreadName(const char*){}}
namespace Common::Log {void VLog(Class,Level,const char*,int,const char*,fmt::string_view,fmt::format_args){std::fputs("ASSERT in draw-pipe test\n",stderr);std::abort();}}
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"DRAW_PIPE_TEST_FAIL line=%d %s\n",__LINE__,#x);std::abort();}}while(0)
struct Consumer {u64 expected{},bytes{};};
static u8 Pattern(u64 index,u32 offset){return u8((index*31+offset*17)^(offset>>5));}
static void Consume(void* state,const u8* bytes,u32 size){
    auto& c=*static_cast<Consumer*>(state);u64 index=0;std::memcpy(&index,bytes,8);
    CHECK(iss::DrawPipe::OnWorker());CHECK(index==c.expected++);
    for(u32 i=8;i<size;++i)CHECK(bytes[i]==Pattern(index,i));c.bytes+=size;
    if(index%2048==0)std::this_thread::sleep_for(std::chrono::milliseconds(1));
}
static void QueueTest(){
    for(unsigned trial=0;trial<4;++trial){Consumer c;u64 expected_bytes=0;
        {iss::DrawPipe queue(Consume,&c);
            for(u64 i=0;i<20000;++i){
                const u32 size=i%1000==0?iss::DrawPipe::MaxPayload-13:16+u32((i*7919)%16384);
                auto* out=queue.Begin(size);std::memcpy(out,&i,8);for(u32 w=8;w<size;++w)out[w]=Pattern(i,w);expected_bytes+=size;queue.Commit();
                if(i%337==0){queue.Drain();CHECK(c.expected==i+1);CHECK(queue.Idle());}
                if(i%2999==0)std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if(trial%2==0){queue.Drain();CHECK(c.expected==20000);}
            // Odd trials exercise destructor drain while packets can still be queued.
        }
        CHECK(c.expected==20000);CHECK(c.bytes==expected_bytes);
    }
}
static void SnapshotTest(){
    Shader::Info info;std::array<u32,16> live{},saved{};info.user_data=live;info.pgm_base=0x10000000;info.flattened_ud_buf.resize(16);
    AmdGpu::Buffer sharp{};sharp.base_address=0x20000000;sharp.stride=16;sharp.num_records=32;
    std::memcpy(info.flattened_ud_buf.data(),&sharp,sizeof(sharp));
    std::array<u32,16> flat{};std::memcpy(flat.data(),info.flattened_ud_buf.data(),flat.size()*4);
    Shader::BufferResource resource;resource.sharp_fetch.summary=Shader::SharpFetch<AmdGpu::Buffer>::Summary::SingleLoad;resource.sharp_fetch.offsets[0]=0;
    const auto before=resource.GetSharp(info);const Shader::Info::UdRead captured{2,4,sharp};
    Shader::Info::ud_snapshots[0]={&info,saved.data(),16,flat.data(),16,info.pgm_base,&captured,1};Shader::Info::num_ud_snapshots=1;
    // Simulate stage A mutating and reallocating the shared vectors while B retains a packet.
    live[0]=0xdeadbeef;info.pgm_base=0x30000000;info.flattened_ud_buf.assign(4096,0);
    CHECK(info.UserData()[0]==0);CHECK(info.ProgramBase()==0x10000000);CHECK(info.FlatUserData().size()==16);
    const auto during=resource.GetSharp(info);CHECK(std::memcmp(&before,&during,sizeof(before))==0);
    const auto vertex=info.ReadUdReg<AmdGpu::Buffer>(2,4);CHECK(std::memcmp(&vertex,&sharp,sizeof(sharp))==0);
    Shader::Info::num_ud_snapshots=0;CHECK(info.UserData()[0]==0xdeadbeef);CHECK(info.ProgramBase()==0x30000000);CHECK(info.FlatUserData().size()==4096);
}
int main(){QueueTest();SnapshotTest();std::puts("PASS: ordered queue, repeated ring wraps, backpressure, sleep/wake, destructor drain and UD/vertex snapshots survive producer mutation.");}
