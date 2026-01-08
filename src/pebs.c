#include "pebs.h"

// #define CHECK_KILLED(thread) if (!(num_loops++ & 0xFFFF) && killed(thread)) return NULL;
#define CHECK_KILLED(thread) 


// Public variables


// Private variables
static int pfd[PEBS_NPROCS][NPBUFTYPES];
static struct perf_event_mmap_page *perf_page[PEBS_NPROCS][NPBUFTYPES];
static uint64_t no_samples[PEBS_NPROCS][NPBUFTYPES];
static FILE* pact_trace_fp = NULL;
static _Atomic bool kill_internal_threads[NUM_INTERNAL_THREADS];
static pthread_t internal_threads[NUM_INTERNAL_THREADS];

static _Thread_local uint64_t last_cyc_cool;

static uint64_t global_clock = 0;


struct perf_sample {
  __u64	ip;             /* if PERF_SAMPLE_IP*/
//   __u32 pid, tid;       /* if PERF_SAMPLE_TID */
  __u64 time;           /* if PERF_SAMPLE_TIME */
  __u64 addr;           /* if PERF_SAMPLE_ADDR */
//   __u64 weight;         /* if PERF_SAMPLE_WEIGHT */
// __u64 data_src;         /* if PERF_SAMPLE_DATA_SRC */
};



struct pebs_stats pebs_stats = {0};


void wait_for_threads() {
    for (int i = 0; i < NUM_INTERNAL_THREADS; i++) {
        void *ret;
        pthread_join(internal_threads[i], &ret);
    }
    LOG_DEBUG("Internal threads killed\n");
}

void pebs_cleanup() {
    
}

static inline void kill_thread(uint8_t thread) {
    atomic_store(&kill_internal_threads[thread], true);
}

void kill_threads() {
    LOG_DEBUG("Killing threads\n");
    for (int i = 0; i < NUM_INTERNAL_THREADS; i++) {
        kill_thread(i);
    }
}

static inline bool killed(uint8_t thread) {
    return atomic_load(&kill_internal_threads[thread]);
}

static inline long perf_event_open(struct perf_event_attr *hw_event, pid_t pid, int cpu, int group_fd, unsigned long flags) {
    int ret;
    ret = syscall(__NR_perf_event_open, hw_event, pid, cpu, group_fd, flags);
  return ret;
}

static struct perf_event_mmap_page* perf_setup(__u64 config, __u64 config1, uint32_t cpu_idx, __u64 cpu, __u64 type) {
    struct perf_event_attr attr = {0};

    attr.type = PERF_TYPE_RAW;
    attr.size = sizeof(struct perf_event_attr);

    attr.config = config;
    attr.config1 = config1;
    attr.sample_period = SAMPLE_PERIOD;

    attr.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TIME | PERF_SAMPLE_ADDR; // PERF_SAMPLE_TID, PERF_SAMPLE_WEIGHT
    attr.disabled = 0;
    //attr.inherit = 1;
    attr.exclude_kernel = 1;
    attr.exclude_hv = 1;
    attr.exclude_callchain_kernel = 1;
    attr.exclude_callchain_user = 1;
    attr.precise_ip = 1;
    
    pfd[cpu_idx][type] = perf_event_open(&attr, -1, cpu, -1, 0);
    assert(pfd[cpu_idx][type] != -1);


    size_t mmap_size = sysconf(_SC_PAGESIZE) * PERF_PAGES;
    /* printf("mmap_size = %zu\n", mmap_size); */
    struct perf_event_mmap_page *p = libc_mmap(NULL, mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED, pfd[cpu_idx][type], 0);
    LOG_DEBUG("PEBS: cpu: %u, type: %llu, buffer size: %lu\n", cpu_idx, type, mmap_size);
    pebs_stats.internal_mem_overhead += mmap_size;

    assert(p != MAP_FAILED);
    fprintf(stderr, "Set up perf on core %llu\n", cpu);


    return p;
}

void* pebs_stats_thread() {
    internal_call = true;

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(PEBS_STATS_CPU, &cpuset);
    int s = pthread_setaffinity_np(internal_threads[PEBS_STATS_THREAD], sizeof(cpu_set_t), &cpuset);
    assert(s == 0);


    while (!killed(PEBS_STATS_THREAD)) {
        sleep(1);
        LOG_STATS("internal_mem_overhead: [%lu]\tpact_allocated: [%lu]\tthrottles: [%lu]\tunthrottles: [%lu]\tunknown_samples: [%lu]\n", 
                pebs_stats.internal_mem_overhead, pebs_stats.mem_allocated, pebs_stats.throttles, pebs_stats.unthrottles, pebs_stats.unknown_samples)
        LOG_STATS("\twrapped_records: [%lu]\twrapped_headers: [%lu]\n", 
                pebs_stats.wrapped_records, pebs_stats.wrapped_headers);

#if FAST_BUFFER != 0
        LOG_STATS("\tfast_free: [%ld]\tfast_used: [%ld]\t fast_size: [%ld]\tslow_used: [%ld]\n", fast_free, fast_used, fast_size, slow_used);
#endif
#if FAST_SIZE != 0
        LOG_STATS("\tfast_used: [%ld]\t fast_size: [%ld]\tnon_tracked_mem: [%lu]\n", fast_used, fast_size, pebs_stats.non_tracked_mem);
#endif
        double percent_fast = 100.0 * pebs_stats.fast_accesses / (pebs_stats.fast_accesses + pebs_stats.slow_accesses);
        LOG_STATS("\tfast_accesses: [%ld]\tslow_accesses: [%ld]\t percent_fast: [%.2f]\n", 
            pebs_stats.fast_accesses, pebs_stats.slow_accesses, percent_fast);
        
        uint64_t migrations = pebs_stats.promotions + pebs_stats.demotions;
        LOG_STATS("\tpromotions: [%lu]\tdemotions: [%lu]\tmigrations: [%lu]\tpebs_resets: [%lu]\tmig_move_time: [%.2f]\tmig_queue_time: [%.2f]\n", 
                pebs_stats.promotions, pebs_stats.demotions, migrations, pebs_stats.pebs_resets, mig_move_time, mig_queue_time);

        LOG_STATS("\tthreshold: [%.2f]\tavg_dist: [%.2f]\tdiff: [%.2f]\n", bot_dist, avg_dist, avg_dist - bot_dist);

        LOG_STATS("\tcold_pages: [%lu]\thot_pages: [%lu]\n", cold_list.numentries, hot_list.numentries);



        pebs_stats.fast_accesses = 0;
        pebs_stats.slow_accesses = 0;
        pebs_stats.promotions = 0;
        pebs_stats.demotions = 0;
        pebs_stats.throttles = 0;
        pebs_stats.unthrottles = 0;
        pebs_stats.pebs_resets = 0;
        


    }
    return NULL;
}

static void start_pebs_stats_thread() {
    int s = pthread_create(&internal_threads[PEBS_STATS_THREAD], NULL, pebs_stats_thread, NULL);
    assert(s == 0);
}

// Could be munmapped at any time
void make_hot_request(struct pact_page* page) {
    if (page == NULL) return;
    // page could be munmapped here (but pages are never actually
    // unmapped so just check if it's in free state once locked)
    if (pthread_mutex_trylock(&page->page_lock) != 0) { // Abort if lock taken to speed up pebs thread
        return;
    }
    // pthread_mutex_lock(&page->page_lock);
    // check if unmapped
    if (page->free) {
        // printf("Page was free\n");
        pthread_mutex_unlock(&page->page_lock);
        return;
    }
    page->hot = true;
    
    // add to hot list if:
    // page is not already in hot list and in slow mem
    if (page->list != &hot_list && page->in_fast == IN_REM) {
        // page should not be hot
        // not be cold since all cold pages are in fast
        // not be free 
        // either was in slow mem or just got dequeued
        // from cold list in migrate thread
        // page->list == &cold_list and in Remote
#if LRU_ALGO == 0
        if (page->list != NULL) {
            assert(page->list == &cold_list);
            page_list_remove_page(&cold_list, page);
        }
#endif
        assert(page->list == NULL);
        enqueue_fifo(&hot_list, page);
        page->mig_start = rdtscp();

    }
#if LRU_ALGO == 1
    // If already in fast update LRU cold list
    else if (page->in_fast == IN_FAST) {
        assert(page->list == &cold_list);
        page_list_remove_page(&cold_list, page);
        enqueue_fifo(&cold_list, page);
    }
#endif
    // printf("page is either already in hot list or is in slow memory\n");
    
    pthread_mutex_unlock(&page->page_lock);

}

void make_cold_request(struct pact_page* page) {
    if (page == NULL) return;
    // page could be munmapped here (but pages are never actually
    // unmapped so just check if it's in free state once locked)
    if (pthread_mutex_trylock(&page->page_lock) != 0) { // Abort if lock taken to speed up pebs thread
        LOG_DEBUG("Failed lock: 0x%lx\n", page->va);
        return;
    }
    // check if unmapped
    if (page->free) {
        pthread_mutex_unlock(&page->page_lock);
        return;
    }
    page->hot = false;
#if LRU_ALGO == 0
    // move to cold list if:
    // page is not already in cold list and
    // page is in fast
    if (page->list != &cold_list && page->in_fast == IN_FAST) {
        // remove from hot list
        if (page->list != NULL) {
            assert(page->list == &hot_list);
            page_list_remove_page(&hot_list, page);
        }
        assert(page->list == NULL);
        enqueue_fifo(&cold_list, page);
    }
#else
    // Even if page is already in cold list
    // move to back of cold list for LRU
    if (page->in_fast == IN_FAST) {
        // assert(page->list != NULL);
        assert(page->list != &free_list);
        if (page->list != NULL) {   // page could be dequeued from migrate thread
            page_list_remove_page(page->list, page);
        }

        assert(page->list == NULL);
        enqueue_fifo(&cold_list, page);
    }
#endif
    pthread_mutex_unlock(&page->page_lock);
}
static uint64_t samples_since_cool = 0;

void process_perf_buffer(int cpu_idx, int evt) {
    struct perf_event_mmap_page *p = perf_page[cpu_idx][evt];
    uint64_t num_loops = 0;

    while (p->data_head != p->data_tail && num_loops++ != 128) {
        
        struct perf_sample rec = {.addr = 0};
        char *data = (char*)p + p->data_offset;
        uint64_t avail = p->data_head - p->data_tail;

        // LOG_DEBUG("Backlog: %lu\n", avail / (sizeof(struct perf_sample) + sizeof(struct perf_event_header)));

        assert(((p->data_size - 1) & p->data_size) == 0);
        assert(p->data_size != 0);

        // header
        uint64_t wrapped_tail = p->data_tail & (p->data_size - 1);
        struct perf_event_header *hdr = (struct perf_event_header *)(data + wrapped_tail);

        assert(hdr->size != 0);
        assert(avail >= hdr->size);

        if (wrapped_tail + hdr->size <= p->data_size) {
            switch (hdr->type) {
                case PERF_RECORD_SAMPLE:
                    if (hdr->size - sizeof(struct perf_event_header) == sizeof(struct perf_sample)) {
                        memcpy(&rec, data + wrapped_tail + sizeof(struct perf_event_header), sizeof(struct perf_sample));
                        // rec = (struct perf_sample *)(data + wrapped_tail + sizeof(struct perf_event_header));
                        // printf("addr: 0x%llx, ip: 0x%llx, time: %llu\n", rec->addr, rec->ip, rec->time);
                    }
                    break;
                case PERF_RECORD_THROTTLE:
                    pebs_stats.throttles++;
                    break;
                case PERF_RECORD_UNTHROTTLE:
                    pebs_stats.unthrottles++;
                    break;
                default:
                    pebs_stats.unknown_samples++;
                    break;
            }
        } else {
            pebs_stats.wrapped_records++;
        }
        p->data_tail += hdr->size;
 
        /* Have PEBS Sample, Now check with pact */
        // continue;
        if (rec.addr == 0) continue;

        uint64_t addr_aligned = rec.addr & PAGE_MASK;
        struct pact_page *page = find_page_no_lock(addr_aligned);

        // Try 4KB aligned page if not 2MB aligned page
        if (page == NULL)
            page = find_page_no_lock(rec.addr & BASE_PAGE_MASK);
        if (page == NULL) continue;
#if RECORD == 1
        struct pebs_rec p_rec = {
            .va = addr_aligned,
            .ip = rec.ip,
            .cyc = rdtscp(),
            .cpu = cpu_idx,
            .evt = evt
        };
        fwrite(&p_rec, sizeof(struct pebs_rec), 1, pact_trace_fp);
#endif

        // if (page->migrated) {
        //     LOG_DEBUG("PEBS: accessed migrated page: 0x%lx\n", page->va);
        // }

        // cool off
        page->accesses >>= (global_clock - page->local_clock);
        page->local_clock = global_clock;

        if (evt == FASTREAD) pebs_stats.fast_accesses++;
        else pebs_stats.slow_accesses++;
        page->accesses++;

        uint64_t cur_cyc = rdtscp();
        if (rec.time > page->cyc_accessed) {
            page->cyc_accessed = rec.time;
            page->ip = rec.ip;
        }

        // LRU cold list
        // if sample is cold move to end of cold queue
        // Everything in FAST is cold

#if HEM_ALGO == 1
        if (page->accesses >= HOT_THRESHOLD) {
            // LOG_DEBUG("PEBS: Made hot: 0x%lx\n", page->va);
#if RECORD == 1
            struct pebs_rec p_rec = {
                .va = page->va,
                .ip = 0,
                .cyc = rdtscp(),
                .cpu = 0,
                .evt = 0
            };
            fwrite(&p_rec, sizeof(struct pebs_rec), 1, pred_fp);
#endif
            make_hot_request(page);
        } else {
            make_cold_request(page);
        }

        // Sample based cooling
        samples_since_cool++;
        if (samples_since_cool >= SAMPLE_COOLING_THRESHOLD) {
            global_clock++;
            samples_since_cool = 0;
            // printf("cyc since last cool: %lu\n", cur_cyc - last_cyc_cool);
            last_cyc_cool = rdtscp();
        }

        // Time based cooling
        // if (cur_cyc - last_cyc_cool > CYC_COOL_THRESHOLD) {
        //     // __atomic_fetch_add(&global_clock, 1, __ATOMIC_RELEASE);
        //     global_clock++;
        //     last_cyc_cool = cur_cyc;
        // }

#endif 

        

        
#if CLUSTER_ALGO == 1
        algo_add_page(page);
        
        if (cold_list.numentries != 0) {
            struct pact_page *pred_pages[MAX_NEIGHBORS * MAX_PRED_DEPTH];
            uint32_t idx = 0;
            algo_predict_pages(page, pred_pages, &idx);

            for (uint32_t i = 0; i < idx; i++) {
                // LOG_DEBUG("PRED: 0x%lx from 0x%lx\n", pred_pages[i]->va, page->va);
#if RECORD == 1
                struct pebs_rec p_rec = {
                    .va = pred_pages[i]->va,
                    .ip = 0,
                    .cyc = rdtscp(),
                    .cpu = 0,
                    .evt = 0
                };
                fwrite(&p_rec, sizeof(struct pebs_rec), 1, pred_fp);
#endif
                make_hot_request(pred_pages[i]);
            }
            
        }
        
#if LRU_ALGO == 1
        // LRU based cold list
        // everything in FAST is in cold list
        // with oldest page at front of queue
        make_cold_request(page);
#endif
#endif

        no_samples[cpu_idx][evt] = cur_cyc;

    }
    no_samples[cpu_idx][evt]++;
    p->data_tail = p->data_head;

    uint64_t cur_cyc = rdtscp();
    if (cur_cyc > no_samples[cpu_idx][evt] + NO_SAMPLE_RESET_TIME) {
        pebs_stats.pebs_resets++;
        ioctl(pfd[cpu_idx][evt], PERF_EVENT_IOC_DISABLE);
        ioctl(pfd[cpu_idx][evt], PERF_EVENT_IOC_RESET);
        ioctl(pfd[cpu_idx][evt], PERF_EVENT_IOC_ENABLE);
        no_samples[cpu_idx][evt] = cur_cyc;
    }
    // Run clustering algorithm
}


void* pebs_scan_thread() {
    internal_call = true;
    // set cpu
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(PEBS_SCAN_CPU, &cpuset);
    // pthread_t thread_id = pthread_self();
    int s = pthread_setaffinity_np(internal_threads[PEBS_THREAD], sizeof(cpu_set_t), &cpuset);
    assert(s == 0);
    // pebs_init();

    last_cyc_cool = rdtscp();

    // uint64_t num_loops = 0;

    
    while (true) {
        CHECK_KILLED(PEBS_THREAD);

        int pebs_start_cpu = 0;
        int num_cores = PEBS_NPROCS;

        for (int cpu_idx = pebs_start_cpu; cpu_idx < pebs_start_cpu + num_cores; cpu_idx++) {
            for(int evt = 0; evt < NPBUFTYPES; evt++) {
                process_perf_buffer(cpu_idx, evt);
            }
        }
    }
    pebs_cleanup();
    return NULL;
}

void pact_migrate_page(struct pact_page *page, int node) {
    unsigned long nodemask = 1UL << node;

    if (mbind(page->va_start, page->size, MPOL_BIND, &nodemask, 64, MPOL_MF_MOVE | MPOL_MF_STRICT) == -1) {
        perror("mbind");
        LOG_DEBUG("mbind failed %p\n", page->va_start);
    } else {
        page->migrated = true;
        if (node == FAST_NODE) {
            // was migrated to fast
            pebs_stats.promotions++;
            page->in_fast = IN_FAST;
#if LRU_ALGO == 1
            page->hot = false;
            enqueue_fifo(&cold_list, page);
#else
            page->hot = true;
            enqueue_fifo(&hot_list, page);
#endif
#if RECORD == 1
            struct pebs_rec p_rec = {
                .va = page->va,
                .ip = 0,
                .cyc = rdtscp(),
                .cpu = 0,
                .evt = 0
            };
            fwrite(&p_rec, sizeof(struct pebs_rec), 1, mig_fp);
#endif
        } else {
#if RECORD == 1
            struct pebs_rec p_rec = {
                .va = page->va,
                .ip = 0,
                .cyc = rdtscp(),
                .cpu = 0,
                .evt = 0
            };
            fwrite(&p_rec, sizeof(struct pebs_rec), 1, cold_fp);
#endif
            pebs_stats.demotions++;
            page->in_fast = IN_REM;
            page->hot = false;
        }
    }
}
void *demote_thread() {
    internal_call = true;
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(PROMOTE_CPU, &cpuset);
    int s = pthread_setaffinity_np(internal_threads[DEMOTE_THREAD], sizeof(cpu_set_t), &cpuset);
    assert(s == 0);


    while (true) {
#if FAST_BUFFER != 0
        // hacky way to update fast_used every second in case there's drift over time
        fast_size = numa_node_size(FAST_NODE, &fast_free);
        fast_used = fast_size - fast_free;
        fast_size -= FAST_BUFFER;

        long slow_free;
        long slow_size = numa_node_size(REM_NODE, &slow_free);
        slow_used = slow_size - slow_free;
#endif
        int bytes_demoted = 0;
        while (fast_free + bytes_demoted < FAST_BUFFER) {
            struct pact_page *cold_page = dequeue_fifo(&cold_list);
            if (cold_page == NULL) {
                LOG_DEBUG("MIG: no cold pages, aborting\n");
                break;
            }
            assert(cold_page != NULL);
            pthread_mutex_lock(&cold_page->page_lock);
#if LRU_ALGO == 1
            if (cold_page->list != NULL) {
#else
            if (cold_page->list != NULL || cold_page->in_fast == IN_REM || cold_page->hot) {
#endif
                // page got yoinked
                pthread_mutex_unlock(&cold_page->page_lock);
                continue;
            }
            assert(cold_page->in_fast == IN_FAST);
            // assert(!cold_page->hot);
            assert(cold_page->list == NULL);

            // pact_migrate_pages(&cold_page, 1, REM_NODE);
            pact_migrate_page(cold_page, REM_NODE);
            cold_page->migrated = true;
            bytes_demoted += cold_page->size;
            LOG_DEBUG("MIG: demoted 0x%lx\n", cold_page->va);
            pthread_mutex_unlock(&cold_page->page_lock);
        }
        sleep(0.01);
    }
    return NULL;
}

void *promote_thread() {
    internal_call = true;

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(PROMOTE_CPU, &cpuset);
    int s = pthread_setaffinity_np(internal_threads[PROMOTE_THREAD], sizeof(cpu_set_t), &cpuset);
    assert(s == 0);
    // uint64_t num_loops = 0;

    struct pact_page *hot_page, *cold_page;
    uint64_t cold_bytes = 0;

    while (true) {
        // Don't do any migrations until hot page comes in
        hot_page = dequeue_fifo(&hot_list);
        if (hot_page == NULL) continue;
        pthread_mutex_lock(&hot_page->page_lock);

        assert(hot_page != NULL);
        if (hot_page->list != NULL || hot_page->in_fast == IN_FAST) {
            pthread_mutex_unlock(&hot_page->page_lock);
            continue;
        }
        
        LOG_DEBUG("MIG: got hot page: 0x%lx\n", hot_page->va);

        uint64_t mig_queue_cyc = rdtscp();
        uint64_t mig_queue_diff = mig_queue_cyc - hot_page->mig_start;
        mig_queue_time = DEC_MIG_TIME * mig_queue_diff + (1.0 - DEC_MIG_TIME) * mig_queue_time;

        LOG_DEBUG("MIG: now enough space: 0x%lx\n", hot_page->va);
        // pact_migrate_pages(&hot_page, 1, FAST_NODE);
        pact_migrate_page(hot_page, FAST_NODE);
        // hot_page->migrated = true;
        // pebs_stats.promotions++;

        // enable fast mmap
        __atomic_fetch_add(&fast_used, hot_page->size - cold_bytes, __ATOMIC_RELEASE);
        atomic_store_explicit(&fast_lock, false, memory_order_release);
        LOG_DEBUG("MIG: Finished migration: 0x%lx\n", hot_page->va);

        uint64_t mig_move_diff = rdtscp() - mig_queue_cyc;
        mig_move_time = DEC_MIG_TIME * mig_move_diff + (1.0 - DEC_MIG_TIME) * mig_move_time;

        pthread_mutex_unlock(&hot_page->page_lock);
    }
}

void start_pebs_thread() {
    int s = pthread_create(&internal_threads[PEBS_THREAD], NULL, pebs_scan_thread, NULL);
    assert(s == 0);
}

void start_promote_thread() {
    int s = pthread_create(&internal_threads[PROMOTE_THREAD], NULL, promote_thread, NULL);
    assert(s == 0);
}

void start_demote_thread() {
    int s = pthread_create(&internal_threads[DEMOTE_THREAD], NULL, demote_thread, NULL);
    assert(s == 0);
}

void pebs_init(void) {
    internal_call = true;

    for (int i = 0; i < NUM_INTERNAL_THREADS; i++) {
        atomic_store(&kill_internal_threads[i], false);
    }

#if PEBS_STATS == 1
    LOG_DEBUG("pebs_stats: %d\n", PEBS_STATS);
    start_pebs_stats_thread();
#endif

    pact_trace_fp = fopen("pact_trace.bin", "wb");
    if (pact_trace_fp == NULL) {
        perror("pact_trace file fopen");
    }
    assert(pact_trace_fp != NULL);

    int pebs_start_cpu = 0;
    int num_cores = PEBS_NPROCS;
    
    for (int i = pebs_start_cpu; i < pebs_start_cpu + num_cores; i++) {
        perf_page[i][FASTREAD] = perf_setup(0x1d3, 0, i, i * 2, FASTREAD);      // MEM_LOAD_L3_MISS_RETIRED.LOCAL_FAST, mem_load_uops_l3_miss_retired.local_dram
        perf_page[i][REMREAD] = perf_setup(0x4d3, 0, i, i * 2, REMREAD);     //  mem_load_uops_l3_miss_retired.remote_fast
        no_samples[i][FASTREAD] = 0;
        no_samples[i][REMREAD] = 0;
    }

    start_pebs_thread();

    start_promote_thread();

    start_demote_thread();

    internal_call = false;
}