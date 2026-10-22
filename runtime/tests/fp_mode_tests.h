/*
 * The float-mode tests, shared by ps2rt_tests (the runtime alone) and the
 * host's ps2host_tests: each reports through the caller's check.
 */

#ifndef PS2RT_FP_MODE_TESTS_H
#define PS2RT_FP_MODE_TESTS_H

namespace ps2rt_tests {

using Check = void (*)(bool ok, const char* what);

// The host's FPU in the VUs' mode and out of it; macro mode's COP2; the EE's
// FPU - each against results pinned from the hardware's definition.
void FloatModeTests(Check check);

}  // namespace ps2rt_tests

#endif  // PS2RT_FP_MODE_TESTS_H
