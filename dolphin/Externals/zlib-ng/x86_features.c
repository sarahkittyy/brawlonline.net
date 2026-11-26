/* x86_features.c - x86 feature check
 *
 * Copyright (C) 2013 Intel Corporation. All rights reserved.
 * Author:
 *  Jim Kukunas
 *
 * For conditions of distribution and use, see copyright notice in zlib.h
 *
 * Brawl Online: this replaces zlib-ng/arch/x86/x86_features.c (see CMakeLists.txt here).
 * That copy turns AVX2 and AVX-512 on from CPUID alone. A CPU can report AVX-512 while
 * Windows has not enabled its ZMM register state (XCR0), and then the first AVX-512
 * instruction raises 0xc000001d, illegal instruction: bug report #362, a Ryzen 7 8700F
 * whose Dolphin crashed in crc32_fold_vpclmulqdq at every boot. Upstream zlib-ng checks
 * XCR0 since 2.1; this does the same.
 */

#include "zlib-ng/zbuild.h"

#ifdef _MSC_VER
#  include <intrin.h>
#else
// Newer versions of GCC and clang come with cpuid.h
#  include <cpuid.h>
#endif

#include <string.h>

Z_INTERNAL int x86_cpu_has_avx2;
Z_INTERNAL int x86_cpu_has_avx512;
Z_INTERNAL int x86_cpu_has_avx512vnni;
Z_INTERNAL int x86_cpu_has_sse2;
Z_INTERNAL int x86_cpu_has_ssse3;
Z_INTERNAL int x86_cpu_has_sse41;
Z_INTERNAL int x86_cpu_has_sse42;
Z_INTERNAL int x86_cpu_has_pclmulqdq;
Z_INTERNAL int x86_cpu_has_vpclmulqdq;
Z_INTERNAL int x86_cpu_has_tzcnt;

static void cpuid(int info, unsigned* eax, unsigned* ebx, unsigned* ecx, unsigned* edx) {
#ifdef _MSC_VER
    unsigned int registers[4];
    __cpuid((int *)registers, info);

    *eax = registers[0];
    *ebx = registers[1];
    *ecx = registers[2];
    *edx = registers[3];
#else
    __cpuid(info, *eax, *ebx, *ecx, *edx);
#endif
}

static void cpuidex(int info, int subinfo, unsigned* eax, unsigned* ebx, unsigned* ecx, unsigned* edx) {
#ifdef _MSC_VER
    unsigned int registers[4];
    __cpuidex((int *)registers, info, subinfo);

    *eax = registers[0];
    *ebx = registers[1];
    *ecx = registers[2];
    *edx = registers[3];
#else
    __cpuid_count(info, subinfo, *eax, *ebx, *ecx, *edx);
#endif
}

/* XCR0: the register states the OS saves on a context switch. Only call it when CPUID
 * reports OSXSAVE. */
static unsigned xgetbv0(void) {
#ifdef _MSC_VER
    return (unsigned)_xgetbv(0);
#else
    unsigned eax, edx;
    __asm__ volatile("xgetbv" : "=a"(eax), "=d"(edx) : "c"(0));
    (void)edx;
    return eax;
#endif
}

void Z_INTERNAL x86_check_features(void) {
    unsigned eax, ebx, ecx, edx;
    unsigned maxbasic;
    int has_os_save_ymm = 0;
    int has_os_save_zmm = 0;

    cpuid(0, &maxbasic, &ebx, &ecx, &edx);
    cpuid(1 /*CPU_PROCINFO_AND_FEATUREBITS*/, &eax, &ebx, &ecx, &edx);

    x86_cpu_has_sse2 = edx & 0x4000000;
    x86_cpu_has_ssse3 = ecx & 0x200;
    x86_cpu_has_sse41 = ecx & 0x80000;
    x86_cpu_has_sse42 = ecx & 0x100000;
    x86_cpu_has_pclmulqdq = ecx & 0x2;

    if (ecx & 0x8000000) { /* OSXSAVE */
        unsigned xcr0 = xgetbv0();
        /* XMM and YMM state */
        has_os_save_ymm = (xcr0 & 0x06) == 0x06;
        /* and the AVX-512 opmask, ZMM0-15 upper halves and ZMM16-31 */
        has_os_save_zmm = (xcr0 & 0xe6) == 0xe6;
    }

    if (maxbasic >= 7) {
        cpuidex(7, 0, &eax, &ebx, &ecx, &edx);

        // check BMI1 bit
        // Reference: https://software.intel.com/sites/default/files/article/405250/how-to-detect-new-instruction-support-in-the-4th-generation-intel-core-processor-family.pdf
        x86_cpu_has_tzcnt = ebx & 0x8;
        // check AVX2 bit
        x86_cpu_has_avx2 = has_os_save_ymm && (ebx & 0x20);
        x86_cpu_has_avx512 = has_os_save_zmm && (ebx & 0x00010000);
        x86_cpu_has_avx512vnni = x86_cpu_has_avx512 && (ecx & 0x800);
        x86_cpu_has_vpclmulqdq = has_os_save_ymm && (ecx & 0x400);
    } else {
        x86_cpu_has_tzcnt = 0;
        x86_cpu_has_avx2 = 0;
        x86_cpu_has_avx512 = 0;
        x86_cpu_has_avx512vnni = 0;
        x86_cpu_has_vpclmulqdq = 0;
    }
}
