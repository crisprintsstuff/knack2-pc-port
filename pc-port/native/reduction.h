// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <cstddef>
#define ISS_SYSV __attribute__((sysv_abi))
namespace iss::native {
struct ReductionBindings {
    std::uint8_t *reset;
    std::uint32_t *frames;
    std::uintptr_t *current,*next;
    void *dispatch_state;
    void (ISS_SYSV *enter)(void *);
    void (ISS_SYSV *leave)(void *);
    void (ISS_SYSV *dispatch)(void *,const void *,std::uint64_t,std::uint32_t,std::uint32_t,std::uint32_t,std::uint32_t,void *);
    float (ISS_SYSV *exponential)(float);
    const float *(*read)(std::uint64_t,std::size_t);
};
float ReduceOrdered(const float *values,std::uint32_t count);
std::uint8_t Reduce(const ReductionBindings&,const void *image_info,std::uint64_t argument,void *context,float *output);
}
