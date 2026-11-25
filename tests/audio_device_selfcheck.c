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
    setvbuf(stdout, NULL, _IONBF, 0);
    int16_t pcm[960] = {0};
    uint64_t begin = void_monotonic_ns();
    for (int i = 0; i < 2000; ++i) {
        /* Keep counters cumulative; a telemetry interval may end across the pause. */
        audio_report_ns = void_monotonic_ns();
        if (i == 1000) {
            uint64_t before = audio_sent;
            uint32_t opens = audio_reopens;
            audio_set_paused(1);
            pthread_t writer; assert(!pthread_create(&writer, NULL, paused_chunk, pcm));
            usleep(60000);
            uint32_t frozen = void_audio_clock_ms();
            int64_t position = AAudioStream_getFramesRead(stream);
            uint64_t fifo_out = atomic_load(&audio_fifo_out);
            usleep(100000);
            printf("pause: clock delta %u ms, device cursor %lld -> %lld, FIFO cursor %llu -> %llu, state %d\n",
                void_audio_clock_ms() - frozen, (long long)position, (long long)AAudioStream_getFramesRead(stream),
                (unsigned long long)fifo_out, (unsigned long long)atomic_load(&audio_fifo_out), AAudioStream_getState(stream));
            assert(void_audio_clock_ms() == frozen && atomic_load(&audio_fifo_out) == fifo_out);
            assert(audio_sent == before);
            int clock_invalid = AAudioStream_getFramesRead(stream) > AAudioStream_getFramesWritten(stream);
            audio_set_paused(0); pthread_join(writer, NULL);
            assert(audio_sent == before + 480 && audio_reopens == opens + clock_invalid);
            printf("production pause: PCM cursor and Miles clock frozen; pending chunk retained; reopened %d\n", clock_invalid);
        } else void_audio_output(pcm, 480);
    }
    assert(stream && audio_dropped == 0 && audio_errors == 0 && audio_route_lost == 0);
    printf("production sink: 960000 frames, elapsed %.3f, xruns %d, requested/sent tail %llu/%llu, dropped %llu\n",
        (void_monotonic_ns() - begin) / 1e9, AAudioStream_getXRunCount(stream),
        (unsigned long long)audio_requested, (unsigned long long)audio_sent, (unsigned long long)audio_dropped);
    assert(audio_requested == audio_sent);
    printf("callback silence %u frames, device buffer %d frames, callback FIFO %llu frames\n",
        atomic_load(&audio_callback_silence), AAudioStream_getBufferSizeInFrames(stream),
        (unsigned long long)(atomic_load(&audio_fifo_in) - atomic_load(&audio_fifo_out)));
    void_audio_drain();
    return 0;
}
