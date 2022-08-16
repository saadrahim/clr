/* Copyright (c) 2015 - 2021 Advanced Micro Devices, Inc.

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE. */

namespace pal {

#define BLIT_KERNEL(...) #__VA_ARGS__

const char* SchedulerSourceCode = BLIT_KERNEL(
\n
extern void __amd_scheduler(__global void*, __global void*, uint);
\n
__kernel void __amd_rocclr_scheduler(__global void* queue, __global void* params, uint paramIdx) {
  __amd_scheduler(queue, params, paramIdx);
}
\n);

const char* SchedulerSourceCode20 = BLIT_KERNEL(
\n
extern void __amd_scheduler_pal(__global void*, __global void*, uint);
\n
 __kernel void __amd_rocclr_scheduler(__global void* queue, __global void* params,
                                         uint paramIdx) {
  __amd_scheduler_pal(queue, params, paramIdx);
}
\n);

const char* GwsInitSourceCode = BLIT_KERNEL(
\n
extern void __ockl_gws_init(uint nwm1, uint rid);
\n
__kernel void __amd_rocclr_gwsInit(uint value) {
  __ockl_gws_init(value, 0);
}
\n);

}  // namespace pal
