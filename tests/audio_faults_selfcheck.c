/* Fault injection against the production sink: no skipped/duplicated PCM on short writes. */
#define VOID_ANDROID_APP
#include "../android/host/android_main.c"
#include <assert.h>

static int started, fail_open, closes, plan[8], plan_count, plan_at, received_frames;
static int64_t written_frames, read_frames;
static int16_t received[960];
static int dummy_stream, dummy_builder;
static int paused, starts;
static int drain_mode, drain_pumps;
static int64_t hardware_frames;
static char drain_report[512];

uint64_t void_monotonic_ns(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}
void void_log(const char *fmt, ...)
{
    if (strstr(fmt, "exit drain:")) {
        va_list ap; va_start(ap, fmt); vsnprintf(drain_report, sizeof drain_report, fmt, ap); va_end(ap);
    }
}
uint32_t void_tick_ms(void) { return (uint32_t)(void_monotonic_ns() / 1000000u); }
aaudio_result_t AAudio_createStreamBuilder(AAudioStreamBuilder **out) { *out = (AAudioStreamBuilder *)&dummy_builder; return AAUDIO_OK; }
void AAudioStreamBuilder_setFormat(AAudioStreamBuilder *b, aaudio_format_t x) { (void)b; assert(x == AAUDIO_FORMAT_PCM_I16); }
void AAudioStreamBuilder_setChannelCount(AAudioStreamBuilder *b, int32_t x) { (void)b; assert(x == 2); }
void AAudioStreamBuilder_setSampleRate(AAudioStreamBuilder *b, int32_t x) { (void)b; assert(x == 48000); }
void AAudioStreamBuilder_setUsage(AAudioStreamBuilder *b, aaudio_usage_t x) { (void)b; assert(x == AAUDIO_USAGE_GAME); }
void AAudioStreamBuilder_setPerformanceMode(AAudioStreamBuilder *b, aaudio_performance_mode_t x) { (void)b; (void)x; }
void AAudioStreamBuilder_setBufferCapacityInFrames(AAudioStreamBuilder *b, int32_t x) { (void)b; assert(x == 2400); }
aaudio_result_t AAudioStreamBuilder_openStream(AAudioStreamBuilder *b, AAudioStream **out)
{
    (void)b;
    if (fail_open) return AAUDIO_ERROR_UNAVAILABLE;
    *out = (AAudioStream *)&dummy_stream; started = 0; written_frames = read_frames = 0; return AAUDIO_OK;
}
aaudio_result_t AAudioStreamBuilder_delete(AAudioStreamBuilder *b) { (void)b; return AAUDIO_OK; }
int32_t AAudioStream_getFramesPerBurst(AAudioStream *s) { (void)s; return 1200; }
int32_t AAudioStream_getBufferCapacityInFrames(AAudioStream *s) { (void)s; return 2400; }
int32_t AAudioStream_getBufferSizeInFrames(AAudioStream *s) { (void)s; return 2400; }
aaudio_result_t AAudioStream_setBufferSizeInFrames(AAudioStream *s, int32_t n) { (void)s; assert(n == 2400); return n; }
int32_t AAudioStream_getSampleRate(AAudioStream *s) { (void)s; return 48000; }
aaudio_performance_mode_t AAudioStream_getPerformanceMode(AAudioStream *s) { (void)s; return AAUDIO_PERFORMANCE_MODE_NONE; }
aaudio_result_t AAudioStream_requestStart(AAudioStream *s) { (void)s; assert(paused || written_frames == 2400); read_frames = written_frames; started = 1; paused = 0; ++starts; return AAUDIO_OK; }
aaudio_result_t AAudioStream_requestPause(AAudioStream *s) { (void)s; paused = 1; return AAUDIO_OK; }
aaudio_result_t AAudioStream_waitForStateChange(AAudioStream *s, aaudio_stream_state_t before,
                                               aaudio_stream_state_t *after, int64_t timeout)
{
    (void)s; assert(before == AAUDIO_STREAM_STATE_UNKNOWN && timeout == 0);
    *after = AAUDIO_STREAM_STATE_STARTED;
    ++drain_pumps;
    if (drain_mode) {
        if (drain_mode != 5 && read_frames < written_frames) read_frames += 240;
        hardware_frames = drain_mode == 2 ? read_frames - 20898336 :
                          drain_mode == 4 ? read_frames + 20898336 : read_frames;
    }
    return AAUDIO_OK;
}
aaudio_result_t AAudioStream_getTimestamp(AAudioStream *s, clockid_t c, int64_t *p, int64_t *t)
{
    (void)s; assert(c == CLOCK_MONOTONIC);
    if (drain_mode == 3 || drain_mode == 5) return AAUDIO_ERROR_INVALID_STATE;
    *p = drain_mode ? hardware_frames : written_frames;
    *t = (int64_t)void_monotonic_ns(); return AAUDIO_OK;
}
aaudio_result_t AAudioStream_requestStop(AAudioStream *s) { (void)s; return AAUDIO_OK; }
aaudio_result_t AAudioStream_close(AAudioStream *s) { (void)s; started = 0; ++closes; return AAUDIO_OK; }
int64_t AAudioStream_getFramesWritten(AAudioStream *s) { (void)s; return written_frames; }
int64_t AAudioStream_getFramesRead(AAudioStream *s) { (void)s; return read_frames; }
int32_t AAudioStream_getXRunCount(AAudioStream *s) { (void)s; return 0; }
const char *AAudio_convertResultToText(aaudio_result_t result) { (void)result; return "mock"; }
aaudio_result_t AAudioStream_write(AAudioStream *s, const void *data, int32_t n, int64_t timeout)
{
    (void)s;
    if (!started) {
        assert(timeout == 0);
        for (int i = 0; i < n * 2; ++i) assert(!((const int16_t *)data)[i]);
        written_frames += n; return n;
    }
    assert(timeout == 100000000LL);
    int result = plan_at < plan_count ? plan[plan_at++] : n;
    if (result > 0) {
        assert(result <= n && received_frames + result <= 480);
        memcpy(received + 2 * received_frames, data, (size_t)result * 4);
        received_frames += result; written_frames += result;
    }
    return result;
}
static void set_plan(int a, int b, int c, int d, int count)
{
    if (started) read_frames = written_frames;
    plan[0] = a; plan[1] = b; plan[2] = c; plan[3] = d;
    plan_count = count; plan_at = received_frames = 0; memset(received, 0, sizeof received);
}
static void *paused_output(void *data) { void_audio_output(data, 480); return NULL; }
int main(void)
{
    int16_t input[960]; for (int i = 0; i < 960; ++i) input[i] = (int16_t)(i + 1);
    set_plan(160, 0, 320, 0, 3); void_audio_output(input, 480);
    assert(!memcmp(input, received, sizeof input) && audio_sent == 480 && !audio_dropped && audio_partial == 1 && audio_zero == 1);
    set_plan(160, AAUDIO_ERROR_DISCONNECTED, 320, 0, 3); void_audio_output(input, 480);
    assert(!memcmp(input, received, sizeof input) && audio_sent == 960 && !audio_dropped && audio_errors == 1 && audio_route_lost == 160 && closes == 1);
    set_plan(0, 0, 0, 480, 4); void_audio_output(input, 480);
    assert(!memcmp(input, received, sizeof input) && audio_sent == 1440 && !audio_dropped && audio_errors == 2 && audio_zero == 4);
    set_plan(480, 0, 0, 0, 1);
    audio_set_paused(1); uint32_t clock = void_audio_clock_ms();
    pthread_t writer; assert(!pthread_create(&writer, NULL, paused_output, input));
    usleep(30000);
    assert(paused && stream && received_frames == 0 && audio_requested == 1440);
    assert(void_audio_clock_ms() - clock <= 1);
    audio_set_paused(0); pthread_join(writer, NULL);
    assert(!memcmp(input, received, sizeof input) && audio_sent == 1920);
    audio_close(); fail_open = 1; set_plan(0, 0, 0, 0, 0); void_audio_output(input, 480);
    assert(!stream && audio_dropped == 480 && audio_requested == audio_sent + audio_dropped);
    fail_open = 0; set_plan(480, 0, 0, 0, 1); void_audio_output(input, 480);
    assert(!memcmp(input, received, sizeof input) && audio_requested == audio_sent + audio_dropped);
    void_audio_drain(); assert(!stream);
    for (drain_mode = 1; drain_mode <= 4; ++drain_mode) {
        assert(audio_open());
        written_frames = 21981312; read_frames = written_frames - 2400;
        hardware_frames = 1082976; drain_pumps = 0; drain_report[0] = 0;
        uint64_t begin = void_monotonic_ns(); void_audio_drain();
        assert(!stream && read_frames == written_frames && drain_pumps == 10);
        assert(void_monotonic_ns() - begin < 300000000u);
        assert(strstr(drain_report, "complete") && !strstr(drain_report, "timeout"));
        assert(strstr(drain_report, drain_mode == 1 ? "timestamp 1" : "timestamp 0"));
        printf("drain case%d: %s\n", drain_mode, drain_report);
    }
    drain_mode = 5; assert(audio_open());
    written_frames = 21981312; read_frames = written_frames - 2400;
    uint64_t stalled_begin = void_monotonic_ns(); void_audio_drain();
    uint64_t stalled_elapsed = void_monotonic_ns() - stalled_begin;
    assert(!stream && read_frames < written_frames && strstr(drain_report, "timeout"));
    assert(stalled_elapsed >= 490000000u && stalled_elapsed < 800000000u);
    printf("drain stalled consumer bounded: %s\n", drain_report);
    puts("audio_faults_selfcheck: exact PCM/partial/disconnect/clock pause; cached timestamps, backward/forward frame-origin mismatch and unavailable timestamp drain PASS");
    return 0;
}
