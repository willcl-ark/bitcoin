// Copyright (c) 2025-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/coverage.h>

#if defined(__clang__)
extern "C" __attribute__((weak)) void __llvm_profile_reset_counters(void);

// Fallback implementations
extern "C" __attribute__((weak)) void __llvm_profile_reset_counters(void) {}

void ResetCoverageCounters() {
    // This will call the real one if available, or our dummy if not.
    __llvm_profile_reset_counters();
}
#else
void ResetCoverageCounters() {}
#endif
