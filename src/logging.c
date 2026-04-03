#include "logging.h"

#if PEBS_STATS == 1
FILE* stats_fp = NULL;
#endif

#if RECORD == 1
FILE* debug_fp = NULL;
FILE* pact_trace_fp = NULL;
#endif

#if RECORD_TIMING == 1
FILE* pebs_fp = NULL;       // SAMPLE, RESET, PRED
FILE* promote_fp = NULL;    // PROMOTE
FILE* demote_fp = NULL;     // DEMOTE
struct time_rec pebs_prof_buf[PEBS_PROF_BUF_SIZE] = {0};
uint32_t pebs_prof_idx = 0;
#endif

struct timespec log_start_time;

void init_log_files() {
    internal_call = true;

#if PEBS_STATS == 1
    stats_fp = fopen("stats.txt", "w");
    assert(stats_fp != NULL);
#endif

#if RECORD == 1
    debug_fp = fopen("debuglog.txt", "w");
    assert(debug_fp != NULL);

    pact_trace_fp = fopen("pact_trace.bin", "wb");
    assert(pact_trace_fp != NULL);
#endif

#if RECORD_TIMING == 1
    pebs_fp = fopen("pebs.bin", "wb");
    assert(pebs_fp != NULL);

    promote_fp = fopen("promote.bin", "wb");
    assert(promote_fp != NULL);

    demote_fp = fopen("demote.bin", "wb");
    assert(demote_fp != NULL);
#endif

    internal_call = false;

    // reference start time
    log_start_time = get_time();
}