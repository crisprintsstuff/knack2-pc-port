// SPDX-License-Identifier: GPL-2.0-or-later
#include "../gpu/page_write_provenance.h"
#include <cstdio>
#include <cstdlib>
#include <random>
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"PAGE PROVENANCE FAIL %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
int main(){
    iss::readback::PageWriteProvenance<128> ledger;
    std::array<std::uint64_t,2> result{};
    CHECK(!ledger.Merge(0,result));
    CHECK(ledger.Publish(0,0,0,100));
    CHECK(ledger.Publish(1,1,0,102)); // same 256 KiB word; must NOT affect page0.
    CHECK(ledger.Merge(0,result)&&result[0]==100&&result[1]==0);
    CHECK(ledger.Publish(0,0,1,55)); // disjoint bytes on another queue remain a dependency.
    result={};CHECK(ledger.Merge(0,result)&&result[0]==100&&result[1]==55);
    CHECK(!ledger.Publish(0,128,0,200));CHECK(!ledger.Publish(2,1,0,200));
    CHECK(!ledger.Publish(0,0,2,200));CHECK(!ledger.Publish(0,0,0,0));
    iss::readback::PageWriteProvenance<128> random_ledger;
    std::array<std::array<std::uint64_t,2>,128> expected{};
    std::mt19937 random(714);
    for(unsigned i=0;i<20000;++i){
        auto first=random()%128,last=random()%128;if(first>last)std::swap(first,last);
        const auto queue=random()%2;const std::uint64_t tick=random()%10000+1;
        CHECK(random_ledger.Publish(first,last,queue,tick));
        for(auto p=first;p<=last;++p)expected[p][queue]=std::max(expected[p][queue],tick);
        const auto page=random()%128;result={};
        CHECK(random_ledger.Merge(page,result)==bool(expected[page][0]||expected[page][1]));
        CHECK(result==expected[page]);
    }
    std::puts("Page provenance: neighbouring-page isolation, both queues, unknown fallback and 20000 randomized writes passed");
}
