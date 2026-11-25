/* Production compatibility sink: real queue, pause, resume and tail drain. */
#define VOID_ANDROID_APP
#include "../android/host/android_main.c"
#include <assert.h>

uint64_t void_monotonic_ns(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + t.tv_nsec;
}
uint32_t void_tick_ms(void) { return (uint32_t)(void_monotonic_ns() / 1000000u); }
static char drain_result[512];
void void_log(const char *fmt, ...)
{
    va_list args; va_start(args, fmt);
    if (strstr(fmt,"exit drain")) vsnprintf(drain_result,sizeof drain_result,fmt,args);
    else vprintf(fmt,args);
    va_end(args);putchar('\n');
}
static void *paused_output(void *data) { void_audio_output(data,480);return NULL; }
int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0);
    audio_compat=1;
    int16_t pcm[960]={0};
    uint64_t start=void_monotonic_ns();
    for(int i=0;i<2000;i++) {
        audio_report_ns=void_monotonic_ns();
        if(i==1000) {
            audio_set_paused(1);
            pthread_t writer;assert(!pthread_create(&writer,NULL,paused_output,pcm));
            usleep(60000);
            uint64_t out=atomic_load(&audio_fifo_out),done=atomic_load(&audio_sl_completed),sent=audio_sent;
            uint32_t clock=void_audio_clock_ms();
            usleep(100000);
            assert(out==atomic_load(&audio_fifo_out)&&done==atomic_load(&audio_sl_completed));
            assert(clock==void_audio_clock_ms()&&sent==audio_sent);
            audio_set_paused(0);pthread_join(writer,NULL);
            assert(audio_sent==sent+480);
        } else void_audio_output(pcm,480);
    }
    assert(!stream&&audio_sl_player&&audio_sent==960000&&audio_requested==audio_sent);
    assert(!audio_errors&&!audio_dropped&&!audio_route_lost);
    void_audio_output(pcm,137);
    assert(audio_sent==960137&&atomic_load(&audio_sl_written)==audio_sent+AUDIO_SL_BUFFERS*AUDIO_WRITE_FRAMES);
    assert(atomic_load(&audio_sl_in)-atomic_load(&audio_sl_out)<=AUDIO_SL_BUFFERS);
    printf("OpenSL ES: %.3f s, %llu frames, callback silence %u, pause/resume and partial buffer PASS\n",
        (void_monotonic_ns()-start)/1e9,(unsigned long long)audio_sent,atomic_load(&audio_callback_silence));
    assert(!atomic_load(&audio_callback_silence));
    void_audio_drain();
    puts(drain_result);
    assert(!audio_sl_player&&!audio_sl_engine&&strstr(drain_result,"complete"));
    assert(atomic_load(&audio_sl_written)==atomic_load(&audio_sl_completed));
    puts("OpenSL ES real queue, pause clock, retained PCM and exit tail PASS");
    return 0;
}
