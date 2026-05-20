#ifndef _pact_HEADER
#define _pact_HEADER

#include <stdio.h>
#include <numa.h>
#include <numaif.h>
#include <math.h>

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

// Skewness detection parameters (inspired by MEMTIS)
#define SKEWNESS_SCALE_FACTOR 11  // Scale down factor for skewness calculation
#define SKEWNESS_THRESHOLD_HIGH 13  // Very hot pages threshold
#define SKEWNESS_THRESHOLD_LOW 0   // Uniform access threshold

// Subpage splitting parameters
#define SUBPAGE_SIZE BASE_PAGE_SIZE  // 4KB subpages
#define SUBPAGES_PER_PAGE (PAGE_SIZE / SUBPAGE_SIZE)  // Number of subpages per page
#define SKEWNESS_SPLIT_THRESHOLD 8.0  // Skewness threshold for splitting
#define MIN_HOT_SUBPAGES_FOR_SPLIT 2  // Minimum hot subpages to consider splitting

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
    
    // Skewness tracking (inspired by MEMTIS)
    double skewness;           // Calculated skewness index
    uint64_t access_sum_sq;    // Sum of squares for variance calculation
    uint64_t access_count;     // Total access count for this page

    // Subpage splitting support (MEMTIS-inspired)
    bool is_split;             // Whether this page has been split into subpages
    uint64_t *subpage_accesses; // Access counts for each 4KB subpage
    bool *subpage_hot;         // Hot status for each subpage
    uint32_t num_hot_subpages; // Number of hot subpages

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

// Skewness calculation functions (inspired by MEMTIS strategy)
double pact_calculate_skewness(struct pact_page *page);
void pact_update_skewness(struct pact_page *page);
double pact_get_skewness_threshold_adjustment(double skewness);

// Subpage splitting functions (MEMTIS-inspired)
void pact_init_subpages(struct pact_page *page);
void pact_update_subpage_access(struct pact_page *page, uint64_t offset);
bool pact_should_split_page(struct pact_page *page);
void pact_split_page(struct pact_page *page);
void pact_cleanup_subpages(struct pact_page *page);
uint32_t pact_get_hot_subpage_count(struct pact_page *page);

void pact_init();
void* pact_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
int pact_munmap(void *addr, size_t length);
void pact_cleanup();
struct pact_page* find_page(uint64_t va);
struct pact_page* find_page_no_lock(uint64_t va);

#endif