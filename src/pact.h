#ifndef _pact_HEADER
#define _pact_HEADER

#include <stdio.h>
#include <numa.h>
#include <numaif.h>

#include "pebs.h"
#include "uthash.h"
#include "algorithm.h"

// #define FAST_SIZE (14 * (1024UL * 1024UL * 1024UL))
// #define REMOTE_SIZE (6 * (1024UL * 1024UL * 1024UL))

#define FAST_NODE 0
#define SLOW_NODE 1

// #define PAGE_SIZE 4096UL              // 4KB
// #define PAGE_SIZE (1 * (1024UL * 1024UL))
#ifndef PAGE_SIZE
#define PAGE_SIZE (2 * (1024UL * 1024UL)) // 2MB
#endif
// #define PAGE_SIZE (256 * 1024UL) // 256KB
#define BASE_PAGE_SIZE 4096UL

#define PAGE_MASK (~(PAGE_SIZE - 1))
#define BASE_PAGE_MASK (~(BASE_PAGE_SIZE - 1))

// Watermark thresholds (in bytes) - based on MEMTIS strategy
#define MIN_WATERMARK_LOWER_LIMIT   (128 * 100 * BASE_PAGE_SIZE)  // ~50MB
#define MIN_WATERMARK_UPPER_LIMIT   (2560 * 100 * BASE_PAGE_SIZE) // ~1000MB
#define MAX_WATERMARK_LOWER_LIMIT   (256 * 100 * BASE_PAGE_SIZE)  // ~100MB
#define MAX_WATERMARK_UPPER_LIMIT   (3840 * 100 * BASE_PAGE_SIZE) // ~1500MB

// Watermark percentages
#define DEMOTION_WATERMARK_PERCENT  2    // 2% for demotion threshold
#define PROMOTION_WATERMARK_PERCENT 3    // 3% for promotion threshold

// Use either FAST_BUFFER or FAST_SIZE
#ifndef FAST_BUFFER 
#define FAST_BUFFER (1 * 1024L * 1024L * 1024L)     // How much to leave available on FAST node
#endif

#ifndef FAST_SIZE
    #define FAST_SIZE (0)
    // #define FAST_SIZE (2 * 1024L * 1024L * 1024L)
#endif


extern struct fifo_list hot_list;
extern struct fifo_list cold_list;
extern struct fifo_list free_list;

extern long fast_free;
extern long fast_size;
extern long fast_used;
extern long slow_used;
extern long demotion_watermark;
extern long promotion_watermark;
extern pthread_mutex_t mmap_lock;
extern _Atomic bool fast_lock;

enum {
    IN_FAST,
    IN_SLOW
};

#ifndef MAX_NEIGHBORS
#define MAX_NEIGHBORS 4
#endif

struct pact_page;

struct neighbor_page {
    struct pact_page *page;
    double distance;
    uint64_t time_diff;
};

struct pact_page {
    pthread_mutex_t page_lock;

    uint64_t va;
#if PAGR_ALGO == 1
    uint64_t cyc;
    uint64_t ip;
#endif
    void* va_start;
    uint64_t size;
    uint64_t accesses;

#if HEM_ALGO == 1
    uint64_t local_clock;
#endif

    uint64_t mig_start;

    UT_hash_handle hh;
    struct pact_page *next, *prev;
#if PAGR_ALGO == 1
    struct neighbor_page neighbors[MAX_NEIGHBORS];
#endif
    struct fifo_list *list;

    uint64_t pagr_pred_time;
    uint64_t hem_pred_time;

    // Page states
    _Atomic uint8_t in_fast;
    _Atomic bool free;
    // Prediction states
    unsigned int pagr_pred : 1;
    unsigned int hem_pred : 1;
    unsigned int pagr_accessed : 1;
    unsigned int hem_accessed : 1;
    unsigned int real_pred : 1;
    unsigned int real_accessed : 1;
};

// Watermark calculation functions (inspired by MEMTIS strategy)
long pact_get_demotion_watermark(long max_bytes);
long pact_get_promotion_watermark(long max_bytes);
void pact_update_watermarks(void);

void pact_init();
void* pact_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
int pact_munmap(void *addr, size_t length);
void pact_cleanup();
struct pact_page* find_page(uint64_t va);
struct pact_page* find_page_no_lock(uint64_t va);

#endif