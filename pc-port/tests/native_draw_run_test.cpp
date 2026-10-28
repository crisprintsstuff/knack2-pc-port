// SPDX-License-Identifier: GPL-2.0-or-later
#include "../gpu/infamous_renderer/native_draw_run.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
using namespace iss::native_geometry;
using Words = std::vector<std::uint32_t>;
static void Packet(Words& w, unsigned op, std::initializer_list<std::uint32_t> payload) {
    w.push_back(0xc0000000u | (unsigned(payload.size() - 1) << 16) | (op << 8));
    w.insert(w.end(), payload);
}
int main() {
    std::array<std::uint32_t, 128> regs{};
    regs[42] = 0x12340000; regs[43] = 2; regs[92] = 4;
    RunInputs in{regs,32,90,42,0,1,{0x212340000ULL,0x250000000ULL,3,3,0,0}};
    Words prefix;
    Packet(prefix,0x10,{0,0xdeadbeef,0xbaadf00d});
    Packet(prefix,0x79,{2,4});
    Packet(prefix,0x2a,{0});
    Packet(prefix,0x2f,{1});
    Packet(prefix,0x76,{10,0x12340030,2});
    const auto draw_word = prefix.size();
    Packet(prefix,0x27,{6,0x60000000,2,6,0});
    const auto consumed = prefix.size();
    Packet(prefix,0x76,{10,0x12340060,2}); // Must not consume a setter without its draw.
    auto r = CollectIndexedRun(prefix,in);
    assert(r.draws.size()==2 && r.consumed_words==consumed && r.packet_count==6);
    assert(r.draws[1].fetch_descriptors==0x212340030ULL && r.draws[1].index_address==0x260000000ULL);
    assert(r.draws[1].packet_word==draw_word && regs[42]==0x12340000);
    assert(CollectIndexedRun(prefix,in,64,5).draws.empty());
    assert(CollectIndexedRun(prefix,in,64,6).consumed_words==consumed);
    unsigned checks=0;
    // Every other opcode is a boundary, even when its packet could look like a draw or setter.
    for(unsigned op=0;op<256;++op) {
        if(op==0x10||op==0x76||op==0x79||op==0x2a||op==0x2f||op==0x27)continue;
        Words w; Packet(w,op,{0}); w.insert(w.end(),prefix.begin(),prefix.end());
        assert(CollectIndexedRun(w,in).draws.empty()); ++checks;
    }
    for(std::size_t n=0;n<consumed;++n) {
        auto truncated=std::span(prefix).first(n);
        assert(CollectIndexedRun(truncated,in).draws.empty()); ++checks;
    }
    for(auto changed : {Words{0xc0017600,9,1}, Words{0xc0017900,2,5},
                        Words{0xc0002a00,1}, Words{0xc0002f00,2},
                        Words{0xc0001000,0x68750001}, Words{0xc0017600,200,0}}) {
        changed.insert(changed.end(),prefix.begin(),prefix.end());
        assert(CollectIndexedRun(changed,in).draws.empty()); ++checks;
    }
    { Words compute; Packet(compute,0x76,{0x200,0});
      compute.insert(compute.end(),prefix.begin(),prefix.end());
      assert(CollectIndexedRun(compute,in).draws.empty()); }
    Words w(prefix.begin(),prefix.begin()+consumed);
    Packet(w,0x76,{9,1,0x99990000}); // Entire setter must be rejected on another changed register.
    Packet(w,0x27,{3,0x70000000,2,3,0});
    r=CollectIndexedRun(w,in); assert(r.draws.size()==2 && r.consumed_words==consumed);
    for(unsigned i=0;i<70;++i)Packet(w,0x27,{3,0x70000000+i*2,2,3,0});
    // A separate all-neutral run reaches the requested bound exactly.
    w.clear(); for(unsigned i=0;i<70;++i)Packet(w,0x27,{3,0x70000000+i*2,2,3,0});
    r=CollectIndexedRun(w,in,64); assert(r.draws.size()==64 && r.consumed_words==63*6);
    for(std::size_t count=2;count<=64;++count) {
        const auto shorter=PrefixIndexedRun(r,w,count);
        assert(shorter.draws.size()==count && shorter.consumed_words==(count-1)*6);
        assert(shorter.packet_count==count-1);
        if(count<64)assert(w[shorter.consumed_words]==0xc0042700); // Next draw untouched.
    }
    assert(PrefixIndexedRun(r,w,1).draws.empty());
    assert(PrefixIndexedRun(r,w,65).draws.empty());
    assert(PrefixIndexedRun(r,std::span(w).first(5),2).draws.empty());
    const auto original_prefix=CollectIndexedRun(prefix,in);
    const auto exact_prefix=PrefixIndexedRun(original_prefix,prefix,2);
    assert(exact_prefix.consumed_words==consumed && exact_prefix.packet_count==6);
    assert(CollectIndexedRun(w,in,257).draws.empty());
    std::array<unsigned char,2048> source{};
    for(std::size_t i=0;i<source.size();++i)source[i]=static_cast<unsigned char>(i*31+7);
    std::uint32_t random=0x51a67d02;
    for(unsigned test=0;test<512;++test) {
        std::vector<MemoryRead> reads;
        std::array<bool,2048> covered{}, actual{};
        for(unsigned i=0;i<1+test%64;++i) {
            random=random*1664525+1013904223; const auto at=random%1920;
            random=random*1664525+1013904223; const auto size=1+random%128;
            reads.push_back({0x1000+at,size});
            for(unsigned j=0;j<size;++j)covered[at+j]=true;
        }
        const auto plan=PlanReadSnapshot(reads,source.size()); assert(plan);
        std::vector<unsigned char> snapshot(plan->size);
        std::uint64_t previous_end=0;
        for(const auto& range:plan->ranges) {
            assert(previous_end<range.address); previous_end=range.address+range.size;
            assert(range.offset+range.size<=snapshot.size());
            std::memcpy(snapshot.data()+range.offset,source.data()+range.address-0x1000,range.size);
            for(std::uint64_t j=0;j<range.size;++j)actual[range.address-0x1000+j]=true;
        }
        assert(actual==covered); // No gaps or unrelated source bytes were read.
        for(std::size_t i=0;i<reads.size();++i) {
            assert(plan->offsets[i]+reads[i].size<=snapshot.size());
            assert(!std::memcmp(snapshot.data()+plan->offsets[i],
                source.data()+reads[i].address-0x1000,reads[i].size));
        }
    }
    const std::array<MemoryRead,1> overflow{{{UINT64_MAX,2}}};
    const std::array<MemoryRead,1> too_big{{{1,128}}};
    assert(!PlanReadSnapshot(overflow)); assert(!PlanReadSnapshot(too_big,127));
    assert(!PlanReadSnapshot({}));
    const std::array<std::uint16_t,4> local_indices{2,0,3,1};
    assert(UsedVertexPrefix(local_indices,8,8,64*1024*1024)==32);
    assert(UsedVertexPrefix(local_indices,8,4,32)==32);
    assert(!UsedVertexPrefix(local_indices,8,8,31));
    assert(!UsedVertexPrefix(local_indices,8,12,64));
    assert(!UsedVertexPrefix({},8,8,64));
    const std::array<std::uint16_t,1> last_vertex{65535};
    assert(UsedVertexPrefix(last_vertex,8,8,524288)==524288);
    assert(!UsedVertexPrefix(last_vertex,8,8,524287));
    for(std::uint16_t maximum=0;maximum<1024;++maximum) {
        const std::array<std::uint16_t,3> draw_indices{maximum,0,std::uint16_t(maximum/2)};
        const auto used=UsedVertexPrefix(draw_indices,8,8,65536);assert(used);
        for(const auto index:draw_indices)for(unsigned byte=0;byte<8;++byte)
            assert(std::uint64_t(index)*8+byte<*used);
    }
    unsigned prefix_checks=0;
    std::vector<std::uint16_t> varied_indices;
    for(unsigned test=0;test<2048;++test) {
        // Long arrays exercise the same scan being reduced from three passes to
        // one; zeros and UINT16_MAX cover both ends of the guest index contract.
        varied_indices.resize(1+(test*67u)%4096u);
        for(auto& index:varied_indices){random=random*1664525+1013904223;index=std::uint16_t(random>>16);}
        if(test%3==0)std::ranges::fill(varied_indices,std::uint16_t{0});
        if(test%3==1)varied_indices[test%varied_indices.size()]=UINT16_MAX;
        const auto maximum=*std::max_element(varied_indices.begin(),varied_indices.end());
        const auto bounds_indices=std::span<const std::uint16_t>(&maximum,1);
        for(unsigned stream=0;stream<3;++stream) {
            const std::uint32_t stride=test%7==0?0:stream==0?8:stream==1?16:UINT32_MAX;
            const std::uint32_t fetch_bytes=test%5==0?0:test%11==0?stride+std::uint32_t(stride!=UINT32_MAX):stream==2?4:8;
            const auto needed=(std::uint64_t(maximum)+1)*stride;
            for(const auto declared:std::array<std::uint64_t,5>{0,needed?needed-1:0,needed,needed+1,UINT64_MAX}) {
                const auto original=UsedVertexPrefix(varied_indices,stride,fetch_bytes,declared);
                const auto once=UsedVertexPrefix(bounds_indices,stride,fetch_bytes,declared);
                assert(original==once);++prefix_checks;
            }
        }
    }
    std::printf("PASS: %u shared-index-maximum vs three-stream prefix checks.\n",prefix_checks);
    std::printf("PASS: %u boundary/truncation checks; 512 differential merged snapshots; no gap reads.\n",checks);
}
