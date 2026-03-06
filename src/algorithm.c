#include "algorithm.h"
#include <math.h>

#define ABS(x) ((x) >= 0 ? (x) : -(x))

#ifndef VA_WEIGHT
#define VA_WEIGHT 1
#endif

#ifndef CYC_WEIGHT
#define CYC_WEIGHT 1
#endif

#ifndef IP_WEIGHT
#define IP_WEIGHT 1
#endif

#ifndef DEC_UP
#define DEC_UP 0.01
#endif

#ifndef DEC_DOWN
#define DEC_DOWN 0.0001
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
double mig_queue_time = 0;
double mig_move_time = 0;

double avg_dist = 1;
double bot_dist = 1;


// static double top_va = 2, bot_va = 1;
// static double top_cyc = 2, bot_cyc = 1;
// static double top_ip = 2, bot_ip = 1;

// // Trends towards upper part of range but still less than max
// static double update_top(double top, double val) {
//     if (val > top) {
//         return DEC_UP * val + (1.0 - DEC_UP) * top;
//     }
//     return DEC_DOWN * val + (1.0 - DEC_DOWN) * top;
// }
#if CLUSTER_ALGO == 1

// Trends towards lower part of range but still greater than min
static inline double update_bot(double bot, double val) {
    if (val < bot / 10) {
        val = bot / 10;
    }
    if (val < bot) {
        return DEC_UP * val + (1.0 - DEC_UP) * bot;
    }
    if (val > bot * 10) {
        val = bot * 10;
    }
    // val = sqrt(val - bot) + bot;
    return DEC_DOWN * val + (1.0 - DEC_DOWN) * bot;
}

static double calc_distance(struct pact_page *a, struct pact_page *b) {
    double distance = 0;
    // double x = 5;
    // printf("%f -> %f\n", (double)(a->va) - (double)(b->va), ABS((double)(a->va) - (double)(b->va)));
    double va_diff = ABS((double)(a->va) - (double)(b->va));
    double cyc_diff = ABS((double)(a->cyc_accessed) - (double)(b->cyc_accessed));
    double ip_diff = ABS((double)(a->ip) - (double)(b->ip));

    // update ranges
    // top_va = update_top(top_va, va_diff);
    // top_cyc = update_top(top_cyc, cyc_diff);
    // top_ip = update_top(top_ip, ip_diff);

    // bot_va = update_bot(bot_va, va_diff);
    // bot_cyc = update_bot(bot_cyc, cyc_diff);
    // bot_ip = update_bot(bot_ip, ip_diff);

    // va_diff = (va_diff - bot_va) / (top_va - bot_va);
    // cyc_diff = (cyc_diff - bot_cyc) / (top_cyc - bot_cyc);
    // ip_diff = (ip_diff - bot_ip) / (top_ip - bot_ip);

    // printf("va: %f, cyc: %f, ip: %f\n", va_diff, cyc_diff, ip_diff);


    distance += va_diff * VA_WEIGHT;
    distance += cyc_diff * CYC_WEIGHT;
    distance += ip_diff * IP_WEIGHT;

    // if (distance == 0) return ;

    double percent_fast = pebs_stats.fast_accesses / (pebs_stats.fast_accesses + pebs_stats.slow_accesses + 1);

    bot_dist = update_bot(bot_dist, distance * (1 - percent_fast * percent_fast));

    // when the percent is good you want it to do less (lower threshold)
    // when the percent is bad you want it to do more (higher threshold)


    avg_dist = DEC_DIST * distance + (1.0 - DEC_DIST) * avg_dist;

    return distance;
}

static void update_neighbors(struct pact_page *old_page)
{
    LOG_PEBS(PAGR_UPDATE_NEIGHBOR);

    struct neighbor_page *neighbors = old_page->neighbors;
    uint64_t base_time = old_page->cyc_accessed;

    /* ---- Decay distances ---- */
    for (uint32_t i = 0; i < MAX_NEIGHBORS; i++) {
        old_page->neighbors[i].distance *= NEIGHBOR_DEC;
    }

    /* ---- Scan history ---- */
    for (uint32_t i = 0; i < HISTORY_SIZE; i++) {

        struct pact_page *cur_page = page_history[i];
        if (!cur_page || cur_page == old_page)
            continue;

        struct neighbor_page *slot = NULL;
        struct neighbor_page *worst = &neighbors[0];

        /* ---- Scan 4 neighbors (small fixed size) ---- */
        for (uint32_t j = 0; j < MAX_NEIGHBORS; j++) {

            struct neighbor_page *n = &neighbors[j];

            if (n->page == cur_page) {
                /* already neighbor -> refresh */
                n->distance = 0;
                goto next_page;
            }

            if (!n->page && !slot)
                slot = n;

            if (n->distance > worst->distance)
                worst = n;
        }

        struct neighbor_page *target = slot ? slot : worst;

        /* ---- Only now compute distance ---- */
        double distance = calc_distance(old_page, cur_page);

        if (!target->page || distance < target->distance) {
            target->page = cur_page;
            target->distance = distance;
            target->time_diff = cur_page->cyc_accessed - base_time;
        }

    next_page:
        ;
    }
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
        if (p->va == page->va)
            return 1;

        if (p->cyc_accessed < min_cyc) {
            min_cyc = p->cyc_accessed;
            old_page = p;
            old_idx = i;
        }
    }

    // Update neighbors of true oldest
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