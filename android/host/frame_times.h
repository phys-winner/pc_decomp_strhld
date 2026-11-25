#ifndef PORT_FRAME_TIMES_H
#define PORT_FRAME_TIMES_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#define PORT_FRAME_TIMES 304u

typedef struct {
    pthread_mutex_t lock;
    uint64_t last_ns, count;
    int32_t intervals_us[PORT_FRAME_TIMES];
} PortFrameTimes;

#define PORT_FRAME_TIMES_INIT { PTHREAD_MUTEX_INITIALIZER, 0, 0, {0} }

/* Call once per successful present with a monotonic timestamp in nanoseconds. */
static inline void port_frame_presented(PortFrameTimes *frames, uint64_t now_ns)
{
    pthread_mutex_lock(&frames->lock);
    if (now_ns > frames->last_ns) {
        if (frames->last_ns) {
            uint64_t us = (now_ns - frames->last_ns) / 1000;
            frames->intervals_us[frames->count % PORT_FRAME_TIMES] = us > INT32_MAX ? INT32_MAX : (int32_t)us;
            ++frames->count;
        }
        frames->last_ns = now_ns;
    }
    pthread_mutex_unlock(&frames->lock);
}

/* One coherent snapshot in the order expected by FpsOverlay.Source. */
static inline uint64_t port_frame_times(PortFrameTimes *frames, int32_t *out, size_t length)
{
    pthread_mutex_lock(&frames->lock);
    uint64_t count = frames->count;
    size_t recorded = count < PORT_FRAME_TIMES ? (size_t)count : PORT_FRAME_TIMES;
    for (size_t i = 0; i < length; ++i) {
        size_t back = length - i;
        out[i] = back <= recorded ? frames->intervals_us[(count - back) % PORT_FRAME_TIMES] : 0;
    }
    pthread_mutex_unlock(&frames->lock);
    return count;
}

#endif
