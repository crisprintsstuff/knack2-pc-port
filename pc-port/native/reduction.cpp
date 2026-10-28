// SPDX-License-Identifier: GPL-2.0-or-later
// Reconstructed from the identity-gated CUSA00004 routine at eboot+0x171610.
// Producer dispatch, context transitions, double buffering and expf stay original.
#include "reduction.h"
#include <cstring>
#include <immintrin.h>
namespace iss::native {
template<class T> static T Load(const void *p,std::size_t offset){T value;std::memcpy(&value,static_cast<const std::uint8_t*>(p)+offset,sizeof(value));return value;}
float ReduceOrdered(const float *values,std::uint32_t count){
    std::uint32_t i=0;float sum=0;
    if(count>=16){
        auto a=_mm256_setzero_ps(),b=_mm256_setzero_ps();
        const auto end=count&~std::uint32_t{15};
        for(;i<end;i+=16){a=_mm256_add_ps(a,_mm256_loadu_ps(values+i));b=_mm256_add_ps(b,_mm256_loadu_ps(values+i+8));}
        // Match the guest's two accumulators and horizontal reduction order.
        auto total=_mm256_add_ps(b,a);
        auto low=_mm_add_ps(_mm256_castps256_ps128(total),_mm256_extractf128_ps(total,1));
        low=_mm_add_ps(low,_mm_shuffle_ps(low,low,_MM_SHUFFLE(1,0,3,2)));
        sum=_mm_cvtss_f32(_mm_hadd_ps(low,low));
    }
    for(;i<count;++i)sum+=values[i];
    return sum;
}
std::uint8_t Reduce(const ReductionBindings& b,const void *image_info,std::uint64_t argument,void *context,float *output){
    const auto previous_mode=Load<std::int32_t>(context,0xde84);
    if(previous_mode!=1)b.enter(context);
    if(*b.reset)*b.frames=0;
    const auto frame=*b.frames;
    const auto dimensions=Load<std::uint32_t>(image_info,8);
    const auto width=(dimensions&0x3fff)+1, height=((dimensions>>14)&0x3fff)+1;
    const auto tiles_x=(width+127)/128,tiles_y=(height+71)/72;
    const auto current=*b.current;
    float sum=0;const bool available=frame>1;
    if(available){
        const auto *descriptor=reinterpret_cast<const void*>(current);
        const auto address=std::uint64_t(Load<std::uint32_t>(descriptor,4))|
            (std::uint64_t(Load<std::uint32_t>(descriptor,8)&0xfff)<<32);
        const auto count=tiles_x*tiles_y;
        sum=ReduceOrdered(b.read(address,std::size_t(count)*sizeof(float)),count);
    }
    *b.current=*b.next;*b.next=current;*b.frames=frame+1;
    b.dispatch(b.dispatch_state,image_info,argument,width,height,tiles_x,tiles_y,context);
    if(previous_mode!=1)b.leave(context);
    *output=b.exponential(sum);
    return available?1:0;
}
}
