#include "timer.h"

inline uint64_t get_time() {
    return rdtscp();
}

inline double elapsed_time(uint64_t start, uint64_t end) {
    return (double)(end - start) / (double)CPU_FREQ;
}

uint64_t rdtscp(void) {
    uint32_t eax, edx;
    // why is "ecx" in clobber list here, anyway? -SG&MH,2017-10-05
    __asm volatile ("rdtscp" : "=a" (eax), "=d" (edx) :: "ecx", "memory");
    return ((uint64_t)edx << 32) | eax;
}