#include "pact.h"

struct pact_page *pages = NULL;
struct fifo_list hot_list;
struct fifo_list cold_list;
struct fifo_list free_list;
pthread_mutex_t pages_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mmap_lock = PTHREAD_MUTEX_INITIALIZER;

long fast_free = 0;
long fast_size = 0;
long fast_used = 0;
long slow_used = 0;

static uint64_t max_pact_va = 0;
static uint64_t min_pact_va = UINT64_MAX;

#define RAM_SIZE (64UL * 1024UL * 1024UL * 1024UL)   // 64GB
#define MAX_PAGES (RAM_SIZE / PAGE_SIZE)
#define LOG2_PAGE_SIZE (__builtin_ctzl(PAGE_SIZE))
#define PAGE_LOOKUP(va) (((va) >> LOG2_PAGE_SIZE) % MAX_PAGES)

static struct pact_page pact_page_table[MAX_PAGES] = {0};

_Atomic bool fast_lock = false;

// DOES NOT UNLOCK PAGE, caller is responsible for unlocking
struct pact_page* find_page(uint64_t va) {
    uint64_t idx = PAGE_LOOKUP(va);

    pthread_mutex_lock(&pact_page_table[idx].page_lock);

    // Find matching va in table (handle collisions with linear probing)
    while (!pact_page_table[idx].free && pact_page_table[idx].va != va) {
        pthread_mutex_unlock(&pact_page_table[idx].page_lock);
        idx = (idx + 1) % MAX_PAGES;
        pthread_mutex_lock(&pact_page_table[idx].page_lock);
    }
    if (!pact_page_table[idx].free && pact_page_table[idx].va == va) {
        return &pact_page_table[idx];   // DOES NOT UNLOCK PAGE, caller is responsible for unlocking
    }
    pthread_mutex_unlock(&pact_page_table[idx].page_lock);
    return NULL;

}

void add_page(uint64_t va, uint8_t in_fast) {
    uint64_t idx = PAGE_LOOKUP(va);

    pthread_mutex_lock(&pact_page_table[idx].page_lock);

    // Find free page in table
    while (!pact_page_table[idx].free) {
        LOG_DEBUG("add_page: duplicate page: 0x%lx\n", va);
        pthread_mutex_unlock(&pact_page_table[idx].page_lock);
        idx = (idx + 1) % MAX_PAGES;
        pthread_mutex_lock(&pact_page_table[idx].page_lock);
    }

    // Initialize page
    pact_page_table[idx].va = va;
#if PAGR_ALGO == 1
    pact_page_table[idx].cyc = 0;
    pact_page_table[idx].ip = 0;
#endif
    pact_page_table[idx].accesses = 0;

#if HEM_ALGO == 1
    pact_page_table[idx].local_clock = 0;
#endif

    pact_page_table[idx].mig_start = 0;

#if PAGR_ALGO == 1
    memset(pact_page_table[idx].neighbors, 0, MAX_NEIGHBORS * sizeof(struct neighbor_page));
#endif
    if (in_fast == IN_FAST) {
        enqueue_fifo(&cold_list, &pact_page_table[idx]);
    }

    pact_page_table[idx].pagr_pred_time = 0;
    pact_page_table[idx].hem_pred_time = 0;

    pact_page_table[idx].in_fast = in_fast;
    pact_page_table[idx].free = false;

    pact_page_table[idx].pagr_pred = 0;
    pact_page_table[idx].hem_pred = 0;
    pact_page_table[idx].pagr_accessed = 0;
    pact_page_table[idx].hem_accessed = 0;
    pact_page_table[idx].real_pred = 0;
    pact_page_table[idx].real_accessed = 0;

    pthread_mutex_unlock(&pact_page_table[idx].page_lock);
    LOG_DEBUG("Added page: 0x%lx, in_fast: %d\n", va, in_fast);
}

struct pact_page* find_page_no_lock(uint64_t va) {
    uint64_t idx = PAGE_LOOKUP(va);
    uint64_t start_idx = idx;

    while (true) {
        if (pact_page_table[idx].free) {
            return NULL;
        }
        if (pact_page_table[idx].va == va) {
            if (pthread_mutex_trylock(&pact_page_table[idx].page_lock) != 0) {
                return NULL;    // Abort early so no waiting
            }
            if (pact_page_table[idx].free || pact_page_table[idx].va != va) {
                pthread_mutex_unlock(&pact_page_table[idx].page_lock);
                return NULL;
            }
            return &pact_page_table[idx];   // DOES NOT UNLOCK PAGE, caller is responsible for unlocking
        }
        idx = (idx + 1) % MAX_PAGES;
        if (idx == start_idx) {
            return NULL;
        }
    }
}


void pact_init() {
    internal_call = true;
#if (FAST_BUFFER != 0 && FAST_SIZE != 0) || (FAST_BUFFER == 0 && FAST_SIZE == 0)
    fprintf(stderr, "Can't have both FAST_BUFFER and FAST_SIZE\n");
    exit(1);
#endif

    // Puts non-tracked mmaps into slow memory so it doesn't exceed
    // the set FAST capacity
    numa_set_preferred(FAST_NODE);

    LOG_DEBUG("pact_page_table size: %lu\n", sizeof(pact_page_table));
    LOG_DEBUG("pact_page size: %lu\n", sizeof(struct pact_page));
    LOG_DEBUG("pact_page_table num pages: %lu\n", sizeof(pact_page_table) / sizeof(struct pact_page));

    // Initialize all locks and all pages as free
    for (uint64_t i = 0; i < RAM_SIZE / PAGE_SIZE; i++) {
        pact_page_table[i].free = true;
        pthread_mutex_init(&pact_page_table[i].page_lock, NULL);
    }

    LOG_DEBUG("finished pact_init\n");

    

    // check how much free space on fast
#if FAST_BUFFER != 0
    fast_size = numa_node_size(FAST_NODE, &fast_free);
    fast_used = fast_size - fast_free;
#endif
#if FAST_SIZE != 0
    fast_size = FAST_SIZE;
#endif
    internal_call = false;
}

#define PAGE_ROUND_UP(x) (((x) + (PAGE_SIZE)-1) & (~((PAGE_SIZE)-1)))
#define PAGE_ROUND_DOWN(x) ((x) & (~((PAGE_SIZE)-1)))

#define PAGE_ROUND_UP_BASE(x) (((x) + (BASE_PAGE_SIZE)-1) & (~((BASE_PAGE_SIZE)-1)))


void* pact_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
    length = PAGE_ROUND_UP_BASE(length);
    internal_call = true;

    unsigned long fast_nodemask = 1UL << FAST_NODE;
    unsigned long slow_nodemask = 1UL << SLOW_NODE;
    void *p_fast = NULL, *p_slow = NULL;

    /* Allocating Memory to return to user program */

    void *p = libc_mmap(addr, length, prot, flags & ~MAP_POPULATE, fd, offset); // Remove MAP_POPULATE until after mbind to prevent faulting and then migrating
    assert(p != MAP_FAILED);

    pthread_mutex_lock(&mmap_lock);
    LOG_DEBUG("fast_used: %lu, length: %lu, fast_size: %lu, fast_lock %d\n", __atomic_load_n(&fast_used, __ATOMIC_ACQUIRE), length, fast_size, atomic_load_explicit(&fast_lock, memory_order_acquire));
    if (__atomic_load_n(&fast_used, __ATOMIC_ACQUIRE) + length <= fast_size 
        && atomic_load_explicit(&fast_lock, memory_order_acquire) == false) {
        // can allocate all on fast
        __atomic_fetch_add(&fast_used, length, __ATOMIC_RELEASE);
        // fast_used += length;
        pthread_mutex_unlock(&mmap_lock);
        LOG_DEBUG("MMAP: All FAST\n");


        if (mbind(p, length, MPOL_PREFERRED, &fast_nodemask, 64, MPOL_MF_MOVE | MPOL_MF_STRICT)) {
            perror("mbind");
            assert(0);
        }
        
        p_fast = p;
        p_slow = p_fast + length + 1;    // Used later to check which node page is in
    } else if (fast_used + PAGE_SIZE > fast_size || atomic_load_explicit(&fast_lock, memory_order_acquire)) {
        pthread_mutex_unlock(&mmap_lock);
        LOG_DEBUG("MMAP: All Remote\n");
        // fast full, all on slow
        if (mbind(p, length, MPOL_PREFERRED, &slow_nodemask, 64, MPOL_MF_MOVE | MPOL_MF_STRICT)) {
            perror("mbind");
            assert(0);
        }
        p_slow = p;
    } else {
        // split between fast and slow
        uint64_t fast_mmap_size = PAGE_ROUND_DOWN(fast_size - fast_used);
        // fast_used += fast_mmap_size;
        __atomic_fetch_add(&fast_used, fast_mmap_size, __ATOMIC_RELEASE);
        pthread_mutex_unlock(&mmap_lock);
        
        uint64_t slow_mmap_size = length - fast_mmap_size;


        LOG_DEBUG("MMAP: fast: %lu, slow: %lu\n", fast_mmap_size, slow_mmap_size);
        p_fast = p;
        p_slow = p_fast + fast_mmap_size;
        if (mbind(p_fast, fast_mmap_size, MPOL_PREFERRED, &fast_nodemask, 64, MPOL_MF_MOVE | MPOL_MF_STRICT) == -1) {
            perror("mbind");
            assert(0);
        }
        if (mbind(p_slow, slow_mmap_size, MPOL_PREFERRED, &slow_nodemask, 64, MPOL_MF_MOVE | MPOL_MF_STRICT) == -1) {
            perror("mbind");
            assert(0);
        }
        
    }

    if (flags & MAP_POPULATE) {
        size_t pagesize = sysconf(_SC_PAGESIZE);
        for (size_t i = 0; i < length; i += pagesize) {
            volatile char *addr = (char*)p + i;
            *addr = 0;   // trigger page fault
        }
    }
    
    // LOG_DEBUG("fast_size: %ld, fast_free: %ld\n", fast_size, fast_free);
    if (p == MAP_FAILED) {
        LOG_DEBUG("mmap failed\n");
        return MAP_FAILED;
    }
    pebs_stats.mem_allocated += length;

    assert((uint64_t)p % BASE_PAGE_SIZE == 0);

    /* End of User program Memory allocation */
    /* Add pages to pact_page_table */

    assert(length >= PAGE_SIZE);

    // Split allocation into PAGE_SIZE chunks and add to pact_page_table
    for (uint64_t i = 0; i < length; i += PAGE_SIZE) {
        uint64_t va = (uint64_t)p + i;
        uint8_t in_fast = (va >= (uint64_t)p_slow) ? IN_SLOW : IN_FAST;
        add_page(va, in_fast);
    }


    internal_call = false;
    return p;
}

int pact_munmap(void *addr, size_t length) {
    internal_call = true;
    LOG_DEBUG("pact_munmap: %p, length: %lu\n", addr, length);
    LOG_DEBUG("pact va range: 0x%lx - 0x%lx\n", min_pact_va, max_pact_va);

    uint64_t num_pact_pages = (length + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint64_t i = 0; i < num_pact_pages; i++) {
        void *va_start = addr + (i * PAGE_SIZE);
        uint64_t va;
        if (length - (i * PAGE_SIZE) < PAGE_SIZE) {
            va = (uint64_t)(va_start);
        } else {
            va = PAGE_ROUND_UP((uint64_t)(va_start));
        }
        struct pact_page *page = find_page(va);
        if (page != NULL) {
            assert(page->free == false);
            page->free = true;

            pebs_stats.mem_allocated -= PAGE_SIZE;

            if (page->list != NULL) {
                page_list_remove_page(page->list, page);
            }

            pthread_mutex_unlock(&page->page_lock);
        }
    }
    internal_call = false;
    return 0;
}

void pact_cleanup() {
    kill_threads();
    // TODO: unmap pages (very difficult since libc_munmap works on 4KB and will unmap multiple pages at a time if in same region)
    wait_for_threads();
}