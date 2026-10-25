/* Exercise the production Android sink, with silent PCM and the real device clock. */
#define VOID_ANDROID_APP
#include "../android/host/android_main.c"
#include <assert.h>

uint64_t void_monotonic_ns(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}
void void_log(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); putchar('\n');
}
uint32_t void_tick_ms(void) { return (uint32_t)(void_monotonic_ns() / 1000000u); }
static void *paused_chunk(void *p) { void_audio_output(p, 480); return NULL; }
int main(void)
{
    int16_t pcm[960] = {0};
    uint64_t begin = void_monotonic_ns();
    for (int i = 0; i < 2000; ++i) {
        if (i == 1000) {
            uint64_t before = audio_sent;
            audio_set_paused(1);
            pthread_t writer; assert(!pthread_create(&writer, NULL, paused_chunk, pcm));
            usleep(60000);
            uint32_t frozen = void_audio_clock_ms();
            int64_t position = AAudioStream_getFramesRead(stream);
            usleep(100000);
            assert(void_audio_clock_ms() - frozen <= 1 && AAudioStream_getFramesRead(stream) == position);
            assert(audio_sent == before);
            audio_set_paused(0); pthread_join(writer, NULL);
            assert(audio_sent == before + 480 && !audio_reopens);
            puts("production pause: device cursor and Miles clock frozen; pending chunk retained; same stream resumed");
        } else void_audio_output(pcm, 480);
    }
    assert(stream && audio_dropped == 0 && audio_errors == 0 && audio_route_lost == 0);
    printf("production sink: 960000 frames, elapsed %.3f, xruns %d, requested/sent tail %llu/%llu, dropped %llu\n",
        (void_monotonic_ns() - begin) / 1e9, AAudioStream_getXRunCount(stream),
        (unsigned long long)audio_requested, (unsigned long long)audio_sent, (unsigned long long)audio_dropped);
    assert(audio_requested == audio_sent);
    void_audio_drain();
    return 0;
}
