#ifndef _LOGGING_HEADER
#define _LOGGING_HEADER

#include <stdio.h>
#include <assert.h>

#include "pact.h"

extern FILE* stats_fp;
extern FILE* debug_fp;
extern FILE* pebs_fp;       // SAMPLE, RESET, PRED
extern FILE* promote_fp;    // PROMOTE
extern FILE* demote_fp;     // DEMOTE
extern FILE* pact_trace_fp;

extern struct timespec log_start_time;

// PEBS profiling enum
enum {
    SAMPLE_READ_START,
    SAMPLE_LOOKUP_START,
    SAMPLE_COOL_START,
    SAMPLE_PRED_START,
#if CLUSTER_ALGO == 1
    PAGR_ADD_PAGE_START,
    PAGR_UPDATE_NEIGHBOR,
    PAGR_PRED_START,
    PAGR_MAKE_HOT_START,
#endif
#if LRU_ALGO == 1
    SAMPLE_LRU_START,
#endif
    SAMPLE_FINISH,
    SAMPLE_RESET_START,
    SAMPLE_RESET_FINISH,
    PEBS_PROF_NP
};

struct __attribute__((packed)) time_rec {
    double ts;
    uint8_t group;
};

#define PEBS_PROF_BUF_SIZE 4096
extern struct time_rec pebs_prof_buf[PEBS_PROF_BUF_SIZE];
extern uint32_t pebs_prof_idx;

// #define LOG_DEBUG(...) { fprintf(debug_fp, __VA_ARGS__); fflush(debug_fp); }
#if RECORD == 1

#define LOG_DEBUG(...)                                                      \
    do {                                                                    \
        fprintf(debug_fp, "[%.9f]\t",                                        \
                elapsed_time(log_start_time, get_time()));                  \
        fprintf(debug_fp, __VA_ARGS__);                                      \
        fflush(debug_fp);                                                    \
    } while (0)


#else
#define LOG_DEBUG(...)
#endif

#if RECORD_TIMING == 1
#define LOG_PEBS(grp)                                                   \
    do {                                                                    \
        pebs_prof_buf[pebs_prof_idx].ts = elapsed_time(log_start_time, get_time()),                 \
        pebs_prof_buf[pebs_prof_idx++].group = (grp);                                                  \
                                                  \
        if (pebs_prof_idx >= PEBS_PROF_BUF_SIZE) {                          \
            fwrite(pebs_prof_buf, sizeof(struct time_rec), PEBS_PROF_BUF_SIZE, pebs_fp); \
            pebs_prof_idx = 0;  \
        }   \
    } while (0)
#else
#define LOG_PEBS(...)
#endif

#if PEBS_STATS == 1
#define LOG_STATS(...) { fprintf(stats_fp, __VA_ARGS__); fflush(stats_fp); }
#endif

void init_log_files();


#endif