/* Device probe for the original 10-ms producer / 20-ms blocking write contract. */
#define _GNU_SOURCE
#include <aaudio/AAudio.h>
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

static int64_t ns(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec;
}
int main(int argc, char **argv)
{
    int paced_by_device = argc > 1 && argv[1][0] == 'd';
    AAudioStreamBuilder *b = NULL; AAudioStream *s = NULL;
    assert(AAudio_createStreamBuilder(&b) == AAUDIO_OK);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(b, 2);
    AAudioStreamBuilder_setSampleRate(b, 48000);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    if (paced_by_device) AAudioStreamBuilder_setBufferCapacityInFrames(b, 4800);
    assert(AAudioStreamBuilder_openStream(b, &s) == AAUDIO_OK);
    AAudioStreamBuilder_delete(b);
    int burst = AAudioStream_getFramesPerBurst(s), capacity = AAudioStream_getBufferCapacityInFrames(s);
    if (paced_by_device) AAudioStream_setBufferSizeInFrames(s, capacity);
    printf("mode=%s rate=%d burst=%d capacity=%d buffer=%d perf=%d sharing=%d\n",
        paced_by_device ? "device" : "original", AAudioStream_getSampleRate(s), burst, capacity,
        AAudioStream_getBufferSizeInFrames(s), AAudioStream_getPerformanceMode(s), AAudioStream_getSharingMode(s));
    int16_t pcm[960] = {0};
    if (paced_by_device) assert(AAudioStream_write(s, pcm, 480, 0) == 480);
    assert(AAudioStream_requestStart(s) == AAUDIO_OK);
    int64_t next = ns(), begin = next, accepted = 0, produced = 0, max_write = 0, max_late = 0;
    int partial = 0, zero = 0, errors = 0;
    for (int i = 0; i < 2000; ++i) {
        if (!paced_by_device) {
            next += 10000000;
            struct timespec due = {next / 1000000000, next % 1000000000};
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &due, NULL);
            int64_t late = ns() - next; if (late > max_late) max_late = late;
        }
        produced += 480;
        int left = 480;
        do {
            int64_t start = ns();
            int result = AAudioStream_write(s, pcm + 2 * (480 - left), left, paced_by_device ? 100000000 : 20000000);
            int64_t elapsed = ns() - start; if (elapsed > max_write) max_write = elapsed;
            if (result < 0) { ++errors; printf("write error %d\n", result); break; }
            if (!result) ++zero;
            else { accepted += result; left -= result; }
            if (left) ++partial;
        } while (paced_by_device && left);
    }
    printf("produced=%" PRId64 " accepted=%" PRId64 " dropped=%" PRId64 " partial=%d zero=%d errors=%d xruns=%d elapsed=%.3f max_write_ms=%.3f max_late_ms=%.3f framesWritten=%" PRId64 " framesRead=%" PRId64 "\n",
        produced, accepted, produced - accepted, partial, zero, errors, AAudioStream_getXRunCount(s),
        (ns() - begin) / 1e9, max_write / 1e6, max_late / 1e6, AAudioStream_getFramesWritten(s), AAudioStream_getFramesRead(s));
    AAudioStream_requestStop(s); AAudioStream_close(s);
    return 0;
}
