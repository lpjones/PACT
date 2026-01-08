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

_Atomic bool fast_lock = false;

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
    internal_call = true;
#if (FAST_BUFFER != 0 && FAST_SIZE != 0) || (FAST_BUFFER == 0 && FAST_SIZE == 0)
    fprintf(stderr, "Can't have both FAST_BUFFER and FAST_SIZE\n");
    exit(1);
#endif

    // Puts non-tracked mmaps into slow memory so it doesn't exceed
    // the set FAST capacity
    numa_set_preferred(FAST_NODE);

    // LOG_DEBUG("FAST size: %lu, REMOTE size: %lu\n", FAST_SIZE, REMOTE_SIZE);

    LOG_DEBUG("finished pact_init\n");

    struct pact_page *dummy_page = calloc(1, sizeof(struct pact_page));
    add_page(dummy_page);

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
    unsigned long slow_nodemask = 1UL << REM_NODE;
    void *p_fast = NULL, *p_slow = NULL;

    void *p = libc_mmap(addr, length, prot, flags | MAP_POPULATE, fd, offset);
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


        if (mbind(p, length, MPOL_BIND, &fast_nodemask, 64, MPOL_MF_MOVE | MPOL_MF_STRICT)) {
            perror("mbind");
            assert(0);
        }
        
        p_fast = p;
        p_slow = p_fast + length + 1;    // Used later to check which node page is in
    } else if (fast_used + PAGE_SIZE > fast_size || atomic_load_explicit(&fast_lock, memory_order_acquire)) {
        pthread_mutex_unlock(&mmap_lock);
        LOG_DEBUG("MMAP: All Remote\n");
        // fast full, all on slow
        if (mbind(p, length, MPOL_BIND, &slow_nodemask, 64, MPOL_MF_MOVE | MPOL_MF_STRICT)) {
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
        if (mbind(p_fast, fast_mmap_size, MPOL_BIND, &fast_nodemask, 64, MPOL_MF_MOVE | MPOL_MF_STRICT) == -1) {
            perror("mbind");
            assert(0);
        }
        if (mbind(p_slow, slow_mmap_size, MPOL_BIND, &slow_nodemask, 64, MPOL_MF_MOVE | MPOL_MF_STRICT) == -1) {
            perror("mbind");
            assert(0);
        }
        
    }
    


    // LOG_DEBUG("fast_size: %ld, fast_free: %ld\n", fast_size, fast_free);
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


        page->in_fast = (page->va_start >= p_slow) ? IN_REM : IN_FAST;
        page->hot = false;
        page->free = false;
        page->migrating = false;
        page->migrated = false;
        memset(page->neighbors, 0, MAX_NEIGHBORS * sizeof(struct neighbor_page));

        assert(page->list == NULL);
        if (page->in_fast == IN_FAST) {
            enqueue_fifo(&cold_list, page);
        }

        pthread_mutex_unlock(&page->page_lock);

        // pthread_mutex_init(&page->page_lock, NULL);

        // LOG_DEBUG("adding recycled page: 0x%lx\n", (uint64_t)page);
        add_page(page);
        num_pact_pages_needed--;
    }

    if (num_pact_pages_needed == 0) {
        internal_call = false; 
        return p;
    }

    uint64_t pages_mmap_size = num_pact_pages_needed * sizeof(struct pact_page);
    void *pages_ptr = libc_mmap(NULL, pages_mmap_size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
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

        page->in_fast = (page->va_start >= p_slow) ? IN_REM : IN_FAST;
        page->hot = false;
        page->free = false;
        page->migrating = false;
        page->migrated = false;
        memset(page->neighbors, 0, MAX_NEIGHBORS * sizeof(struct neighbor_page));
        pthread_mutex_init(&page->page_lock, NULL);
        page->list = NULL;
        if (page->in_fast == IN_FAST) {
            enqueue_fifo(&cold_list, page);
        }

        
        // LOG_DEBUG("adding page: 0x%lx\n", (uint64_t)page);
        add_page(page);
        num_pact_pages_needed--;
        i++;
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
            pthread_mutex_lock(&page->page_lock);
            assert(page->free == false);
            page->free = true;
            remove_page(page);
            // if (page->in_fast == IN_FAST) {
            //     fast_used -= page->size;
            // }
            pebs_stats.mem_allocated -= page->size;

            if (page->list != NULL) {
                page_list_remove_page(page->list, page);
            }
            enqueue_fifo(&free_list, page);

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