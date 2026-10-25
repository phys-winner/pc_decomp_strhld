/* Haptic cue bits and a ring of recent frame costs. The Java layer polls
 * both; nothing here touches guest memory. */
#include "feedback.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static _Atomic int pending_feedback;

void stronghold_feedback(int bits) { atomic_fetch_or_explicit(&pending_feedback, bits, memory_order_relaxed); }
int stronghold_feedback_take(void) { return atomic_exchange_explicit(&pending_feedback, 0, memory_order_relaxed); }

enum { PERF_FRAMES = 240 };
static pthread_mutex_t perf_lock = PTHREAD_MUTEX_INITIALIZER;
static float busy_ms[PERF_FRAMES];
static uint64_t ended_ns[PERF_FRAMES];
static unsigned perf_next, perf_count;
static uint64_t frame_started_ns;

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

void stronghold_perf_frame_begin(void) { frame_started_ns = now_ns(); }

void stronghold_perf_frame_end(void)
{
    if (!frame_started_ns) return;
    uint64_t end = now_ns();
    pthread_mutex_lock(&perf_lock);
    busy_ms[perf_next] = (float)((double)(end - frame_started_ns) / 1e6);
    ended_ns[perf_next] = end;
    perf_next = (perf_next + 1) % PERF_FRAMES;
    if (perf_count < PERF_FRAMES) perf_count++;
    pthread_mutex_unlock(&perf_lock);
    frame_started_ns = 0;
}

void stronghold_perf_reset(void)
{
    pthread_mutex_lock(&perf_lock);
    perf_next = perf_count = 0;
    pthread_mutex_unlock(&perf_lock);
}

static int compare_float(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

int stronghold_perf_stats(float stats[4])
{
    float sorted[PERF_FRAMES];
    pthread_mutex_lock(&perf_lock);
    unsigned count = perf_count;
    memcpy(sorted, busy_ms, sizeof sorted);
    uint64_t newest = ended_ns[(perf_next + PERF_FRAMES - 1) % PERF_FRAMES];
    uint64_t oldest = ended_ns[(perf_next + PERF_FRAMES - count) % PERF_FRAMES];
    pthread_mutex_unlock(&perf_lock);
    memset(stats, 0, 4 * sizeof *stats);
    if (count < 30) return 0;
    /* Only the filled entries take part; the ring keeps them contiguous modulo its size. */
    float values[PERF_FRAMES];
    for (unsigned i = 0; i < count; i++) values[i] = sorted[(perf_next + PERF_FRAMES - count + i) % PERF_FRAMES];
    double sum = 0;
    for (unsigned i = 0; i < count; i++) sum += values[i];
    qsort(values, count, sizeof *values, compare_float);
    stats[0] = (float)(sum / count);
    stats[1] = values[(count * 95 + 99) / 100 - 1];
    stats[2] = newest > oldest ? (float)((count - 1) * 1e9 / (double)(newest - oldest)) : 0;
    stats[3] = (float)count;
    return 1;
}
