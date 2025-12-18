#include "pact.h"

struct pact_page *pages = NULL;
struct fifo_list hot_list;
struct fifo_list cold_list;
struct fifo_list free_list;
pthread_mutex_t pages_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mmap_lock = PTHREAD_MUTEX_INITIALIZER;

long dram_free = 0;
long dram_size = 0;
long dram_used = 0;
long rem_used = 0;

static uint64_t max_pact_va = 0;
static uint64_t min_pact_va = UINT64_MAX;

_Atomic bool dram_lock = false;

static __attribute__((constructor)) void init(void)
{
    init_log_files();
    LOG_DEBUG("ENV Variables:\n");
    LOG_DEBUG("\tpebs.h:\n"
                "\t\tSAMPLE_PERIOD:\t%d\n"
                "\t\tPEBS_SCAN_CPU:\t%d\n"
                "\t\tPEBS_STATS_CPU:\t%d\n"
                "\t\tMIGRATE_CPU:\t%d\n"
                "\t\tNO_SAMPLE_RESET_TIME:\t%d\n"
                "\t\tPERF_PAGES:\t%d\n"
                "\t\tPEBS_NPROCS:\t%d\n"
                "\t\tHOT_THRESHOLD:\t%d\n"
                "\t\tSAMPLE_COOLING_THRESHOLD:\t%d\n"
                "\t\tCYC_COOL_THRESHOLD:\t%d\n"
                "\t\tLRU_ALGO:\t%d\n",
                SAMPLE_PERIOD,
                PEBS_SCAN_CPU,
                PEBS_STATS_CPU,
                MIGRATE_CPU,
                NO_SAMPLE_RESET_TIME,
                PERF_PAGES,
                PEBS_NPROCS,
                HOT_THRESHOLD,
                SAMPLE_COOLING_THRESHOLD,
                CYC_COOL_THRESHOLD,
                LRU_ALGO);
    
    LOG_DEBUG("\tpact.h\n"
                "\t\tDRAM_BUFFER:\t%ld\n"
                "\t\tDRAM_NODE:\t%d\n"
                "\t\tREM_NODE:\t%d\n"
                "\t\tPAGE_SIZE:\t%d\n"
                "\t\tBASE_PAGE_SIZE:\t%lu\n"
                "\t\tPAGE_MASK:\t%x\n"
                "\t\tBASE_PAGE_MASK:\t%lx\n"
                "\t\tMAX_NEIGHBORS:\t%d\n",
                DRAM_BUFFER,
                DRAM_NODE,
                REM_NODE,
                PAGE_SIZE,
                BASE_PAGE_SIZE,
                PAGE_MASK,
                BASE_PAGE_MASK,
                MAX_NEIGHBORS);

    LOG_DEBUG("\talgorithm.h\n"
                "\t\tVA_WEIGHT:\t%d\n"
                "\t\tCYC_WEIGHT:\t%d\n"
                "\t\tIP_WEIGHT:\t%d\n"
                "\t\tDEC_UP:\t%f\n"
                "\t\tDEC_DOWN:\t%f\n"
                "\t\tDEC_DIST:\t%f\n"
                "\t\tDEC_MIG_TIME:\t%f\n"
                "\t\tHISTORY_SIZE:\t%d\n"
                "\t\tMAX_PRED_DEPTH:\t%d\n",
                VA_WEIGHT,
                CYC_WEIGHT,
                IP_WEIGHT,
                DEC_UP,
                DEC_DOWN,
                DEC_DIST,
                DEC_MIG_TIME,
                HISTORY_SIZE,
                MAX_PRED_DEPTH);

    LOG_DEBUG("pebs_init\n");
    pebs_init();

    LOG_DEBUG("pact_init\n");
    pact_init();
}

// If the allocations are smaller than the PAGE_SIZE it's possible to 
void add_page(struct pact_page *page) {
    struct pact_page *p;
    pthread_mutex_lock(&pages_lock);

    // struct pact_page *cur_page, *tmp;
    // LOG_DEBUG("pages: ");
    // HASH_ITER(hh, pages, cur_page, tmp) {
    //     LOG_DEBUG("0x%lx, ", cur_page->va);
    // }
    // LOG_DEBUG("\n");

    HASH_FIND(hh, pages, &(page->va), sizeof(uint64_t), p);
    if (p != NULL) {
        LOG_DEBUG("add_page: duplicate page: 0x%lx\n", page->va);
        // free(page);
        pthread_mutex_unlock(&pages_lock);
        return;
    }
    assert(p == NULL);
    HASH_ADD(hh, pages, va, sizeof(uint64_t), page);
    pthread_mutex_unlock(&pages_lock);
}

void remove_page(struct pact_page *page)
{
  pthread_mutex_lock(&pages_lock);
  HASH_DEL(pages, page);
  pthread_mutex_unlock(&pages_lock);
}

struct pact_page* find_page_no_lock(uint64_t va) {
    struct pact_page *page;
    if (pthread_mutex_trylock(&pages_lock) != 0) {
        return NULL;    // Abort early so no waiting
    }
    HASH_FIND(hh, pages, &va, sizeof(uint64_t), page);
    pthread_mutex_unlock(&pages_lock);
    return page;
}

struct pact_page* find_page(uint64_t va)
{
  struct pact_page *page;
  pthread_mutex_lock(&pages_lock);
  HASH_FIND(hh, pages, &va, sizeof(uint64_t), page);
  pthread_mutex_unlock(&pages_lock);
  return page;
}

void pact_init() {
#if (DRAM_BUFFER != 0 && DRAM_SIZE != 0) || (DRAM_BUFFER == 0 && DRAM_SIZE == 0)
    fprintf(stderr, "Can't have both DRAM_BUFFER and DRAM_SIZE\n");
    exit(1);
#endif

    // Puts non-tracked mmaps into remote memory so it doesn't exceed
    // the set DRAM capacity
    numa_set_preferred(DRAM_NODE);

    // LOG_DEBUG("DRAM size: %lu, REMOTE size: %lu\n", DRAM_SIZE, REMOTE_SIZE);

    LOG_DEBUG("finished pact_init\n");

    struct pact_page *dummy_page = calloc(1, sizeof(struct pact_page));
    add_page(dummy_page);

    // check how much free space on dram
#ifdef DRAM_BUFFER
    dram_size = numa_node_size(DRAM_NODE, &dram_free);
    dram_size -= dram_free;
#endif
#ifdef DRAM_SIZE
    dram_size = DRAM_SIZE;
#endif
}

#define PAGE_ROUND_UP(x) (((x) + (PAGE_SIZE)-1) & (~((PAGE_SIZE)-1)))
#define PAGE_ROUND_DOWN(x) ((x) & (~((PAGE_SIZE)-1)))

#define PAGE_ROUND_UP_BASE(x) (((x) + (BASE_PAGE_SIZE)-1) & (~((BASE_PAGE_SIZE)-1)))


struct pact_page* create_pact_page(uint64_t addr, bool in_fastmem) {
    uint64_t pact_align_addr = addr & PAGE_MASK;
    uint64_t pact_offset_addr = addr & (~PAGE_MASK);
    struct pact_page *page;

    page = find_page(pact_align_addr);
    if (page == NULL) {
        // LOG_DEBUG("Creating new page: %lx\n", addr);
        // page doesn't exist, create new page entry
        page = calloc(1, sizeof(struct pact_page));
        page->va = pact_align_addr;
        if (page == NULL) {
            LOG_DEBUG("Failed to make new page: %lx\n", addr);
        } else {
            add_page(page);
        }
    }
    page->in_dram = in_fastmem ? IN_DRAM : IN_REM;
    if (page->in_dram == IN_DRAM) {
        if (page->list != NULL) {
            page_list_remove_page(page->list, page);
        }
        enqueue_fifo(&cold_list, page);
    }
    // add 
    // bitmask for which base pages exist within page
    // 2MB/4KB = 512 bits / 8 = 64 bytes = 0.003% of mem used
    // bitmask used to calculate size of current page
    uint64_t bit_idx = pact_offset_addr / BASE_PAGE_SIZE;

    // Check if bit is already flipped
    if ((page->base_bitmask[bit_idx / 64] >> (bit_idx % 64)) & 1) {
        // LOG_DEBUG("Page already exists: %lx\n", addr);
        return page;
    }
    page->base_bitmask[bit_idx / 64] |= (1 << (64 - (bit_idx % 64)));
    page->size += BASE_PAGE_SIZE;

    return page;
}

void* pact_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
    length = PAGE_ROUND_UP_BASE(length);

    unsigned long dram_nodemask = 1UL << DRAM_NODE;
    unsigned long rem_nodemask = 1UL << REM_NODE;
    void *p_dram = NULL, *p_rem = NULL;

    void *p = mmap(addr, length, prot, flags, fd, offset);
    assert(p != MAP_FAILED);

    pthread_mutex_lock(&mmap_lock);

    if (__atomic_load_n(&dram_used, __ATOMIC_ACQUIRE) + length <= dram_size 
        && atomic_load_explicit(&dram_lock, memory_order_acquire) == false) {
        // can allocate all on dram
        __atomic_fetch_add(&dram_used, length, __ATOMIC_RELEASE);
        // dram_used += length;
        pthread_mutex_unlock(&mmap_lock);
        LOG_DEBUG("MMAP: All DRAM\n");


        if (mbind(p, length, MPOL_BIND, &dram_nodemask, 64, MPOL_MF_MOVE | MPOL_MF_STRICT)) {
            perror("mbind");
            assert(0);
        }
        
        p_dram = p;
        p_rem = p_dram + length + 1;    // Used later to check which node page is in
    } else if (dram_used + PAGE_SIZE > dram_size || atomic_load_explicit(&dram_lock, memory_order_acquire)) {
        pthread_mutex_unlock(&mmap_lock);
        LOG_DEBUG("MMAP: All Remote\n");
        // dram full, all on remote
        if (mbind(p, length, MPOL_BIND, &rem_nodemask, 64, MPOL_MF_MOVE | MPOL_MF_STRICT)) {
            perror("mbind");
            assert(0);
        }
        p_rem = p;
    } else {
        // split between dram and remote
        uint64_t dram_mmap_size = PAGE_ROUND_DOWN(dram_size - dram_used);
        // dram_used += dram_mmap_size;
        __atomic_fetch_add(&dram_used, dram_mmap_size, __ATOMIC_RELEASE);
        pthread_mutex_unlock(&mmap_lock);
        
        uint64_t rem_mmap_size = length - dram_mmap_size;


        LOG_DEBUG("MMAP: dram: %lu, remote: %lu\n", dram_mmap_size, rem_mmap_size);
        p_dram = p;
        p_rem = p_dram + dram_mmap_size;
        if (mbind(p_dram, dram_mmap_size, MPOL_BIND, &dram_nodemask, 64, MPOL_MF_MOVE | MPOL_MF_STRICT) == -1) {
            perror("mbind");
            assert(0);
        }
        if (mbind(p_rem, rem_mmap_size, MPOL_BIND, &rem_nodemask, 64, MPOL_MF_MOVE | MPOL_MF_STRICT) == -1) {
            perror("mbind");
            assert(0);
        }
        
    }
    


    // LOG_DEBUG("dram_size: %ld, dram_free: %ld\n", dram_size, dram_free);
    if (p == MAP_FAILED) {
        LOG_DEBUG("mmap failed\n");
        return MAP_FAILED;
    }
    pebs_stats.mem_allocated += length;

    assert((uint64_t)p % BASE_PAGE_SIZE == 0);

    // recycle pages from free_pact_pages
    uint64_t num_pact_pages_needed = (length + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t i = 0;
    for (i = 0; free_list.numentries > 0 && num_pact_pages_needed > 0; i++) {
        // printf("recycling pages\n");
        struct pact_page *page = dequeue_fifo(&free_list);
        if (page == NULL) break;
        pthread_mutex_lock(&page->page_lock);

        // use lock to cause atomic update of page
        assert(page->free);
        page->va_start = p + (i * PAGE_SIZE);
        if (length - (i * PAGE_SIZE) < PAGE_SIZE) {
            page->va = (uint64_t)(page->va_start);
            page->size = length - (i * PAGE_SIZE);
            if (page->size < BASE_PAGE_SIZE) page->size = BASE_PAGE_SIZE;   // Always at least 4KB
        } else {
            page->size = PAGE_SIZE;
            // Align va to PAGE_SIZE address for future lookups in hashmap
            page->va = PAGE_ROUND_UP((uint64_t)(page->va_start));
        }
        if (page->va > max_pact_va) max_pact_va = page->va;
        if (page->va < min_pact_va) min_pact_va = page->va;
        page->mig_up = 0;
        page->mig_down = 0;
        page->accesses = 0;
        page->migrating = false;
        page->local_clock = 0;
        page->cyc_accessed = 0;
        page->ip = 0;

        // page->prev = NULL;
        // page->next = NULL;


        page->in_dram = (page->va_start >= p_rem) ? IN_REM : IN_DRAM;
        page->hot = false;
        page->free = false;
        page->migrating = false;
        page->migrated = false;
        memset(page->neighbors, 0, MAX_NEIGHBORS * sizeof(struct neighbor_page));

        assert(page->list == NULL);
        if (page->in_dram == IN_DRAM) {
            enqueue_fifo(&cold_list, page);
        }

        pthread_mutex_unlock(&page->page_lock);

        // pthread_mutex_init(&page->page_lock, NULL);

        // LOG_DEBUG("adding recycled page: 0x%lx\n", (uint64_t)page);
        add_page(page);
        num_pact_pages_needed--;
    }

    if (num_pact_pages_needed == 0) {
        return p;
    }

    uint64_t pages_mmap_size = num_pact_pages_needed * sizeof(struct pact_page);
    void *pages_ptr = mmap(NULL, pages_mmap_size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    assert(pages_ptr != MAP_FAILED);
    pebs_stats.internal_mem_overhead += pages_mmap_size;
    
    for (uint64_t j = 0; num_pact_pages_needed > 0; j++) {
        // struct pact_page* page = create_pact_page(page_boundry, pages_ptr);
        struct pact_page *page = (struct pact_page *)(pages_ptr + (j * sizeof(struct pact_page)));

        // Don't need lock since first creation of page so no threads have cached data on it
        page->va_start = p + (i * PAGE_SIZE);
        if (length - (i * PAGE_SIZE) < PAGE_SIZE) {
            page->va = (uint64_t)(page->va_start);
            page->size = length - (i * PAGE_SIZE);
            if (page->size < BASE_PAGE_SIZE) page->size = BASE_PAGE_SIZE;   // Always at least 4KB
        } else {
            page->size = PAGE_SIZE;
            // Align va to PAGE_SIZE address for future lookups in hashmap
            page->va = PAGE_ROUND_UP((uint64_t)(page->va_start));
        }
        if (page->va > max_pact_va) max_pact_va = page->va;
        if (page->va < min_pact_va) min_pact_va = page->va;
        page->mig_up = 0;
        page->mig_down = 0;
        page->accesses = 0;
        page->local_clock = 0;
        page->cyc_accessed = 0;
        page->ip = 0;

        page->prev = NULL;
        page->next = NULL;

        page->in_dram = (page->va_start >= p_rem) ? IN_REM : IN_DRAM;
        page->hot = false;
        page->free = false;
        page->migrating = false;
        page->migrated = false;
        memset(page->neighbors, 0, MAX_NEIGHBORS * sizeof(struct neighbor_page));
        pthread_mutex_init(&page->page_lock, NULL);
        page->list = NULL;
        if (page->in_dram == IN_DRAM) {
            enqueue_fifo(&cold_list, page);
        }

        
        // LOG_DEBUG("adding page: 0x%lx\n", (uint64_t)page);
        add_page(page);
        num_pact_pages_needed--;
        i++;
    }
    return p;
}

int pact_munmap(void *addr, size_t length) {
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
            pthread_mutex_lock(&page->page_lock);
            assert(page->free == false);
            page->free = true;
            remove_page(page);
            // if (page->in_dram == IN_DRAM) {
            //     dram_used -= page->size;
            // }
            pebs_stats.mem_allocated -= page->size;

            if (page->list != NULL) {
                page_list_remove_page(page->list, page);
            }
            enqueue_fifo(&free_list, page);

            pthread_mutex_unlock(&page->page_lock);
        }
    }
    return 0;
}