/* SPDX-License-Identifier: MIT */
#ifndef MINEW_COMPILER_COMPAT_H
#define MINEW_COMPILER_COMPAT_H
#if !defined(__ASSEMBLER__) && !defined(_ASMLANGUAGE)
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ == 12
/* GCC 12's arm_acle.h lacks the newer __rbit intrinsic. Cortex-M33 has RBIT. */
static __inline__ __attribute__((__always_inline__)) unsigned int minew_gcc12_rbit(unsigned int x) {
 unsigned int result;
 __asm__("rbit %0, %1" : "=r"(result) : "r"(x));
 return result;
}
#define __rbit minew_gcc12_rbit
#endif
#endif
#endif
