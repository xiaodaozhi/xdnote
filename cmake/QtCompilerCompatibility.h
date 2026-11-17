#pragma once
// Qt 6.8.2 qyieldcpu.h uses __yield when AppleClang reports it as a builtin.
// New AppleClang requires its public ACLE declaration before that Qt header.
// Keep the fixed SDK byte-identical; apply only to the actual ARM64 target.
#if defined(__APPLE__) && defined(__aarch64__) && defined(__clang__)
#include <arm_acle.h>
#endif
