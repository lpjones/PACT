#ifndef _TIMER_HEADER
#define _TIMER_HEADER

#ifndef CPU_FREQ
    #define CPU_FREQ 2400000000
#endif

#include <time.h>
#include <stdio.h>
#include <assert.h>
#include <stdint.h>

inline double elapsed_time(uint64_t start, uint64_t end) {
    return (double)(end - start) / (double)CPU_FREQ;
}

inline uint64_t rdtscp(void) {
    uint32_t eax, edx;
    // why is "ecx" in clobber list here, anyway? -SG&MH,2017-10-05
    __asm volatile ("rdtscp" : "=a" (eax), "=d" (edx) :: "ecx", "memory");
    return ((uint64_t)edx << 32) | eax;
}

#endif