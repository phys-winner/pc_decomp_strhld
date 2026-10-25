/* Android application glue: JNI from GameActivity, the native window, the
 * game thread, touch input in DirectInput scan codes, and AAudio output for
 * the DirectSound mixer. */
#ifdef VOID_ANDROID_APP
#define _GNU_SOURCE
#include "host.h"
#include "input.h"
#include "display.h"
#include "touch_api.h"
#include "game_ui.h"
#include "feedback.h"
#include <aaudio/AAudio.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <jni.h>
#include <math.h>
#include <time.h>
#include <unistd.h>

int void_game_main(const char *game, const char *data, const char *images, int selftest);
void void_process_exit(uint32_t code, int run_detach);

static ANativeWindow *window;
static pthread_mutex_t window_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t window_cond = PTHREAD_COND_INITIALIZER;
static pthread_t game_thread;
static int game_started;
static char game_dir[512], data_dir[512], images_dir[512];
static char launch_marker[512];
static JavaVM *jvm;
static jobject activity;

/* The activity's surface comes and goes (Home, the app switcher, the screen
 * turning off): window_serial counts the changes, and the renderer takes a
 * new one up at its next present (d3d9_gl.c), acknowledging the serial it
 * has switched to; a destroyed surface waits (briefly) for that. */
static uint32_t window_serial, window_acked;
static _Atomic int audio_paused;
static void audio_set_paused(int paused);

/* d3d9_gl.c asks for the window when the game creates its device; wait for
 * the surface if the activity has not delivered it yet. */
void *void_android_window(void)
{
    pthread_mutex_lock(&window_lock);
    while (!window) pthread_cond_wait(&window_cond, &window_lock);
    ANativeWindow *w = window;
    pthread_mutex_unlock(&window_lock);
    return w;
}

/* The current window (NULL while the activity has none) and its serial; with
 * wait set, block until there is one. */
void *void_android_window_current(uint32_t *serial, int wait)
{
    pthread_mutex_lock(&window_lock);
    while (wait && !window) pthread_cond_wait(&window_cond, &window_lock);
    ANativeWindow *w = window;
    *serial = window_serial;
    pthread_mutex_unlock(&window_lock);
    return w;
}

void void_android_window_ack(uint32_t serial)
{
    pthread_mutex_lock(&window_lock);
    window_acked = serial;
    pthread_cond_broadcast(&window_cond);
    pthread_mutex_unlock(&window_lock);
}

static void *game_main(void *arg)
{
    (void)arg;
    int code = void_game_main(game_dir, data_dir, images_dir, 0);
    void_log("[android] game thread finished with %d", code);
    void_process_exit((uint32_t)code, 0);
    return NULL;
}

JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *reserved)
{
    (void)reserved;
    jvm = vm;
    return JNI_VERSION_1_6;
}

void void_android_process_exit(uint32_t code)
{
    if (!code && launch_marker[0]) unlink(launch_marker);
}

JNIEXPORT jboolean JNICALL Java_org_stronghold_port_GameActivity_nativeStart(JNIEnv *env, jobject self, jstring game,
    jstring data, jstring images, jstring marker, jint screen_w, jint screen_h, jboolean verbose, jboolean skip_movies)
{
    void void_android_gdi_init(JNIEnv *env);
    void_android_gdi_init(env);
    if (game_started) return JNI_TRUE;
    const char *g = (*env)->GetStringUTFChars(env, game, NULL);
    const char *d = (*env)->GetStringUTFChars(env, data, NULL);
    const char *i = (*env)->GetStringUTFChars(env, images, NULL);
    const char *m = (*env)->GetStringUTFChars(env, marker, NULL);
    snprintf(game_dir, sizeof game_dir, "%s", g);
    snprintf(data_dir, sizeof data_dir, "%s", d);
    snprintf(images_dir, sizeof images_dir, "%s", i);
    snprintf(launch_marker, sizeof launch_marker, "%s", m);
    (*env)->ReleaseStringUTFChars(env, game, g);
    (*env)->ReleaseStringUTFChars(env, data, d);
    (*env)->ReleaseStringUTFChars(env, images, i);
    (*env)->ReleaseStringUTFChars(env, marker, m);
    activity = (*env)->NewGlobalRef(env, self);
    void_verbose = verbose ? 1 : 0;
    void void_files_skip_movies(int enabled);
    void_files_skip_movies(skip_movies);
    stronghold_game_ui_skip(skip_movies);
    stronghold_display_configure(screen_w, screen_h);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64u << 20);
    game_started = pthread_create(&game_thread, &attr, game_main, NULL) == 0;
    pthread_attr_destroy(&attr);
    return game_started ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_org_stronghold_port_GameActivity_nativeSurface(JNIEnv *env, jobject self, jobject surface)
{
    (void)self;
    if (!surface) stronghold_touch_abort();
    pthread_mutex_lock(&window_lock);
    ANativeWindow *old = window;
    window = surface ? ANativeWindow_fromSurface(env, surface) : NULL;
    uint32_t serial = ++window_serial;
    pthread_cond_broadcast(&window_cond);
    if (!surface && old) {
        /* Android wants the EGL surface gone before surfaceDestroyed returns;
         * the game thread lets it go at its next present. A game busy
         * loading may not present for a while: do not hold the UI thread
         * long (EGL keeps its own reference to the window meanwhile). */
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_sec += 1;
        while (window_acked != serial && pthread_cond_timedwait(&window_cond, &window_lock, &until) == 0) {}
    }
    pthread_mutex_unlock(&window_lock);
    if (old) ANativeWindow_release(old);
}

JNIEXPORT jint JNICALL Java_org_stronghold_port_GameActivity_nativeUiEvent(JNIEnv *env,jclass self)
{ (void)env;(void)self;return stronghold_game_ui_event(); }
JNIEXPORT void JNICALL Java_org_stronghold_port_GameActivity_nativeEditorRequest(JNIEnv *env,jclass self)
{ (void)env;(void)self;stronghold_game_editor_request(); }
JNIEXPORT jintArray JNICALL Java_org_stronghold_port_GameActivity_nativeSettings(JNIEnv *env,jclass self)
{
    (void)self;int values[SH_GAME_SETTINGS];
    if(!stronghold_game_settings_get(values))return NULL;
    jintArray result=(*env)->NewIntArray(env,SH_GAME_SETTINGS);
    if(result)(*env)->SetIntArrayRegion(env,result,0,SH_GAME_SETTINGS,values);return result;
}
JNIEXPORT void JNICALL Java_org_stronghold_port_GameActivity_nativeSetting(JNIEnv *env,jclass self,jint index,jint value)
{ (void)env;(void)self;stronghold_game_setting_set(index,value); }
JNIEXPORT jobjectArray JNICALL Java_org_stronghold_port_GameActivity_nativeEditor(JNIEnv *env,jclass self)
{
    (void)self;uint16_t text[250];int limit,bank;
    int token=stronghold_game_editor_get(text,&limit,&bank),length=0;
    if(!token)return NULL;
    while(length<249&&text[length])length++;
    jclass strings=(*env)->FindClass(env,"java/lang/String");
    jobjectArray result=(*env)->NewObjectArray(env,4,strings,NULL);
    if(!result)return NULL;
    jstring value=(*env)->NewString(env,text,length);(*env)->SetObjectArrayElement(env,result,0,value);(*env)->DeleteLocalRef(env,value);
    int numbers[]={token,limit,bank};
    for(int i=0;i<3;i++){char number[16];snprintf(number,sizeof number,"%d",numbers[i]);
        value=(*env)->NewStringUTF(env,number);(*env)->SetObjectArrayElement(env,result,i+1,value);(*env)->DeleteLocalRef(env,value);}
    return result;
}
JNIEXPORT jboolean JNICALL Java_org_stronghold_port_GameActivity_nativeEdit(JNIEnv *env,jclass self,jstring text,jint token)
{
    (void)self;int length=(*env)->GetStringLength(env,text);
    const jchar *chars=(*env)->GetStringChars(env,text,NULL);if(!chars)return JNI_FALSE;
    int ok=stronghold_game_editor_set(chars,length,token);
    (*env)->ReleaseStringChars(env,text,chars);return ok?JNI_TRUE:JNI_FALSE;
}

/* onPause/onResume: the game itself waits in its present while it has no
 * surface; the audio stream pauses with the activity. */
JNIEXPORT void JNICALL Java_org_stronghold_port_GameActivity_nativePause(JNIEnv *env, jobject self, jboolean paused)
{
    (void)env; (void)self;
    audio_set_paused(paused ? 1 : 0);
    if (paused) stronghold_touch_abort();
}

JNIEXPORT void JNICALL Java_org_stronghold_port_GameActivity_nativeTouch(JNIEnv *env, jobject self,
    jint action, jint sequence, jfloat x, jfloat y, jfloat dx, jfloat dy, jfloat scale)
{
    (void)env; (void)self;
    if (!game_started || audio_paused) return;
    stronghold_touch_enqueue(action, (uint32_t)sequence, x, y, dx, dy, scale);
}

JNIEXPORT jint JNICALL Java_org_stronghold_port_GameActivity_nativeTouchStatus(JNIEnv *env, jobject self,
    jint sequence)
{
    (void)env; (void)self;
    return game_started && !audio_paused ? stronghold_touch_status((uint32_t)sequence) : SH_TOUCH_BLOCKED;
}

JNIEXPORT void JNICALL Java_org_stronghold_port_GameActivity_nativeRenderScale(JNIEnv *env, jobject self, jfloat scale)
{
    (void)env; (void)self;
    stronghold_display_set_render_scale(scale);
}

JNIEXPORT void JNICALL Java_org_stronghold_port_GameActivity_nativeSmoothCamera(JNIEnv *env, jclass self, jboolean enabled)
{
    (void)env; (void)self;
    stronghold_display_smooth_camera(enabled ? 1 : 0);
}

JNIEXPORT void JNICALL Java_org_stronghold_port_GameActivity_nativePowersScale(JNIEnv *env, jclass self, jfloat scale)
{
    (void)env; (void)self; stronghold_display_powers_scale(scale);
}
JNIEXPORT void JNICALL Java_org_stronghold_port_GameActivity_nativeCutout(JNIEnv *env, jclass self, jfloat left)
{
    (void)env; (void)self; stronghold_display_cutout(left);
}

/* World and UI layers: SH_FILTER_* each, shared sharpness, deband, crisp text. */
JNIEXPORT void JNICALL Java_org_stronghold_port_GameActivity_nativeFilters(JNIEnv *env, jclass self, jint world, jint ui,
    jfloat sharpness, jboolean deband, jboolean crisp_text)
{
    (void)env; (void)self;
    stronghold_display_set_filters(world, ui, sharpness, deband ? 1 : 0, crisp_text ? 1 : 0);
}

/* The placement loupe follows an aiming finger; coordinates are view fractions. */
JNIEXPORT void JNICALL Java_org_stronghold_port_GameActivity_nativeMagnifier(JNIEnv *env, jclass self, jboolean active,
    jfloat x, jfloat y, jint size, jint zoom)
{
    (void)env; (void)self;
    stronghold_present_loupe_settings(size, zoom);
    stronghold_display_magnifier(active ? 1 : 0, x, y);
}

/* Haptic cues raised by the engine since the last call (SH_FEEDBACK_*). */
JNIEXPORT jint JNICALL Java_org_stronghold_port_GameActivity_nativeFeedback(JNIEnv *env, jclass self)
{
    (void)env; (void)self;
    return stronghold_feedback_take();
}

/* Recent frame costs (mean and p95 busy ms, fps, frames), NULL until enough
 * frames exist; reset starts the next sample from an empty window. */
JNIEXPORT jfloatArray JNICALL Java_org_stronghold_port_GameActivity_nativePerfStats(JNIEnv *env, jclass self, jboolean reset)
{
    (void)self;
    float stats[4];
    int ok = stronghold_perf_stats(stats);
    if (reset) stronghold_perf_reset();
    if (!ok) return NULL;
    jfloatArray result = (*env)->NewFloatArray(env, 4);
    if (result) (*env)->SetFloatArrayRegion(env, result, 0, 4, stats);
    return result;
}

/* Mouse button edges wait for this: MODE 0 reached the game thread. */
JNIEXPORT jboolean JNICALL Java_org_stronghold_port_GameActivity_nativeTouchDesktop(JNIEnv *env, jclass self)
{
    (void)env; (void)self;
    return stronghold_touch_desktop() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jfloat JNICALL Java_org_stronghold_port_GameActivity_nativeWorldZoom(JNIEnv *env, jobject self)
{
    (void)env; (void)self;
    return stronghold_display_world_zoom();
}

JNIEXPORT jfloatArray JNICALL Java_org_stronghold_port_GameActivity_nativeHudPoint(JNIEnv *env, jclass self, jfloat x, jfloat y)
{
    (void)self;
    float point[2], scale;
    if (!game_started || !stronghold_display_layer_view(0, x, y, &point[0], &point[1], &scale)) return NULL;
    jfloatArray result = (*env)->NewFloatArray(env, 2);
    if (result) (*env)->SetFloatArrayRegion(env, result, 0, 2, point);
    return result;
}

JNIEXPORT jintArray JNICALL Java_org_stronghold_port_GameActivity_nativeControlState(JNIEnv *env,jclass self)
{
    (void)self;int values[SH_CONTROL_STATE];stronghold_game_control_state(values);
    jintArray result=(*env)->NewIntArray(env,SH_CONTROL_STATE);
    if(result)(*env)->SetIntArrayRegion(env,result,0,SH_CONTROL_STATE,values);
    return result;
}
JNIEXPORT void JNICALL Java_org_stronghold_port_GameActivity_nativeCommand(JNIEnv *env,jclass self,jint key,jint modifiers)
{ (void)env;(void)self;if(game_started&&!audio_paused)stronghold_game_command(key,modifiers); }

/* Scan code (DIK) based input from the touch layer. */
static uint32_t dik_to_vk(uint32_t dik)
{
    for (uint32_t vk = 1; vk < 256; ++vk) if (void_vk_to_dik(vk) == dik) return vk;
    return 0;
}

JNIEXPORT void JNICALL Java_org_stronghold_port_GameActivity_nativeKey(JNIEnv *env, jobject self, jint scan, jboolean down)
{
    (void)env; (void)self;
    uint32_t vk = dik_to_vk((uint32_t)scan);
    if (vk) void_input_key(vk, down);
}

JNIEXPORT jboolean JNICALL Java_org_stronghold_port_GameActivity_nativeText(JNIEnv *env, jobject self, jstring value)
{
    (void)self;
    if (!value || !game_started || audio_paused) return JNI_FALSE;
    jsize length = (*env)->GetStringLength(env, value);
    if ((uint32_t)length > VOID_INPUT_TEXT_MAX) return JNI_FALSE;
    const jchar *text = (*env)->GetStringChars(env, value, NULL);
    if (!text) return JNI_FALSE;
    int accepted = void_input_text((const uint16_t *)text, (uint32_t)length);
    (*env)->ReleaseStringChars(env, value, text);
    return accepted ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_org_stronghold_port_GameActivity_nativeMouseButton(JNIEnv *env, jobject self, jint button, jboolean down)
{
    (void)env; (void)self;
    void_input_mouse_button(button, down);
}

/* A point of the view in [0,1] as game pixels, undoing the present's
 * aspect-preserving fit (d3d9_gl.c). */
static void view_to_game(float x, float y, int *gx, int *gy)
{
    if (stronghold_display_map_point(x, y, gx, gy)) return;
    int w, h;
    void_input_screen_size(&w, &h);
    float sw = window ? (float)ANativeWindow_getWidth(window) : (float)w;
    float sh = window ? (float)ANativeWindow_getHeight(window) : (float)h;
    float scale = sw / (float)w < sh / (float)h ? sw / (float)w : sh / (float)h;
    float dx = (sw - (float)w * scale) / 2.0f, dy = (sh - (float)h * scale) / 2.0f;
    *gx = (int)((x * sw - dx) / scale);
    *gy = (int)((y * sh - dy) / scale);
}

/* The frame edges a view point touches, before any world zoom or HUD scale:
 * the mapped cursor of a zoomed world never reaches the frame edge, but the
 * original's edge scrolling (input poll 4682B0/4684B0) should follow the
 * physical screen edge. Letterbox counts as beyond the edge. */
static void pointer_edges(float x, float y)
{
    int w, h;
    void_input_screen_size(&w, &h);
    float sw = window ? (float)ANativeWindow_getWidth(window) : (float)w;
    float sh = window ? (float)ANativeWindow_getHeight(window) : (float)h;
    if (w <= 0 || h <= 0 || sw <= 0 || sh <= 0 || !isfinite(x) || !isfinite(y)) return;
    float scale = sw / (float)w < sh / (float)h ? sw / (float)w : sh / (float)h;
    float fx = (x * sw - (sw - (float)w * scale) / 2.0f) / scale, fy = (y * sh - (sh - (float)h * scale) / 2.0f) / scale;
    void_input_set_edges((fx >= w - 1 ? 1 : 0) | (fx < 1 ? 2 : 0) | (fy >= h - 1 ? 4 : 0) | (fy < 1 ? 8 : 0));
}

/* Edge scrolling for a pointer the view no longer sees (it left over a system
 * edge-gesture window), or 0 to stop it while Android controls take over. */
JNIEXPORT void JNICALL Java_org_stronghold_port_GameActivity_nativeMouseEdges(JNIEnv *env, jclass self, jint edges)
{
    (void)env; (void)self;
    void_input_set_edges(edges);
}

/* Relative motion in game pixels, or an absolute position in [0,1]. */
JNIEXPORT void JNICALL Java_org_stronghold_port_GameActivity_nativeMouseMove(JNIEnv *env, jobject self, jfloat x, jfloat y, jboolean relative)
{
    (void)env; (void)self;
    if (relative) {
        void_input_mouse_move((int)x, (int)y);
    } else {
        int nx, ny;
        uint32_t cx, cy;
        view_to_game(x, y, &nx, &ny);
        void_input_cursor(&cx, &cy);
        void_input_mouse_move(nx - (int)cx, ny - (int)cy);
        pointer_edges(x, y);
    }
}

JNIEXPORT void JNICALL Java_org_stronghold_port_GameActivity_nativeMouseWheel(JNIEnv *env, jobject self, jint delta)
{
    (void)env; (void)self;
    void_input_mouse_wheel(delta);
}

/* Put the cursor at a point of the view in [0,1] without mouse motion (a
 * paint stroke starts under the finger instead of turning the camera). */
JNIEXPORT void JNICALL Java_org_stronghold_port_GameActivity_nativeMouseWarp(JNIEnv *env, jobject self, jfloat x, jfloat y)
{
    (void)env; (void)self;
    int nx, ny;
    view_to_game(x, y, &nx, &ny);
    void_input_mouse_warp(nx, ny);
    pointer_edges(x, y);
}

/* ------------------------------------------------------------- audio */
static AAudioStream *stream;
static pthread_mutex_t audio_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t audio_requested, audio_sent, audio_dropped, audio_route_lost;
static uint32_t audio_partial, audio_zero, audio_errors, audio_reopens;
static uint64_t audio_report_ns, audio_last_ns, audio_max_gap_ns, audio_max_write_ns;
static pthread_mutex_t audio_pause_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t audio_pause_start_ns, audio_pause_total_ns;
static _Atomic int audio_shutting_down;

static void audio_set_paused(int paused)
{
    pthread_mutex_lock(&audio_pause_lock);
    uint64_t now = void_monotonic_ns();
    if (paused && !audio_paused) audio_pause_start_ns = now;
    if (!paused && audio_paused) audio_pause_total_ns += now - audio_pause_start_ns;
    audio_paused = paused;
    pthread_mutex_unlock(&audio_pause_lock);
}

int void_audio_is_paused(void) { return audio_paused; }

/* Miles' refill timeout uses timeGetTime: background time must not discard
 * already mixed speech when its timer resumes against an unchanged cursor. */
uint32_t void_audio_clock_ms(void)
{
    pthread_mutex_lock(&audio_pause_lock);
    uint64_t stopped = audio_pause_total_ns;
    if (audio_paused) stopped += void_monotonic_ns() - audio_pause_start_ns;
    uint32_t value = void_tick_ms() - (uint32_t)(stopped / 1000000u);
    pthread_mutex_unlock(&audio_pause_lock);
    return value;
}

static void audio_close(void)
{
    if (!stream) return;
    AAudioStream_requestStop(stream);
    AAudioStream_close(stream);
    stream = NULL;
}

static int audio_open(void)
{
    AAudioStreamBuilder *b = NULL;
    aaudio_result_t result = AAudio_createStreamBuilder(&b);
    if (result != AAUDIO_OK) return 0;
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(b, 2);
    AAudioStreamBuilder_setSampleRate(b, 48000);
    AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_GAME);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    /* File lookup no longer blocks Miles' timer. Two 25-ms legacy bursts
     * keep device-clock pacing without the former 100-ms output queue. */
    AAudioStreamBuilder_setBufferCapacityInFrames(b, 2400);
    result = AAudioStreamBuilder_openStream(b, &stream);
    AAudioStreamBuilder_delete(b);
    if (result != AAUDIO_OK) { stream = NULL; return 0; }
    int32_t burst = AAudioStream_getFramesPerBurst(stream);
    int32_t capacity = AAudioStream_getBufferCapacityInFrames(stream);
    int32_t target = burst > 0 ? 2 * burst : 2400;
    if (target > capacity) target = capacity;
    int32_t size = AAudioStream_setBufferSizeInFrames(stream, target);
    if (size < 0) size = AAudioStream_getBufferSizeInFrames(stream);
    /* Prime before starting. The old code started an empty stream. */
    int16_t silence[960] = {0};
    int32_t primed = 0;
    while (primed < size) {
        int32_t n = size - primed < 480 ? size - primed : 480;
        int32_t wrote = AAudioStream_write(stream, silence, n, 0);
        if (wrote <= 0) break;
        primed += wrote;
    }
    result = AAudioStream_requestStart(stream);
    if (result != AAUDIO_OK) { audio_close(); return 0; }
    ++audio_reopens;
    void_log("[audio] opened: rate %d, burst %d, buffer %d/%d, primed %d, performance %d",
        AAudioStream_getSampleRate(stream), burst, size, capacity, primed,
        AAudioStream_getPerformanceMode(stream));
    return 1;
}

void void_audio_wait_ready(void)
{
    if (!audio_paused) return;
    pthread_mutex_lock(&audio_lock);
    if (stream) {
        aaudio_result_t result = AAudioStream_requestPause(stream);
        if (result != AAUDIO_OK) {
            void_log("[audio] pause failed %d; route will reopen", result);
            int64_t queued = AAudioStream_getFramesWritten(stream) - AAudioStream_getFramesRead(stream);
            if (queued > 0) audio_route_lost += (uint64_t)queued;
            audio_close();
        }
    }
    pthread_mutex_unlock(&audio_lock);
    while (audio_paused && !audio_shutting_down) usleep(10000);
    pthread_mutex_lock(&audio_lock);
    if (stream && AAudioStream_requestStart(stream) != AAUDIO_OK) audio_close();
    audio_last_ns = 0;
    pthread_mutex_unlock(&audio_lock);
}

void void_audio_begin_shutdown(void) { audio_shutting_down = 1; }

void void_audio_drain(void)
{
    pthread_mutex_lock(&audio_lock);
    if (stream) {
        int64_t target = AAudioStream_getFramesWritten(stream), position = 0, stamp = 0;
        int64_t consumed = 0, hardware = -1;
        int32_t capacity = AAudioStream_getBufferCapacityInFrames(stream);
        int32_t burst = AAudioStream_getFramesPerBurst(stream), rate = AAudioStream_getSampleRate(stream);
        uint64_t start = void_monotonic_ns();
        int timestamp = 0;
        do {
            /* MMAP timestamps arrive through the service message queue. The
             * getter alone returns its cached value once writes have stopped. */
            aaudio_stream_state_t state;
            AAudioStream_waitForStateChange(stream, AAUDIO_STREAM_STATE_UNKNOWN, &state, 0);
            consumed = AAudioStream_getFramesRead(stream);
            timestamp = AAudioStream_getTimestamp(stream, CLOCK_MONOTONIC, &hardware, &stamp) == AAUDIO_OK;
            /* Some pause/start paths expose a stale hardware frame origin.
             * The read/write counters retain the stream's cumulative origin. */
            if (timestamp && capacity > 0 &&
                (hardware < consumed - capacity || hardware > consumed + capacity)) timestamp = 0;
            position = timestamp ? hardware : consumed;
            if (position >= target) break;
            usleep(2000);
        } while (void_monotonic_ns() - start < 500000000u);
        /* The counter fallback confirms consumption, not a DAC timestamp.
         * Retain one actual device burst before the platform's draining stop. */
        if (!timestamp && position >= target && burst > 0 && rate > 0)
            usleep((useconds_t)(((uint64_t)burst * 1000000u + rate - 1) / rate));
        void_log("[audio] exit drain: written %lld presented %lld timestamp %d, %.1f ms (read %lld hw %lld, %s)",
            (long long)target, (long long)position, timestamp, (void_monotonic_ns() - start) / 1e6,
            (long long)consumed, (long long)hardware, position >= target ? "complete" : "timeout");
        audio_close();
    }
    pthread_mutex_unlock(&audio_lock);
}

void void_audio_output(const int16_t *frames, uint32_t count)
{
    void_audio_wait_ready();
    pthread_mutex_lock(&audio_lock);
    uint64_t now = void_monotonic_ns();
    if (!audio_report_ns) audio_report_ns = now;
    if (audio_last_ns && now - audio_last_ns > audio_max_gap_ns) audio_max_gap_ns = now - audio_last_ns;
    audio_last_ns = now;
    audio_requested += count;
    uint32_t sent = 0, stalled = 0, recoveries = 0;
    while (sent < count) {
        if (!stream && !audio_open()) { ++audio_errors; break; }
        uint64_t start = void_monotonic_ns();
        int32_t wrote = AAudioStream_write(stream, frames + 2 * sent, (int32_t)(count - sent), 100000000LL);
        uint64_t elapsed = void_monotonic_ns() - start;
        if (elapsed > audio_max_write_ns) audio_max_write_ns = elapsed;
        if (wrote > 0 && (uint32_t)wrote <= count - sent) {
            if ((uint32_t)wrote < count - sent) ++audio_partial;
            sent += (uint32_t)wrote; audio_sent += (uint32_t)wrote; stalled = 0;
            continue;
        }
        if (!wrote && ++stalled < 3) { ++audio_zero; continue; }
        if (!wrote) ++audio_zero;
        ++audio_errors;
        int64_t queued = AAudioStream_getFramesWritten(stream) - AAudioStream_getFramesRead(stream);
        if (queued > 0) audio_route_lost += (uint64_t)queued;
        void_log("[audio] write %d (%s), sent %u/%u, queued %lld: reopening",
            wrote, AAudio_convertResultToText(wrote ? wrote : AAUDIO_ERROR_TIMEOUT), sent, count, (long long)queued);
        audio_close();
        if (++recoveries > 1 || audio_paused) break;
    }
    audio_dropped += count - sent;
    /* Measure only frames actually accepted by AAudio, never attempted data. */
    static uint32_t level_frames;
    static double level_sum;
    static int level_peak;
    for (uint32_t i = 0; i < sent * 2; ++i) {
        int v = frames[i] < 0 ? -frames[i] : frames[i];
        if (v > level_peak) level_peak = v;
        level_sum += (double)frames[i] * frames[i];
    }
    level_frames += sent;
    now = void_monotonic_ns();
    if (now - audio_report_ns >= 10000000000ull) {
        int64_t queued = stream ? AAudioStream_getFramesWritten(stream) - AAudioStream_getFramesRead(stream) : 0;
        void_log("[audio] %.1f s: requested %llu sent %llu dropped %llu, short %u zero %u errors %u opens %u route-lost %llu, queue %lld, max gap/write %.1f/%.1f ms, peak %d rms %.0f xruns %d",
            (now - audio_report_ns) / 1e9, (unsigned long long)audio_requested, (unsigned long long)audio_sent,
            (unsigned long long)audio_dropped, audio_partial, audio_zero, audio_errors, audio_reopens,
            (unsigned long long)audio_route_lost, (long long)queued, audio_max_gap_ns / 1e6, audio_max_write_ns / 1e6,
            level_peak, level_frames ? sqrt(level_sum / (2.0 * level_frames)) : 0,
            stream ? AAudioStream_getXRunCount(stream) : -1);
        audio_requested = audio_sent = audio_dropped = audio_route_lost = 0;
        audio_partial = audio_zero = audio_errors = audio_reopens = 0;
        audio_max_gap_ns = audio_max_write_ns = 0;
        audio_report_ns = now;
        level_frames = 0;
        level_sum = 0;
        level_peak = 0;
    }
    pthread_mutex_unlock(&audio_lock);
    if (sent < count) usleep((useconds_t)((uint64_t)(count - sent) * 1000000u / 48000u));
}
#endif
