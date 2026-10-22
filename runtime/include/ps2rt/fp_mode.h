/*
 * The host's floating-point mode for the VUs' arithmetic.
 *
 * The VUs - and VU0 in macro mode - round toward zero and take denormals as
 * zero. The host's FPU does the same for a scope: VuFloatMode sets it and puts
 * back what was there before.
 *
 * x86: MXCSR's rounding control toward zero, FTZ and DAZ - the calls ps2host's
 * Vu::Run and macro mode made before this header.
 * arm64: FPCR's RMode toward zero and FZ. FZ flushes denormal inputs and
 * results alike, which is FTZ and DAZ together; with rounding toward zero,
 * x86's tininess (after rounding) and arm64's (before rounding) agree.
 *
 * Everything else - the EE's FPU (eefpu.cpp) above all - runs in the thread's
 * default: to nearest, denormals kept. Engine threads start that way.
 */

#ifndef PS2RT_FP_MODE_H
#define PS2RT_FP_MODE_H

#include <cstdint>

#if defined(__aarch64__)
#define PS2RT_FP_FPCR 1
#elif defined(__SSE__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 1)
#include <xmmintrin.h>
#define PS2RT_FP_MXCSR 1
#else
#error "ps2rt: no way to set the VUs' float mode (round toward zero, denormals as zero) on this processor"
#endif

namespace ps2rt {

#if PS2RT_FP_MXCSR
using FloatControl = unsigned;

inline FloatControl ReadFloatControl() { return _mm_getcsr(); }
inline void WriteFloatControl(FloatControl value) { _mm_setcsr(value); }
// Rounding control 11 (toward zero), FTZ (bit 15), DAZ (bit 6).
inline FloatControl VuFloatControl(FloatControl saved) {
    return (saved & ~0x6000u) | 0x6000u | 0x8000u | 0x0040u;
}
#else
using FloatControl = std::uint64_t;

inline FloatControl ReadFloatControl() {
    std::uint64_t value;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(value));
    return value;
}
// A write of FPCR can hold up the pipeline, so one that changes nothing - a
// scope inside another - is not made.
inline void WriteFloatControl(FloatControl value) {
    if (value != ReadFloatControl()) __asm__ __volatile__("msr fpcr, %0" : : "r"(value) : "memory");
}
// RMode (bits 23:22) 11 (toward zero), FZ (bit 24); DN, FZ16 and AH untouched.
inline FloatControl VuFloatControl(FloatControl saved) {
    return (saved & ~(std::uint64_t(3) << 22)) | (std::uint64_t(3) << 22) | (std::uint64_t(1) << 24);
}
#endif

// The VUs' float mode for a scope. `set` false leaves the mode alone (macro
// mode under PS2RT_EE_ROUND_NEAREST, as verification runs it).
class VuFloatMode {
public:
    explicit VuFloatMode(bool set = true) : saved_(ReadFloatControl()), set_(set) {
        if (set_) WriteFloatControl(VuFloatControl(saved_));
    }
    ~VuFloatMode() {
        if (set_) WriteFloatControl(saved_);
    }
    VuFloatMode(const VuFloatMode&) = delete;
    VuFloatMode& operator=(const VuFloatMode&) = delete;

private:
    FloatControl saved_;
    bool set_;
};

}  // namespace ps2rt

#endif  // PS2RT_FP_MODE_H
