#ifndef _PEBS_HEADER
#define _PEBS_HEADER

#ifndef __USE_GNU
    #define __USE_GNU
#endif
#ifndef _GNU_SOURCE
    #define _GNU_SOURCE
#endif
#include <stdio.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <assert.h>
#include <unistd.h>
#include <time.h>
#include <linux/perf_event.h>
#include <sys/syscall.h>
#include <string.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <sched.h>
#include <sys/ioctl.h>

#include "timer.h"
#include "interpose.h"
#include "logging.h"
#include "spsc-ring.h"
#include "fifo.h"


#ifndef NO_SAMPLE_RESET_TIME
    #define NO_SAMPLE_RESET_TIME (CPU_FREQ >> 5)
#endif

#ifndef PEBS_SCAN_CPU
    #define PEBS_SCAN_CPU 2
#endif

#ifndef PEBS_STATS_CPU
    #define PEBS_STATS_CPU 4
#endif

#ifndef PROMOTE_CPU
    #define PROMOTE_CPU 6
#endif

#ifndef DEMOTE_CPU
    #define DEMOTE_CPU 8
#endif

#ifndef SAMPLE_PERIOD
    #define SAMPLE_PERIOD 3200
#endif

// Dynamic PEBS sampling parameters (similar to memtis)
#define PACT_SAMPLE_PERIOD_MIN_RATIO 50  // 50%
#define PACT_SAMPLE_PERIOD_MAX_RATIO 10  // 10%
#define PACT_CPU_QUOTA 3  // 3% target CPU usage
#define PACT_CPU_CHECK_PERIOD_MS 1000  // 1 second
#define PACT_SAMPLE_SLEEP_US 2000  // 2ms between sample processing iterations

// PEBS period lists (prime numbers like memtis)
#define PCOUNT 30
static const unsigned int pebs_period_list[PCOUNT] = {
    199,    // 200 - min
    293,    // 300
    401,    // 400
    499,    // 500
    599,    // 600
    701,    // 700
    797,    // 800
    907,    // 900
    997,    // 1000
    1201,   // 1200
    1399,   // 1400
    1601,   // 1600
    1801,   // 1800
    1999,   // 2000
    2503,   // 2500
    3001,   // 3000
    3499,   // 3500
    4001,   // 4000
    4507,   // 4507
    4999,   // 5000
    6007,   // 6000
    7001,   // 7000
    7993,   // 8000
    9001,   // 9000
    10007,  // 10000
    12007,  // 12000
    13999,  // 14000
    16001,  // 16000
    17989,  // 18000
    19997,  // 20000 - max
};

#ifndef PERF_PAGES
    #define PERF_PAGES (1 + (1 << 5))  // Uses 8GB total for 16 CPUs
#endif

#ifndef PEBS_MAX_SAMPLE_RATIO
    #define PEBS_MAX_SAMPLE_RATIO 0.1  // Process samples until buffer is 10% full
#endif

#ifndef PEBS_NPROCS
    #define PEBS_NPROCS 16
#endif

#ifndef HOT_THRESHOLD
    #define HOT_THRESHOLD 8
#endif

#ifndef C_HOT_THRESHOLD
    #define C_HOT_THRESHOLD 4
#endif

#ifndef SAMPLE_COOLING_THRESHOLD
    #define SAMPLE_COOLING_THRESHOLD 100000
#endif

#ifndef CYC_COOL_THRESHOLD
    #define CYC_COOL_THRESHOLD 10000000
#endif

#ifndef LRU_ALGO
    #define LRU_ALGO 0
#endif

enum {
    PEBS_THREAD,
    PEBS_STATS_THREAD,
    PROMOTE_THREAD,
    DEMOTE_THREAD,
    NUM_INTERNAL_THREADS
};

enum {
    PAGR_MODE = 0,
    HEM_MODE = 1,
    NUM_MODES
};

enum pbuftype {
  FASTREAD = 0,
  REMREAD = 1,  
  NPBUFTYPES
};

struct pebs_rec {
  uint64_t cyc;
  uint64_t va;
  uint64_t ip;
  uint32_t cpu;
  uint8_t  evt;
} __attribute__((packed));

struct pebs_stats {
    uint64_t throttles, unthrottles;
    uint64_t internal_mem_overhead, mem_allocated;
    uint64_t unknown_samples;
    uint64_t wrapped_records;
    uint64_t wrapped_headers;
    uint64_t fast_accesses, slow_accesses;
    uint64_t promotions, demotions;
    uint64_t pebs_resets;
    uint64_t non_tracked_mem;
    uint64_t mig_failed;
};

extern struct pebs_stats pebs_stats;
extern uint64_t real_promotions;
extern uint64_t real_prom_not_accessed;

// Dynamic PEBS sampling variables
extern unsigned int pact_sample_period_idx;
extern unsigned int pact_cpu_quota;
extern _Atomic bool dynamic_pebs_enabled;

// Dynamic PEBS sampling functions
void pact_update_sample_period(uint64_t new_period);
unsigned int pact_get_sample_period(unsigned int idx);
void pact_increase_sample_period(void);
void pact_decrease_sample_period(void);

void pebs_init();
void start_pebs_thread();
void wait_for_threads();
void kill_threads();

#endif