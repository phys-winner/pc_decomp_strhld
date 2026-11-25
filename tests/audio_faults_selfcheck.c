/* Deterministic callback, route recovery, pause and drain checks of the production sink. */
#define VOID_ANDROID_APP
#define dlopen mock_audio_dlopen
#define dlsym mock_audio_dlsym
#include "../android/host/android_main.c"
#undef dlopen
#undef dlsym
#include <assert.h>

static int started, fail_open, closes, received_frames;
static int64_t written_frames, read_frames;
static int16_t received[960];
static int dummy_stream, dummy_builder;
static int paused, starts;
static int burst = 256, capacity = 2048, expected_size = 512;
static aaudio_performance_mode_t performance = AAUDIO_PERFORMANCE_MODE_LOW_LATENCY;
static int drain_mode, drain_pumps;
static int64_t hardware_frames;
static char drain_report[512];
static AAudioStream_dataCallback registered_callback;
static AAudioStream_errorCallback registered_error;
static int xruns, buffer_size;
static int16_t started_pcm[960];

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
static void mock_audio_set_usage(AAudioStreamBuilder *b, aaudio_usage_t x) { (void)b; assert(x == AAUDIO_USAGE_GAME); }
void *mock_audio_dlopen(const char *path, int flags)
{
    assert(!strcmp(path, "libaaudio.so") && flags == RTLD_NOW);
    return &dummy_builder;
}
void *mock_audio_dlsym(void *library, const char *name)
{
    assert(library == &dummy_builder && !strcmp(name, "AAudioStreamBuilder_setUsage"));
    return (void *)mock_audio_set_usage;
}
void AAudioStreamBuilder_setPerformanceMode(AAudioStreamBuilder *b, aaudio_performance_mode_t x) { (void)b; (void)x; }
void AAudioStreamBuilder_setDataCallback(AAudioStreamBuilder *b, AAudioStream_dataCallback cb, void *user)
{ (void)b; assert(!user); registered_callback = cb; }
void AAudioStreamBuilder_setErrorCallback(AAudioStreamBuilder *b, AAudioStream_errorCallback cb, void *user)
{ (void)b; assert(!user); registered_error = cb; }
void AAudioStreamBuilder_setFramesPerDataCallback(AAudioStreamBuilder *b, int32_t n)
{ (void)b; assert(n == 480); }
aaudio_result_t AAudioStreamBuilder_openStream(AAudioStreamBuilder *b, AAudioStream **out)
{
    (void)b;
    if (fail_open) return AAUDIO_ERROR_UNAVAILABLE;
    *out = (AAudioStream *)&dummy_stream; started = paused = 0; written_frames = read_frames = 0; return AAUDIO_OK;
}
aaudio_result_t AAudioStreamBuilder_delete(AAudioStreamBuilder *b) { (void)b; return AAUDIO_OK; }
int32_t AAudioStream_getFramesPerBurst(AAudioStream *s) { (void)s; return burst; }
int32_t AAudioStream_getBufferCapacityInFrames(AAudioStream *s) { (void)s; return capacity; }
int32_t AAudioStream_getBufferSizeInFrames(AAudioStream *s) { (void)s; return buffer_size; }
aaudio_result_t AAudioStream_setBufferSizeInFrames(AAudioStream *s, int32_t n)
{ (void)s; buffer_size = n < capacity ? n : capacity; return buffer_size; }
int32_t AAudioStream_getSampleRate(AAudioStream *s) { (void)s; return 48000; }
aaudio_performance_mode_t AAudioStream_getPerformanceMode(AAudioStream *s) { (void)s; return performance; }
aaudio_result_t AAudioStream_requestStart(AAudioStream *s)
{
    assert(registered_callback && registered_error);
    if (!paused) {
        assert(buffer_size == expected_size);
        registered_callback(s, NULL, started_pcm, 480);
        written_frames += 480;
        if (atomic_load(&audio_fifo_in) != atomic_load(&audio_fifo_out)) {
            registered_callback(s, NULL, started_pcm, 480);
            written_frames += 480;
        }
    }
    read_frames = written_frames; started = 1; paused = 0; ++starts; return AAUDIO_OK;
}
aaudio_result_t AAudioStream_requestPause(AAudioStream *s) { (void)s; paused = 1; return AAUDIO_OK; }
aaudio_result_t AAudioStream_waitForStateChange(AAudioStream *s, aaudio_stream_state_t before,
                                               aaudio_stream_state_t *after, int64_t timeout)
{
    (void)s; assert(before == AAUDIO_STREAM_STATE_UNKNOWN && timeout == 0);
    *after = AAUDIO_STREAM_STATE_STARTED;
    ++drain_pumps;
    if (drain_mode) {
        if (atomic_load(&audio_callback_tail) > written_frames) written_frames = atomic_load(&audio_callback_tail);
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
int32_t AAudioStream_getXRunCount(AAudioStream *s) { (void)s; return xruns; }
const char *AAudio_convertResultToText(aaudio_result_t result) { (void)result; return "mock"; }
static void consume(int n)
{
    assert(started && !paused && received_frames + n <= 480);
    registered_callback(stream, NULL, received + 2 * received_frames, n);
    received_frames += n; written_frames += n; read_frames = written_frames;
}
static void *paused_output(void *data) { void_audio_output(data, 480); return NULL; }
static SLresult failed_sl_enqueue(SLAndroidSimpleBufferQueueItf queue, const void *pcm, SLuint32 size)
{ (void)queue; (void)pcm; assert(size == 1920); return SL_RESULT_RESOURCE_ERROR; }
int main(void)
{
    int16_t input[960]; for (int i = 0; i < 960; ++i) input[i] = (int16_t)(i + 1);
    /* Different callback lengths repeatedly cross the native ring boundary. */
    for (unsigned i = 0; i < 20; ++i) {
        received_frames = 0;
        void_audio_output(input, 480);
        assert(atomic_load(&audio_fifo_in) - atomic_load(&audio_fifo_out) == 480);
        consume(160); consume(320);
        assert(!memcmp(input, received, sizeof input) && !audio_dropped);
    }
    assert(!atomic_load(&audio_callback_silence));
    /* A callback timestamp newer than the clock snapshot is not a stalled stream. */
    atomic_store(&audio_callback_ns, void_monotonic_ns() + 1000000000u);
    void_audio_wait_ready();
    assert(stream && !closes && !audio_errors);
    atomic_store(&audio_callback_ns, void_monotonic_ns());
    /* A 20 ms writer hold still has two complete blocks ready for the device. */
    void_audio_output(input, 480); void_audio_output(input, 480);
    assert(atomic_load(&audio_fifo_in) - atomic_load(&audio_fifo_out) == 960);
    received_frames = 0; consume(480); assert(!memcmp(input, received, sizeof input));
    received_frames = 0; consume(480); assert(!memcmp(input, received, sizeof input));
    assert(!atomic_load(&audio_callback_silence));
    received_frames = 0; consume(160);
    for (int i = 0; i < 320; ++i) assert(!received[i]);
    assert(atomic_exchange(&audio_callback_silence, 0) == 160);
    /* The error thread only records failure. The writer closes and reopens. */
    void_audio_output(input, 480);
    registered_error(stream, NULL, AAUDIO_ERROR_DISCONNECTED);
    assert(stream && !closes && atomic_load(&audio_callback_error));
    void_audio_wait_ready();
    assert(stream && closes == 1 && audio_errors == 1 && !audio_route_lost);
    assert(!memcmp(input, started_pcm, sizeof input));
    assert(atomic_load(&audio_fifo_in) == atomic_load(&audio_fifo_out));
    /* An absent callback is bounded by the watchdog, rather than hanging the writer. */
    void_audio_output(input, 480);
    audio_start_ns = void_monotonic_ns() - 3000000000u;
    atomic_store(&audio_callback_ns, void_monotonic_ns() - 600000000u);
    void_audio_wait_ready();
    assert(stream && closes == 2 && audio_errors == 2);
    assert(!memcmp(input, started_pcm, sizeof input));
    uint64_t before = audio_sent;
    audio_set_paused(1); uint32_t clock = void_audio_clock_ms();
    pthread_t writer; assert(!pthread_create(&writer, NULL, paused_output, input));
    usleep(30000);
    assert(paused && stream && audio_sent == before);
    for (unsigned i = 0; i < 100; ++i) { assert(void_audio_clock_ms() == clock); usleep(100); }
    audio_set_paused(0); pthread_join(writer, NULL);
    received_frames = 0; consume(480);
    assert(!memcmp(input, received, sizeof input) && audio_sent == before + 480);
    /* Old MMAP clocks may run beyond submitted PCM even in PAUSED. */
    void_audio_output(input, 480);
    audio_set_paused(1);
    assert(!pthread_create(&writer, NULL, paused_output, input));
    usleep(30000);
    int before_closes = closes;
    read_frames = written_frames + 2400;
    audio_set_paused(0); pthread_join(writer, NULL);
    assert(closes == before_closes + 1 && !memcmp(input, started_pcm, sizeof input));
    received_frames = 0; consume(480); assert(!memcmp(input, received, sizeof input));
    /* Startup has a separate deadline; a slow first callback still stays bounded. */
    assert(atomic_load(&audio_callback_tail));
    atomic_store(&audio_callback_ns, void_monotonic_ns() - 600000000u);
    void_audio_wait_ready(); assert(closes == before_closes + 1);
    atomic_store(&audio_callback_ns, void_monotonic_ns() - 2100000000u);
    audio_start_ns = void_monotonic_ns() - 2100000000u;
    void_audio_wait_ready(); assert(closes == before_closes + 2);
    assert(atomic_load(&audio_callback_tail));
    audio_close(); fail_open = 1; void_audio_output(input, 480);
    assert(!stream && audio_dropped == 480 && audio_requested == audio_sent + audio_dropped);
    fail_open = 0; void_audio_output(input, 480); received_frames = 0; consume(480);
    assert(!memcmp(input, received, sizeof input) && audio_requested == audio_sent + audio_dropped);
    void_audio_drain(); assert(!stream);
    for (drain_mode = 1; drain_mode <= 4; ++drain_mode) {
        assert(audio_open());
        written_frames = 21981312; read_frames = written_frames - 2400;
        atomic_store(&audio_callback_tail, written_frames + 480);
        hardware_frames = 1082976; drain_pumps = 0; drain_report[0] = 0;
        uint64_t begin = void_monotonic_ns(); void_audio_drain();
        assert(!stream && read_frames == written_frames && drain_pumps == 12);
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
    performance = AAUDIO_PERFORMANCE_MODE_LOW_LATENCY;
    burst = 96; capacity = 5760; expected_size = 192;
    assert(audio_open() && buffer_size == 192);
    xruns = 1; void_audio_output(input, 480); received_frames = 0; consume(480);
    assert(buffer_size == 288);
    void_audio_output(input, 480); received_frames = 0; consume(480);
    assert(buffer_size == 288);
    audio_close();
    burst = 192; expected_size = 384;
    assert(audio_open() && buffer_size == 384);
    audio_close();
    burst = 128; expected_size = 256;
    assert(audio_open() && buffer_size == 256);
    audio_close();
    capacity = expected_size = 128;
    assert(audio_open() && buffer_size == 128);
    audio_close();
    /* A normal route can batch several callbacks; retain one real burst, not just 10 ms. */
    performance = AAUDIO_PERFORMANCE_MODE_NONE;
    burst = 1200; capacity = expected_size = 2400;
    assert(audio_open() && audio_fifo_target == 1920);
    for (int i = 0; i < 2; ++i) { received_frames = 0; consume(480); }
    for (int i = 0; i < 4; ++i) void_audio_output(input, 480);
    assert(atomic_load(&audio_fifo_in) - atomic_load(&audio_fifo_out) == 1920);
    for (int i = 0; i < 4; ++i) {
        received_frames = 0; consume(480); assert(!memcmp(input, received, sizeof input));
    }
    audio_close(); burst = 960; capacity = 1924; expected_size = 1920;
    assert(audio_open() && audio_fifo_target == 1440);
    received_frames = 0; consume(480);
    ++xruns; void_audio_output(input, 480);
    assert(buffer_size == 1920); /* Capacity padding is not a whole extra burst. */
    received_frames = 0; consume(480); assert(!memcmp(input, received, sizeof input));
    void_audio_output(input, 480);
    registered_error(stream, NULL, AAUDIO_ERROR_DISCONNECTED);
    void_audio_drain();
    assert(!stream && strstr(drain_report, "pending 480") && strstr(drain_report, "timeout"));
    /* A rejected OpenSL buffer never advances the submitted frame/slot counters. */
    const struct SLAndroidSimpleBufferQueueItf_ failed_sl = {.Enqueue = failed_sl_enqueue};
    const struct SLAndroidSimpleBufferQueueItf_ *failed_sl_interface = &failed_sl;
    audio_sl_queue = &failed_sl_interface;
    int64_t written = atomic_load(&audio_sl_written);
    uint64_t slot = atomic_load(&audio_sl_in);
    assert(audio_sl_enqueue(input, 480) == SL_RESULT_RESOURCE_ERROR);
    assert(atomic_load(&audio_sl_in) == slot && atomic_load(&audio_sl_written) == written);
    audio_sl_queue = NULL;
    puts("OpenSL enqueue failure never claims submitted PCM PASS");
    puts("callback: PCM order, wrap, silence, disconnect, watchdog, pause, bounded queue and xrun buffer growth PASS");
    puts("drain: cached timestamps, backward/forward frame-origin mismatch and unavailable timestamp PASS");
    return 0;
}
