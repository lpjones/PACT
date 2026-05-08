#include "pebs.h"

#include <time.h>

void sleep_ms(int milliseconds) {
    struct timespec ts;
    ts.tv_sec = milliseconds / 1000;
    ts.tv_nsec = (milliseconds % 1000) * 1000000;
    nanosleep(&ts, NULL);
}

// Public variables


// Private variables
static int pfd[PEBS_NPROCS][NPBUFTYPES];
static struct perf_event_mmap_page *perf_page[PEBS_NPROCS][NPBUFTYPES];
static uint64_t no_samples[PEBS_NPROCS][NPBUFTYPES];
static _Atomic bool kill_internal_threads[NUM_INTERNAL_THREADS];
static pthread_t internal_threads[NUM_INTERNAL_THREADS];

static _Thread_local uint64_t last_cyc_cool;

uint64_t real_promotions = 0;
uint64_t real_prom_not_accessed = 0;
static uint64_t hem_promotions = 0;
static uint64_t pagr_promotions = 0;
static uint64_t pagr_prom_not_accessed = 0;
static uint64_t hem_prom_not_accessed = 0;
static uint8_t pact_mode = PAGR_MODE;
static double cpu_usage_ema = 0.0;  // Exponential moving average of CPU usage


// Dynamic PEBS sampling variables
unsigned int pact_sample_period_idx = 0;  // Start with first period (199)
unsigned int pact_cpu_quota = PACT_CPU_QUOTA;
_Atomic bool dynamic_pebs_enabled = true;

#if HEM_ALGO == 1
static uint64_t global_clock = 0;
#endif


struct perf_sample {
  __u64	ip;             /* if PERF_SAMPLE_IP*/
//   __u32 pid, tid;       /* if PERF_SAMPLE_TID */
  __u64 time;           /* if PERF_SAMPLE_TIME */
  __u64 addr;           /* if PERF_SAMPLE_ADDR */
//   __u64 weight;         /* if PERF_SAMPLE_WEIGHT */
// __u64 data_src;         /* if PERF_SAMPLE_DATA_SRC */
};



struct pebs_stats pebs_stats = {0};

static inline long perf_event_open(struct perf_event_attr *hw_event, pid_t pid, int cpu, int group_fd, unsigned long flags) {
    int ret;
    ret = syscall(__NR_perf_event_open, hw_event, pid, cpu, group_fd, flags);
  return ret;
}

// Dynamic PEBS sampling helper functions
unsigned int pact_get_sample_period(unsigned int idx) {
    if (idx < 0)
        return pebs_period_list[0];
    else if (idx < PCOUNT)
        return pebs_period_list[idx];
    else
        return pebs_period_list[PCOUNT - 1];
}

void pact_increase_sample_period(void) {
    if (pact_sample_period_idx < PCOUNT - 1)
        pact_sample_period_idx++;
}

void pact_decrease_sample_period(void) {
    if (pact_sample_period_idx > 0)
        pact_sample_period_idx--;
}

void pact_update_sample_period(uint64_t new_period) {
    // Update sample period for all active perf events
    for (int cpu_idx = 0; cpu_idx < PEBS_NPROCS; cpu_idx++) {
        for (int evt = 0; evt < NPBUFTYPES; evt++) {
            if (pfd[cpu_idx][evt] >= 0) {
                ioctl(pfd[cpu_idx][evt], PERF_EVENT_IOC_PERIOD, &new_period);
            }
        }
    }
    LOG_DEBUG("PACT: Updated sample period to %lu\n", new_period);
}

static struct perf_event_mmap_page* perf_setup(__u64 config, __u64 config1, uint32_t cpu_idx, __u64 cpu, __u64 type) {
    struct perf_event_attr attr = {0};

    attr.type = PERF_TYPE_RAW;
    attr.size = sizeof(struct perf_event_attr);

    attr.config = config;
    attr.config1 = config1;
    attr.sample_period = pact_get_sample_period(pact_sample_period_idx);  // Use dynamic period

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

#if PEBS_STATS == 1
void* pebs_stats_thread() {
    internal_call = true;

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(PEBS_STATS_CPU, &cpuset);
    int s = pthread_setaffinity_np(internal_threads[PEBS_STATS_THREAD], sizeof(cpu_set_t), &cpuset);
    assert(s == 0);


    while (true) {
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
#if PAGR_ALGO == 1
        LOG_STATS("\tthreshold: [%.2f]\tavg_dist: [%.2f]\tdiff: [%.2f]\n", bot_dist, avg_dist, avg_dist - bot_dist);
#endif

        LOG_STATS("\tcold_pages: [%lu]\thot_pages: [%lu]\n", cold_list.numentries, hot_list.numentries);

        LOG_STATS("\tmig_failed: [%lu]\n", pebs_stats.mig_failed);

        LOG_STATS("\them_prom_not_accessed: [%lu]\them_promotions: [%lu]\them_percent_prom_not_accessed: [%.2f]\n", hem_prom_not_accessed, hem_promotions, 100.0 * (hem_prom_not_accessed + 1) / (hem_promotions + 1));
        LOG_STATS("\tpagr_prom_not_accessed: [%lu]\tpagr_promotions: [%lu]\tpagr_percent_prom_not_accessed: [%.2f]\n", pagr_prom_not_accessed, pagr_promotions, 100.0 * (pagr_prom_not_accessed + 1) / (pagr_promotions + 1));
        LOG_STATS("\treal_promotions: [%lu]\treal_prom_not_accessed: [%lu]\treal_percent_prom_not_accessed: [%.2f]\n", real_promotions, real_prom_not_accessed, 100.0 * (real_prom_not_accessed) / (real_promotions + 1));
        // LOG_STATS("\tpact_mode: [%s]\n", pact_mode == PAGR_MODE ? "PAGR" : "HEM");
        LOG_STATS("\tpact_sample_period: [%u]\tpact_pebs_usage: [%.2f]\n", pact_get_sample_period(pact_sample_period_idx), cpu_usage_ema);


        pebs_stats.fast_accesses = 0;
        pebs_stats.slow_accesses = 0;
        pebs_stats.promotions = 0;
        pebs_stats.demotions = 0;
        pebs_stats.throttles = 0;
        pebs_stats.unthrottles = 0;
        pebs_stats.pebs_resets = 0;
        pebs_stats.mig_failed = 0;

        


    }
    return NULL;
}
#endif

#if PEBS_STATS == 1
static void start_pebs_stats_thread() {
    int s = pthread_create(&internal_threads[PEBS_STATS_THREAD], NULL, pebs_stats_thread, NULL);
    assert(s == 0);
}
#endif

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
    assert(page->list != &free_list);
    
    // add to hot list if:
    // page is not already in hot list and in slow mem
    if (page->list != &hot_list && page->in_fast == IN_SLOW) {
        // page should not be hot
        // not be cold since all cold pages are in fast
        // not be free 
        // either was in slow mem or just got dequeued
        // from cold list in migrate thread
        // page->list == &cold_list and in Remote
        if (page->list != NULL) {
            assert(page->list == &cold_list);
            page_list_remove_page(&cold_list, page);
        }
        page->mig_start = rdtscp();

#if RECORD == 1 
        // record prediction
        struct pebs_rec p_rec = {
            .va = page->va,
            .ip = 0,
            .cyc = rdtscp(),
            .cpu = 0,
            .evt = 0
        };
        fwrite(&p_rec, sizeof(struct pebs_rec), 1, pred_fp);
#endif

        assert(page->list == NULL);
        enqueue_fifo(&hot_list, page);

    }
#if LRU_ALGO == 1
    // If already in fast update LRU cold list
    else if (page->in_fast == IN_FAST) {
        if (page->list != NULL) {
            page_list_remove_page(page->list, page);
        }
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

#if HEM_ALGO == 1
static uint64_t samples_since_cool = 0;
#endif

void process_perf_buffer(int cpu_idx, int evt) {

}


void* pebs_scan_thread() {
    struct perf_event_mmap_page *p;
    uint64_t sleep_timeout = 2; // 2ms
    struct perf_sample rec;

    struct perf_event_header *hdr;
    internal_call = true;
    // set cpu
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(PEBS_SCAN_CPU, &cpuset);
    // pthread_t thread_id = pthread_self();
    int s = pthread_setaffinity_np(internal_threads[PEBS_THREAD], sizeof(cpu_set_t), &cpuset);
    assert(s == 0);

    // Dynamic PEBS sampling variables
    struct timespec last_wall_ts;
    struct timespec last_cpu_ts;
    clock_gettime(CLOCK_MONOTONIC, &last_wall_ts);
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &last_cpu_ts);
    uint64_t cpu_check_period = (uint64_t)PACT_CPU_CHECK_PERIOD_MS * 1000000ULL;

    while (true) {
        // Check CPU usage every PACT_CPU_CHECK_PERIOD_MS milliseconds
        struct timespec current_wall_ts;
        clock_gettime(CLOCK_MONOTONIC, &current_wall_ts);
        uint64_t current_walltime = (uint64_t)current_wall_ts.tv_sec * 1000000000ULL + current_wall_ts.tv_nsec;
        uint64_t last_walltime = (uint64_t)last_wall_ts.tv_sec * 1000000000ULL + last_wall_ts.tv_nsec;

        if (current_walltime - last_walltime >= cpu_check_period && dynamic_pebs_enabled) {
            // Calculate CPU usage over the last period for this thread
            struct timespec current_cpu_ts;
            clock_gettime(CLOCK_THREAD_CPUTIME_ID, &current_cpu_ts);
            uint64_t current_cputime = (uint64_t)current_cpu_ts.tv_sec * 1000000000ULL + current_cpu_ts.tv_nsec;
            uint64_t last_cputime = (uint64_t)last_cpu_ts.tv_sec * 1000000000ULL + last_cpu_ts.tv_nsec;

            if (current_cputime > last_cputime) {
                uint64_t cputime_diff = current_cputime - last_cputime;
                uint64_t walltime_diff = current_walltime - last_walltime;

                double instant_cpu_usage = (double)cputime_diff / walltime_diff * 100.0;

                // Apply exponential moving average with alpha = 0.2
                cpu_usage_ema = 0.2 * instant_cpu_usage + 0.8 * cpu_usage_ema;

                LOG_DEBUG("PACT: CPU usage EMA: %.2f%%, target: %d%%\n", cpu_usage_ema, pact_cpu_quota);

                // Hysteresis control: ±0.5% tolerance band
                if (cpu_usage_ema > pact_cpu_quota + 0.5) {
                    // CPU usage too high, increase sample period
                    pact_increase_sample_period();
                    uint64_t new_period = pact_get_sample_period(pact_sample_period_idx);
                    pact_update_sample_period(new_period);
                    LOG_DEBUG("PACT: Increased sample period to %lu (idx: %u)\n", new_period, pact_sample_period_idx);
                } else if (cpu_usage_ema < pact_cpu_quota - 0.5) {
                    // CPU usage too low, decrease sample period
                    pact_decrease_sample_period();
                    uint64_t new_period = pact_get_sample_period(pact_sample_period_idx);
                    pact_update_sample_period(new_period);
                    LOG_DEBUG("PACT: Decreased sample period to %lu (idx: %u)\n", new_period, pact_sample_period_idx);
                }
            }

            last_cpu_ts = current_cpu_ts;
            last_wall_ts = current_wall_ts;
        }

        int pebs_start_cpu = 0;
        int num_cores = PEBS_NPROCS;

        for (int cpu_idx = pebs_start_cpu; cpu_idx < pebs_start_cpu + num_cores; cpu_idx++) {
            for(int evt = 0; evt < NPBUFTYPES; evt++) {
                if (!perf_page[cpu_idx][evt]) {
                    continue;
                }
                p = perf_page[cpu_idx][evt];
                __u64 head, tail, data_offset, avail;
                bool cond;

                do {
                    LOG_START_PEBS(SAMPLE_READ);
                    LOG_START_PEBS(SAMPLE);

                    __sync_synchronize();

                    head = p->data_head;
                    tail = p->data_tail;
                    data_offset = p->data_offset;

                    if (head == tail) {
                        break;
                    }

                    rec.addr = 0;
                    char *data = (char*)p + data_offset;
                    avail = head - tail;

                    if (avail > (PERF_PAGES * PEBS_MAX_SAMPLE_RATIO)) {
                        cond = true;
                    } else {
                        cond = false;
                    }

                    if (avail < sizeof(struct perf_event_header)) {
                        break;
                    }

                    uint64_t data_size = p->data_size;
                    assert(((data_size - 1) & data_size) == 0); // ensure power of 2
                    assert(data_size != 0);

                    // header
                    uint64_t wrapped_tail = tail & (data_size - 1);  // modulo for power of 2
                    hdr = (struct perf_event_header *)(data + wrapped_tail);

                    assert(hdr->size != 0);
                    assert(avail >= hdr->size);

                    if (wrapped_tail + hdr->size <= data_size) {
                        switch (hdr->type) {
                            case PERF_RECORD_SAMPLE:
                                if (hdr->size - sizeof(struct perf_event_header) == sizeof(struct perf_sample)) {
                                    memcpy(&rec, data + wrapped_tail + sizeof(struct perf_event_header), sizeof(struct perf_sample));
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
                    if (rec.addr == 0) continue;
                    no_samples[cpu_idx][evt] = rdtscp();
                    LOG_END_PEBS(SAMPLE_READ);
                    LOG_START_PEBS(SAMPLE_LOOKUP);

                    uint64_t addr_aligned = rec.addr & PAGE_MASK;
                    struct pact_page *page = find_page_no_lock(addr_aligned);

                    // Try 4KB aligned page if not 2MB aligned page
                    if (page == NULL) {
                        page = find_page_no_lock(rec.addr & BASE_PAGE_MASK);
                    }
                    LOG_END_PEBS(SAMPLE_LOOKUP);
                    if (page == NULL) {
                        continue;
                    }
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
                    uint64_t cur_cyc = rdtscp();
                    if (page->pagr_pred && page->pagr_pred_time + mig_queue_time + mig_move_time < cur_cyc) {
                        page->pagr_accessed = true;
                    }
                    if (page->hem_pred && page->hem_pred_time + mig_queue_time + mig_move_time < cur_cyc) {
                        page->hem_accessed = true;
                    }
                    page->real_accessed = true;

                    if (evt == FASTREAD) {
                        page->in_fast = IN_FAST;
                        pebs_stats.fast_accesses++;
                    } else {
                        page->in_fast = IN_SLOW;
                        pebs_stats.slow_accesses++;
                    }

                    double pagr_prom_not_accessed_perc = (double)(pagr_prom_not_accessed + 1) / (pagr_promotions + 1);
                    double hem_prom_not_accessed_perc = (double)(hem_prom_not_accessed + 1) / (hem_promotions + 1);

                    if (pagr_prom_not_accessed_perc < hem_prom_not_accessed_perc) {
                        pact_mode = PAGR_MODE;
                    } else {
                        pact_mode = HEM_MODE;
                    }

                    
                    LOG_START_PEBS(SAMPLE_PRED);

            #if HEM_ALGO == 1
                    // cool off
                    page->accesses >>= (global_clock - page->local_clock);
                    page->local_clock = global_clock;

                    page->accesses++;
                    if (page->accesses >= HOT_THRESHOLD) {
                        page->hem_accessed = false;
                        page->hem_pred = true;
                        page->hem_pred_time = rdtscp();
                        if (pact_mode == HEM_MODE) {
                            make_hot_request(page);
                        }
                    } else {
                        if (pact_mode == HEM_MODE) {
                            make_cold_request(page);
                        }
                    }
                    LOG_END_PEBS(SAMPLE_PRED);
                    // Sample based cooling
                    samples_since_cool++;
                    if (samples_since_cool >= SAMPLE_COOLING_THRESHOLD) {
                        global_clock++;
                        samples_since_cool = 0;
                        last_cyc_cool = rdtscp();
                    }
            #endif 

                    
            #if PAGR_ALGO == 1
                    if (rec.time > page->cyc) {
                        page->cyc = rec.time;
                        page->ip = rec.ip;
                    }
                    LOG_START_PEBS(PAGR_ADD_PAGE);

                    uint8_t err = algo_add_page(page);
                    LOG_END_PEBS(PAGR_ADD_PAGE);

                    
                    double percent_fast = pebs_stats.fast_accesses / (pebs_stats.fast_accesses + pebs_stats.slow_accesses + 1);
                    if (err == 0 && cold_list.numentries != 0 && percent_fast < 1) {
                        struct pact_page *pred_pages[MAX_NEIGHBORS * MAX_PRED_DEPTH];
                        uint32_t idx = 0;
                        LOG_START_PEBS(PAGR_PRED);
                        algo_predict_pages(page, pred_pages, &idx);
                        LOG_END_PEBS(PAGR_PRED);

                        LOG_START_PEBS(PAGR_MAKE_HOT);
                        uint64_t cur_cyc = rdtscp();
                        for (uint32_t i = 0; i < idx; i++) {
                            pred_pages[i]->pagr_pred = true;
                            pred_pages[i]->pagr_accessed = false;
                            pred_pages[i]->pagr_pred_time = cur_cyc;
                            if (pact_mode == PAGR_MODE) {
                                make_hot_request(pred_pages[i]);
                            }
                        }
                        LOG_END_PEBS(PAGR_MAKE_HOT);
                        
                    }
                    LOG_END_PEBS(SAMPLE_PRED);
            #endif
            // #if SEQ_ALGO == 1
                    struct pact_page *next_page = find_page_no_lock(addr_aligned + PAGE_SIZE);

                    // Try 4KB aligned page if not 2MB aligned page
                    if (next_page != NULL) {
                        LOG_DEBUG("Found sequential page: 0x%lx\n", next_page->va);
                        make_hot_request(next_page);
                    } else {
                        LOG_DEBUG("No sequential page found for address: 0x%lx\n", addr_aligned + PAGE_SIZE);
                    }
            // #endif
            #if LRU_ALGO == 1
                    LOG_START_PEBS(SAMPLE_LRU);
                    make_cold_request(page);
                    LOG_END_PEBS(SAMPLE_LRU);
            #endif
                    LOG_END_PEBS(SAMPLE);
                } while (cond);

                no_samples[cpu_idx][evt]++;
                uint64_t cur_cyc = rdtscp();
                if (cur_cyc > no_samples[cpu_idx][evt] + NO_SAMPLE_RESET_TIME * (pact_sample_period_idx + 1)) {
                    pebs_stats.pebs_resets++;
                    ioctl(pfd[cpu_idx][evt], PERF_EVENT_IOC_DISABLE);
                    ioctl(pfd[cpu_idx][evt], PERF_EVENT_IOC_RESET);
                    ioctl(pfd[cpu_idx][evt], PERF_EVENT_IOC_ENABLE);
                    no_samples[cpu_idx][evt] = cur_cyc;

                    // pact_increase_sample_period();
                    // uint64_t new_period = pact_get_sample_period(pact_sample_period_idx);
                    // pact_update_sample_period(new_period);
                    // LOG_DEBUG("PACT: Increased sample period to %lu (idx: %u)\n", new_period, pact_sample_period_idx);
                }

                
            }
        }
        sleep_ms(sleep_timeout);
    }
    return NULL;
}

static uint64_t last_cyc = 0;

void pact_migrate_page(struct pact_page *page, int node) {
    unsigned long nodemask = 1UL << node;
    if (mbind(page->va_start, page->size, MPOL_BIND, &nodemask, 64, MPOL_MF_MOVE | MPOL_MF_STRICT) == -1) {
        perror("mbind");
        // LOG_DEBUG("mbind failed %p\n", page->va_start);
        pebs_stats.mig_failed++;
        if (node == FAST_NODE) {    // Tried to promote it
            enqueue_fifo(&hot_list, page);
        } else {                    // Tried to demote it
            enqueue_fifo(&cold_list, page);
        }
    } else {
        if (node == FAST_NODE) {
            page->real_pred = true;
            page->real_accessed = false;
            
#if RECORD == 1
            // record promotion
            struct pebs_rec p_rec = {
                .va = page->va,
                .ip = 0,
                .cyc = rdtscp(),
                .cpu = 0,
                .evt = 0
            };
            fwrite(&p_rec, sizeof(struct pebs_rec), 1, promote_pred_fp);
#endif
            // was migrated to fast
            pebs_stats.promotions++;
            page->in_fast = IN_FAST;
#if LRU_ALGO == 1
            enqueue_fifo(&cold_list, page);
#else
            enqueue_fifo(&hot_list, page);
#endif

        } else {
            if (page->real_pred) {
                real_promotions++;
                if (!page->real_accessed) {
                    real_prom_not_accessed++;
                }
                page->real_pred = false;
            }
            if (page->pagr_pred) {
                pagr_promotions++;
                if (!page->pagr_accessed) {
                    pagr_prom_not_accessed++;
                }
                page->pagr_pred = false;
            }
            if (page->hem_pred) {
                hem_promotions++;
                if (!page->hem_accessed) {
                    hem_prom_not_accessed++;
                }
                page->hem_pred = false;
            }

            uint64_t cur_cyc = rdtscp();

            if (cur_cyc - last_cyc > 2 * CPU_FREQ) {
                last_cyc = cur_cyc;
                real_promotions >>= 1;
                real_prom_not_accessed >>= 1;
                hem_promotions >>= 1;
                hem_prom_not_accessed >>= 1;
                pagr_promotions >>= 1;
                pagr_prom_not_accessed >>= 1;
            }
#if RECORD == 1
            // record demotion
            struct pebs_rec p_rec = {
                .va = page->va,
                .ip = 0,
                .cyc = rdtscp(),
                .cpu = 0,
                .evt = 0
            };
            fwrite(&p_rec, sizeof(struct pebs_rec), 1, demote_pred_fp);
#endif
            pebs_stats.demotions++;
            page->in_fast = IN_SLOW;
        }
    }
}
void *demote_thread() {
    internal_call = true;
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(DEMOTE_CPU, &cpuset);
    int s = pthread_setaffinity_np(internal_threads[DEMOTE_THREAD], sizeof(cpu_set_t), &cpuset);
    assert(s == 0);


    while (true) {
#if FAST_BUFFER != 0
        // hacky way to update fast_used every second in case there's drift over time
        fast_size = numa_node_size(FAST_NODE, &fast_free);
        fast_used = fast_size - fast_free;
        fast_size -= FAST_BUFFER;

        long slow_free;
        long slow_size = numa_node_size(SLOW_NODE, &slow_free);
        slow_used = slow_size - slow_free;
#endif
        int bytes_demoted = 0;
        while (fast_free + bytes_demoted < FAST_BUFFER) {
            struct pact_page *cold_page = dequeue_fifo(&cold_list);
            if (cold_page == NULL) {
                // LOG_DEBUG("MIG: no cold pages, aborting\n");
                break;
            }
            assert(cold_page != NULL);
            pthread_mutex_lock(&cold_page->page_lock);
            if (cold_page->list != NULL || cold_page->in_fast == IN_SLOW) {
                // page got yoinked
                pthread_mutex_unlock(&cold_page->page_lock);
                continue;
            }
            assert(cold_page->in_fast == IN_FAST);
            assert(cold_page->list == NULL);

            // pact_migrate_pages(&cold_page, 1, SLOW_NODE);
#if RECORD == 1
            // record prediction
            struct pebs_rec p_rec = {
                .va = cold_page->va,
                .ip = 0,
                .cyc = rdtscp(),
                .cpu = 0,
                .evt = 0
            };
            fwrite(&p_rec, sizeof(struct pebs_rec), 1, demote_pred_fp);
#endif
            pact_migrate_page(cold_page, SLOW_NODE);
            bytes_demoted += cold_page->size;
            // LOG_DEBUG("MIG: demoted 0x%lx\n", cold_page->va);
            pthread_mutex_unlock(&cold_page->page_lock);
        }
        // sleep(0.01);
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

    struct pact_page *hot_page;

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
        
        // LOG_DEBUG("MIG: got hot page: 0x%lx\n", hot_page->va);

        uint64_t mig_queue_cyc = rdtscp();
        uint64_t mig_queue_diff = mig_queue_cyc - hot_page->mig_start;
        mig_queue_time = DEC_MIG_TIME * mig_queue_diff + (1.0 - DEC_MIG_TIME) * mig_queue_time;
        pact_migrate_page(hot_page, FAST_NODE);

        // LOG_DEBUG("MIG: Finished migration: 0x%lx\n", hot_page->va);

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

    

    int pebs_start_cpu = 0;
    int num_cores = PEBS_NPROCS;
    
    for (int i = pebs_start_cpu; i < pebs_start_cpu + num_cores; i++) {
        perf_page[i][FASTREAD] = perf_setup(0x1d3, 0, i, i * 2, FASTREAD);      // MEM_LOAD_L3_MISS_RETIRED.LOCAL_FAST, mem_load_uops_l3_miss_retired.local_dram
        perf_page[i][REMREAD] = perf_setup(0x4d3, 0, i, i * 2, REMREAD);     //  mem_load_uops_l3_miss_retired.remote_fast
        no_samples[i][FASTREAD] = 0;
        no_samples[i][REMREAD] = 0;
    }

    start_pebs_thread();

#if HEM_ALGO == 1 || PAGR_ALGO == 1
    start_promote_thread();

    start_demote_thread();
#endif
    internal_call = false;
}