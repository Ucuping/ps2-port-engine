/*
 * The runtime's own tests (ps2rt alone, no host): built with every port,
 * beside the host's tests, which run the same checks.
 */

#include <cstdio>
#include <cstdlib>

#include "fp_mode_tests.h"

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    }
}

}  // namespace

int main() {
    ps2rt_tests::FloatModeTests(Check);
    std::printf(g_failures ? "runtime tests: %d failed\n" : "runtime tests: all passed\n", g_failures);
    return g_failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
