#include "algorithm.h"
#include <float.h>

double mig_queue_time = 0;
double mig_move_time = 0;

#if PAGR_ALGO == 1

#define ABS(x) ((x) >= 0 ? (x) : -(x))
#define MIN(a, b) ({ \
    typeof(a) _a = (a); \
    typeof(b) _b = (b); \
    _a < _b ? _a : _b; \
})

#define MAX(a, b) ({ \
    typeof(a) _a = (a); \
    typeof(b) _b = (b); \
    _a > _b ? _a : _b; \
})

#define CLIP(x, a, b) (MIN(MAX((x), (a)), (b)))

#ifndef VA_WEIGHT
#define VA_WEIGHT 2
#endif

#ifndef CYC_WEIGHT
#define CYC_WEIGHT 1
#endif

#ifndef IP_WEIGHT
#define IP_WEIGHT 1
#endif

#ifndef DEC_FAST
#define DEC_FAST 0.01
#endif

#ifndef DEC_SLOW
#define DEC_SLOW 0.0002
#endif

#ifndef DEC_DIST
#define DEC_DIST 0.0001
#endif

#ifndef NEIGHBOR_DEC
#define NEIGHBOR_DEC 1.1
#endif



struct pact_page *page_history[HISTORY_SIZE];
uint32_t page_his_idx = 0;
double mig_time = 0;


double avg_dist = 1;
double bot_dist = 1;


static double top_va = 2, bot_va = 1;
static double top_cyc = 2, bot_cyc = 1;
static double top_ip = 2, bot_ip = 1;

// Trends towards upper part of range but still less than max
static inline double update_top(double top, double val) {
    if (val < top) {
        return DEC_SLOW * val + (1.0 - DEC_SLOW) * top;
    }
    return DEC_FAST * val + (1.0 - DEC_FAST) * top;
}

// Trends towards lower part of range but still greater than min
static inline double update_bot(double bot, double val) {
    if (val < bot) {
        return DEC_FAST * val + (1.0 - DEC_FAST) * bot;
    }
    return DEC_SLOW * val + (1.0 - DEC_SLOW) * bot;
}

static double calc_distance(struct pact_page *a, struct pact_page *b) {
    double distance = 0;
    // double x = 5;
    // printf("%f -> %f\n", (double)(a->va) - (double)(b->va), ABS((double)(a->va) - (double)(b->va)));
    double va_diff = ABS((double)(a->va) - (double)(b->va));
    double cyc_diff = ABS((double)(a->cyc) - (double)(b->cyc));
    double ip_diff = ABS((double)(a->ip) - (double)(b->ip));

    // Normalization
    double va_diff_clip = CLIP(va_diff, bot_va / 10, top_va * 10);
    double cyc_diff_clip = CLIP(cyc_diff, bot_cyc / 10, top_cyc * 10);
    double ip_diff_clip = CLIP(ip_diff, bot_ip / 10, top_ip * 10);

    top_va = update_top(top_va, va_diff_clip);
    top_cyc = update_top(top_cyc, cyc_diff_clip);
    top_ip = update_top(top_ip, ip_diff_clip);

    bot_va = update_bot(bot_va, va_diff_clip);
    bot_cyc = update_bot(bot_cyc, cyc_diff_clip);
    bot_ip = update_bot(bot_ip, ip_diff_clip);

    va_diff = ABS((va_diff - bot_va) / (top_va - bot_va));
    cyc_diff = ABS((cyc_diff - bot_cyc) / (top_cyc - bot_cyc));
    ip_diff = ABS((ip_diff - bot_ip) / (top_ip - bot_ip));

    // LOG_DEBUG("va: %f, cyc: %f, ip: %f\n", va_diff, cyc_diff, ip_diff);


    distance += va_diff * VA_WEIGHT;
    distance += cyc_diff * CYC_WEIGHT;
    distance += ip_diff * IP_WEIGHT;

    // if (distance == 0) return ;

    // double percent_fast = pebs_stats.fast_accesses / (pebs_stats.fast_accesses + pebs_stats.slow_accesses + 1);

    double dist_clip = CLIP(distance, bot_dist / 10, avg_dist * 10);
    double prom_hit_perc = 1 - (double)(real_prom_not_accessed) / (real_promotions + 1);
    // higher miss percentage should make it less likely to be promoted (lower threshold)
    // miss % = 0 -> doing great, higher threshold
    // miss % = 1 -> doing terrible, lower threshold
    // bot_dist = update_bot(bot_dist, dist_clip);
    bot_dist = update_bot(bot_dist, dist_clip * prom_hit_perc * prom_hit_perc * prom_hit_perc * prom_hit_perc);
    // bot_dist = update_bot(bot_dist, distance * (1 - percent_fast * percent_fast));

    // when the percent is good you want it to do less (lower threshold)
    // when the percent is bad you want it to do more (higher threshold)


    avg_dist = DEC_DIST * dist_clip + (1.0 - DEC_DIST) * avg_dist;

    return distance;
}

// Update the neighbors for the page kicked out of the page_history buffer
static void update_neighbors(struct pact_page *old_page)
{
    LOG_START_PEBS(PAGR_UPDATE_NEIGHBOR);

    // cool neighbors
    for (uint32_t i = 0; i < MAX_NEIGHBORS; i++) {
        old_page->neighbors[i].distance *= NEIGHBOR_DEC;
    }

    for (uint32_t i = 0; i < HISTORY_SIZE; i++) {
        struct pact_page *cur_page = page_history[i];
        if (cur_page == old_page) continue;

        double distance = calc_distance(old_page, cur_page);
        // assert(distance != 0);
        
        // Find empty spot or furthest distance neighbor O(MAX_NEIGHBORS)
        struct neighbor_page *furthest_neighbor = NULL;
        for (uint32_t j = 0; j < MAX_NEIGHBORS; j++) {
            if (old_page->neighbors[j].page == cur_page) {
                // already a neighbor, update and continue
                // LOG_DEBUG("Already a neighbor\n");
                furthest_neighbor = &old_page->neighbors[j];
                furthest_neighbor->distance = 0;
                break;
            }
            if (old_page->neighbors[j].page == NULL) {  // empty spot
                // LOG_DEBUG("Empty spot\n");
                assert(old_page->neighbors[j].distance == 0);
                assert(old_page->neighbors[j].time_diff == 0);
                // printf("found empty spot\n");
                furthest_neighbor = &old_page->neighbors[j];
                break;
            }

            if (furthest_neighbor == NULL || old_page->neighbors[j].distance > furthest_neighbor->distance) {
                furthest_neighbor = &old_page->neighbors[j];
            }
        }

        // Replace furthest page with cur page if it's closer
        // printf("furthest: %f, distance: %f\n", furthest_neighbor->distance, distance);
        if (furthest_neighbor->distance == 0 || distance < furthest_neighbor->distance) {
            // printf("adding page\n");
            furthest_neighbor->page = cur_page;
            furthest_neighbor->distance = distance;
            furthest_neighbor->time_diff = cur_page->cyc - old_page->cyc;
        }
        
    }
    LOG_END_PEBS(PAGR_UPDATE_NEIGHBOR);
    // printf("Neighbors:\t");
    // for (uint32_t i = 0; i < MAX_NEIGHBORS; i++) {
    //     if (old_page->neighbors[i].page != NULL)
    //         printf("0x%lx, ", old_page->neighbors[i].page->va);
    // }
    // printf("\n");
}

uint8_t algo_add_page(struct pact_page *page)
{
    struct pact_page *old_page = NULL;
    uint32_t old_idx = 0;

    uint64_t min_cyc = UINT64_MAX;

    // Single pass: find oldest + detect duplicate
    for (uint32_t i = 0; i < HISTORY_SIZE; i++) {
        struct pact_page *p = page_history[i];

        if (!p) {
            page_history[i] = page;
            return 0;
        }

        // Skip if same VA as last inserted (duplicate suppression)
        // if (p->va == page->va)
        //     return 1;

        if (p->cyc < min_cyc) {
            min_cyc = p->cyc;
            old_page = p;
            old_idx = i;
        }
    }

    update_neighbors(old_page);

    page_history[old_idx] = page;

    return 0;
}

void algo_predict_pages(struct pact_page *page, struct pact_page **pred_pages, uint32_t *idx) {
    if (pebs_stats.throttles > pebs_stats.unthrottles) return;
    // record_sample(page); //29

    if (hot_list.numentries == 0) {
        mig_queue_time = 0;
    }

    assert(*idx == 0);
    // double threshold = avg_dist / 4000;
    // LOG_DEBUG("Threshold: %.2e, avg_dist: %.2e\n", bot_dist, avg_dist);
    
#if ALL_ALGO == 1
    for (uint32_t i = 0; i < MAX_NEIGHBORS; i++) {
        if (page->neighbors[i].distance != 0) {
            pred_pages[(*idx)++] = page->neighbors[i].page;
        }
    }
#endif

#if DFS_ALGO == 1
    // DFS
    double threshold = bot_dist;
    uint64_t tot_time_diff = 0;
    struct pact_page *cur_page = page;
    for (uint32_t d = 0; d < MAX_PRED_DEPTH; d++) {
        struct neighbor_page *closest_neighbor = NULL;
        // if (d > 1) {
        //     LOG_DEBUG("PRED: Depth=%u\n", d);
        // }
        for (uint32_t i = 0; i < MAX_NEIGHBORS; i++) {
            if (cur_page->neighbors[i].distance != 0 && cur_page->neighbors[i].distance < threshold) {
                // found close neighbor
                if (closest_neighbor == NULL || cur_page->neighbors[i].distance < closest_neighbor->distance) {
                    closest_neighbor = &page->neighbors[i];
                }
                if (cur_page->neighbors[i].time_diff + tot_time_diff > mig_move_time + mig_queue_time) {
                    // Far enough into future to migrate
                    pred_pages[(*idx)++] = cur_page->neighbors[i].page;
                }
            }
        }
        if (closest_neighbor == NULL || closest_neighbor->page == NULL) break;
        cur_page = closest_neighbor->page;
        tot_time_diff += closest_neighbor->time_diff;
    }

#endif

}
#endif

