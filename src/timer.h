#ifndef _TIMER_HEADER
#define _TIMER_HEADER

#ifndef CPU_FREQ
    #define CPU_FREQ 2400000000
#endif

#include <time.h>
#include <stdio.h>
#include <assert.h>
#include <stdint.h>

uint64_t get_time(void);
double elapsed_time(uint64_t start, uint64_t end);
uint64_t rdtscp(void);

#endif