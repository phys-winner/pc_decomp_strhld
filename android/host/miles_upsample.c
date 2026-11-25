/* Bounded linear resampling for the pinned Miles mixer. */
#include "host.h"

#define MILES_PHASE_STEP 0x2114C400u
#define MILES_SOURCE_END 0x2114C418u
#define MILES_TARGET_END 0x2114C41Cu
#define MILES_CURRENT_SAMPLE 0x2114C42Cu
#define MILES_CURRENT_RIGHT 0x2114C430u
#define MILES_PREVIOUS_RIGHT 0x2114C434u

static inline uint32_t load_pcm(const uint8_t *source, unsigned wide)
{
    if (!wide) return (uint32_t)*source - 128u;
    int16_t sample;
    memcpy(&sample, source, sizeof sample);
    return (uint32_t)(int32_t)sample;
}

static inline uint32_t interpolate(uint32_t previous, uint32_t current, uint32_t phase)
{
    uint32_t product = (current - previous) * (phase >> 16);
    return previous + (uint32_t)((int32_t)product >> 16);
}

static inline __attribute__((always_inline)) uint32_t mix_run(uint32_t state[4], unsigned mode,
                                                               uint32_t count, uint32_t step)
{
    unsigned wide = (mode >> 1) & 1u, stereo = mode & 1u, paired = (mode >> 2) & 1u;
    unsigned split = paired && (stereo || (mode & 8u));
    unsigned source_frame = (1u + wide) * (1u + paired);
    const uint8_t *source = GPTR(state[0]);
    uint32_t *target = GPTR(state[1]);
    uint32_t phase = state[2], previous = state[3];
    uint32_t current = MEM32(MILES_CURRENT_SAMPLE);
    uint32_t current_right = split ? MEM32(MILES_CURRENT_RIGHT) : 0;
    uint32_t previous_right = split ? MEM32(MILES_PREVIOUS_RIGHT) : 0;
    unsigned shift = wide ? 11 : 19;
    uint32_t left = mode & 8u ? MEM32(0x2114C410u) : 1u << shift;
    uint32_t right = mode & 8u ? MEM32(0x2114C414u) : left;

    for (uint32_t i = 0; i < count; ++i) {
        uint32_t sample = interpolate(previous, current, phase);
        uint32_t right_sample = split ? interpolate(previous_right, current_right, phase) : sample;
        uint32_t mix = sample * left;
        if (!stereo && split) mix += right_sample * right;
        target[0] += mix;
        if (stereo) target[1] += right_sample * right;
        target += 1u + stereo;

        uint32_t next = phase + step;
        if (next < phase) {
            source += source_frame;
            previous = current;
            current = load_pcm(source, wide);
            if (paired) {
                uint32_t sample_right = load_pcm(source + 1u + wide, wide);
                if (split) {
                    previous_right = current_right;
                    current_right = sample_right;
                } else {
                    current += sample_right;
                }
            }
        }
        phase = next;
    }
    uint32_t carries = ((uint64_t)state[2] + (uint64_t)count * step) >> 32;
    MEM32(MILES_CURRENT_SAMPLE) = current;
    if (split) {
        MEM32(MILES_CURRENT_RIGHT) = current_right;
        MEM32(MILES_PREVIOUS_RIGHT) = previous_right;
    }
    state[0] += carries * source_frame;
    state[1] += count * 4u * (1u + stereo);
    state[2] = phase;
    state[3] = previous;
    return count + carries;
}

__attribute__((visibility("hidden")))
uint32_t stronghold_miles_upsample(uint32_t state[4], unsigned mode, uint32_t budget)
{
    uint32_t source = state[0], target = state[1], phase = state[2];
    uint32_t source_end = MEM32(MILES_SOURCE_END), target_end = MEM32(MILES_TARGET_END);
    uint32_t step = MEM32(MILES_PHASE_STEP);
    unsigned src_frame = (1u + ((mode >> 1) & 1u)) * (1u + ((mode >> 2) & 1u));
    unsigned dst_frame = 4u * (1u + (mode & 1u));
    uint32_t frames = (target_end - target) / dst_frame;
    if (mode >= 16 || budget < 8 || frames < 9 || (target & 3u) || source_end <= source || target_end <= target ||
        (source_end - source) % src_frame || (target_end - target) % dst_frame ||
        !void_vm_readable(source, source_end - source) || !void_vm_writable(target, target_end - target) ||
        (source < target_end && target < source_end) ||
        (source < MILES_PREVIOUS_RIGHT + 4 && source_end > MILES_PHASE_STEP) ||
        (target < MILES_PREVIOUS_RIGHT + 4 && target_end > MILES_PHASE_STEP)) return 0;

    uint32_t count = frames - 1 < 16384u ? frames - 1 : 16384u;
    if (budget > 32768u) budget = 32768u;
    /* Each carry loads one source frame and consumes an extra guest backedge. */
    uint64_t source_limit = ((uint64_t)((source_end-source)/src_frame) << 32) - 1u - phase;
    if ((uint64_t)count * step > source_limit) count = source_limit / step;
    uint64_t per_edge = (UINT64_C(1) << 32) + step;
    uint64_t edge_limit = ((uint64_t)(budget + 1u) << 32) - 1u - phase;
    if ((uint64_t)count * per_edge > edge_limit) count = edge_limit / per_edge;
    if (!count) return 0;

#define RUN(n) case n: return mix_run(state,n,count,step)
    switch (mode) {
        RUN(0); RUN(1); RUN(2); RUN(3); RUN(4); RUN(5); RUN(6); RUN(7);
        RUN(8); RUN(9); RUN(10); RUN(11); RUN(12); RUN(13); RUN(14); RUN(15);
    }
#undef RUN
    return 0;
}
