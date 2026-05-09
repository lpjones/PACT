#ifndef _LOGGING_HEADER
#define _LOGGING_HEADER

#include <stdio.h>
#include <assert.h>

#include "pact.h"

extern FILE* stats_fp;
extern FILE* debug_fp;
extern FILE* pebs_fp;       // SAMPLE, RESET, PRED
extern FILE* pact_trace_fp;
extern FILE* pred_fp;
extern FILE* promote_pred_fp;
extern FILE* demote_pred_fp;

extern uint64_t log_start_time;

// PEBS profiling enum
enum {
    CPU_USAGE,
    SAMPLE_WHOLE,
    SAMPLE_READ,
    SAMPLE_LOOKUP,
    MODE_SWITCH,
    SAMPLE_COOL,
    HEM_PRED,
    PAGR_PRED,
    PAGR_ADD_PAGE,
    PAGR_UPDATE_NEIGHBOR,
    PAGR_MAKE_HOT,
    SAMPLE_LRU,
    SAMPLE_FINISH,
    SAMPLE_RESET,
    PEBS_PROF_NP
};

struct __attribute__((packed)) time_rec {
    double ts;
    uint8_t group;
};

#define PEBS_PROF_BUF_SIZE 4096
extern struct time_rec pebs_prof_buf[PEBS_PROF_BUF_SIZE];
extern uint32_t pebs_prof_idx;
extern uint64_t pebs_start_times[PEBS_PROF_NP];

// #define LOG_DEBUG(...) { fprintf(debug_fp, __VA_ARGS__); fflush(debug_fp); }
#if RECORD == 1

#define LOG_DEBUG(...)                                                      \
    do {                                                                    \
        fprintf(debug_fp, "[%.9f]\t", elapsed_time(log_start_time, rdtscp()));                  \
        fprintf(debug_fp, __VA_ARGS__);                                      \
        fflush(debug_fp);                                                    \
    } while (0)


#else
#define LOG_DEBUG(...)
#endif

#if RECORD_TIMING == 1
#define LOG_START_PEBS(grp)                                                   \
    do {                     \
        pebs_start_times[grp] = rdtscp();                                                \
    } while (0);
#define LOG_END_PEBS(grp) \
    do { \
        pebs_prof_buf[pebs_prof_idx].ts = elapsed_time(pebs_start_times[grp], rdtscp()); \
        pebs_prof_buf[pebs_prof_idx++].group = (grp); \
        \
        if (pebs_prof_idx >= PEBS_PROF_BUF_SIZE) {                          \
            fwrite(pebs_prof_buf, sizeof(struct time_rec), PEBS_PROF_BUF_SIZE, pebs_fp); \
            pebs_prof_idx = 0;  \
        }   \
    } while (0);
#else
#define LOG_START_PEBS(...)
#define LOG_END_PEBS(...)
#endif

#if PEBS_STATS == 1
#define LOG_STATS(...) { fprintf(stats_fp, __VA_ARGS__); fflush(stats_fp); }
#endif

void init_log_files();


#endif