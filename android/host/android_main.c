/* Android application glue: JNI from GameActivity, the native window, the
 * game thread, touch input in DirectInput scan codes, and selectable audio output. */
#ifdef VOID_ANDROID_APP
#define _GNU_SOURCE
#include "host.h"
#include "input.h"
#include "display.h"
#include "touch_api.h"
#include "game_ui.h"
#include "feedback.h"
#include "mods.h"
#include <aaudio/AAudio.h>
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include <SLES/OpenSLES_AndroidConfiguration.h>
#include <dlfcn.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <jni.h>
#include <math.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>

int void_game_main(const char *game, const char *data, const char *images, int selftest);
void void_process_exit(uint32_t code, int run_detach);

/* The linker synthesizes a separate Win32 registry for each ELF. */
extern const void_shim __start_void_shims[], __stop_void_shims[]
    __attribute__((weak, visibility("hidden")));

size_t stronghold_common_shims(const void_shim **first)
{
    *first = __start_void_shims;
    return *first ? (size_t)(__stop_void_shims - *first) : 0;
}

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
static int audio_compat;
int void_audio_compatibility(void) { return audio_compat; }
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

/* The shared launcher's renderer failure policy (owner's decision of 24 November 2026): renderer_present.c has
 * written the record (it lives in the edition's library, where the first presented frame is marked); this
 * returns to the launcher through GameActivity.onRendererFailed, which ends the process. The launcher reads the
 * record through CrashMonitor and says that no other renderer is available. The failing thread does not return. */
void stronghold_renderer_end(const char *reason, int device_lost)
{
    void_log("[renderer] %s: %s", device_lost ? "context lost" : "did not start", reason ? reason : "");
    /* `activity` holds GameActivity's class (nativeStart is a static native): its static entry finds the activity. */
    JNIEnv *env = NULL;
    if (jvm && activity && (*jvm)->AttachCurrentThread(jvm, &env, NULL) == JNI_OK && env) {
        jmethodID method = (*env)->GetStaticMethodID(env, (jclass)activity, "onNativeRendererFailed", "(Ljava/lang/String;Z)V");
        if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); method = NULL; }
        jstring text = method ? (*env)->NewStringUTF(env, reason ? reason : "") : NULL;
        if (method && text) (*env)->CallStaticVoidMethod(env, (jclass)activity, method, text, device_lost ? JNI_TRUE : JNI_FALSE);
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    }
    /* Java ends the process; the game must not go on without a picture meanwhile. */
    sleep(15);
    _exit(1);
}

void void_android_process_exit(uint32_t code)
{
    if (!code && launch_marker[0]) unlink(launch_marker);
}

JNIEXPORT jboolean JNICALL Java_org_stronghold_port_GameActivity_nativeStart(JNIEnv *env, jobject self, jstring game,
    jstring data, jstring images, jstring marker, jint screen_w, jint screen_h, jboolean verbose, jboolean skip_movies, jboolean compat_audio)
{
    void void_android_gdi_init(JNIEnv *env);
    void_android_gdi_init(env);
    if (game_started) return JNI_TRUE;
    audio_compat = compat_audio ? 1 : 0;
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
JNIEXPORT jstring JNICALL Java_org_stronghold_port_GameActivity_nativeMods(JNIEnv *env,jclass self,jstring path)
{
    (void)self;
    if (game_started) return (*env)->NewStringUTF(env,"Mod changes require a new game process");
    const char *text=(*env)->GetStringUTFChars(env,path,NULL);
    if (!text) return NULL;
    const char *error=stronghold_mods_configure(text);
    (*env)->ReleaseStringUTFChars(env,path,text);
    return error ? (*env)->NewStringUTF(env,error) : NULL;
}
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

/* FpsOverlay.Source: intervals between presented frames in microseconds, oldest first. */
JNIEXPORT jlong JNICALL Java_org_stronghold_port_GameActivity_nativeFrameTimes(JNIEnv *env, jclass self, jintArray intervals)
{
    (void)self;
    uint64_t stronghold_frame_times(int32_t *out, size_t length);
    int32_t values[304];
    jsize length = (*env)->GetArrayLength(env, intervals);
    if (length <= 0 || length > 304) return 0;
    uint64_t count = stronghold_frame_times(values, (size_t)length);
    (*env)->SetIntArrayRegion(env, intervals, 0, length, values);
    return (jlong)count;
}

/* GL_RENDERER, a newline and GL_VERSION of the game context, or NULL before it exists. */
JNIEXPORT jstring JNICALL Java_org_stronghold_port_GameActivity_nativeRendererInfo(JNIEnv *env, jclass self)
{
    (void)self;
    const char *stronghold_renderer_info(void);
    const char *info = stronghold_renderer_info();
    return info && *info ? (*env)->NewStringUTF(env, info) : NULL;
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
enum { AUDIO_OUTPUT_RATE = 48000, AUDIO_WRITE_FRAMES = 480, AUDIO_FIFO_FRAMES = 8192 };
static AAudioStream *stream;
static pthread_mutex_t audio_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t audio_requested, audio_sent, audio_dropped, audio_route_lost;
static uint32_t audio_errors, audio_reopens;
static uint64_t audio_report_ns, audio_last_ns, audio_max_gap_ns, audio_start_ns;
static pthread_mutex_t audio_pause_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t audio_pause_start_ms, audio_pause_total_ms;
static _Atomic int audio_shutting_down;
static _Atomic uint32_t audio_queued_frames;
/* One writer, one device callback. The callback never waits for the guest or a mutex. */
static int16_t audio_fifo[AUDIO_FIFO_FRAMES][2];
static _Atomic uint64_t audio_fifo_in, audio_fifo_out;
static _Atomic uint32_t audio_callback_silence;
static _Atomic int audio_callback_error, audio_fifo_primed;
static _Atomic uint64_t audio_callback_ns;
static _Atomic int64_t audio_callback_tail;
static int32_t audio_xruns;
static uint32_t audio_fifo_target = 2 * AUDIO_WRITE_FRAMES;
static SLObjectItf audio_sl_engine, audio_sl_mix, audio_sl_player;
static SLPlayItf audio_sl_play;
static SLAndroidSimpleBufferQueueItf audio_sl_queue;
enum { AUDIO_SL_BUFFERS = 4 };
static int16_t audio_sl_pcm[AUDIO_SL_BUFFERS][AUDIO_WRITE_FRAMES * 2];
static _Atomic uint32_t audio_sl_lengths[AUDIO_SL_BUFFERS];
static _Atomic uint64_t audio_sl_in, audio_sl_out;
static _Atomic int64_t audio_sl_written, audio_sl_completed;

static uint32_t audio_copy_callback(void *data, int32_t frames)
{
    atomic_store_explicit(&audio_callback_ns, void_monotonic_ns(), memory_order_relaxed);
    uint64_t out = atomic_load_explicit(&audio_fifo_out, memory_order_relaxed);
    uint64_t in = atomic_load_explicit(&audio_fifo_in, memory_order_acquire);
    uint32_t count = in - out < (uint32_t)frames ? (uint32_t)(in - out) : (uint32_t)frames;
    uint32_t at = (uint32_t)out % AUDIO_FIFO_FRAMES;
    uint32_t first = count < AUDIO_FIFO_FRAMES - at ? count : AUDIO_FIFO_FRAMES - at;
    memcpy(data, audio_fifo[at], first * 4u);
    memcpy((int16_t *)data + first * 2u, audio_fifo, (count - first) * 4u);
    memset((int16_t *)data + count * 2u, 0, ((uint32_t)frames - count) * 4u);
    return count;
}

static void audio_commit_callback(uint32_t count, int32_t frames, int64_t tail)
{
    uint64_t out = atomic_load_explicit(&audio_fifo_out, memory_order_relaxed);
    /* The last PCM may still be inside this callback when drain observes an empty FIFO. */
    atomic_store_explicit(&audio_callback_tail, tail, memory_order_relaxed);
    atomic_store_explicit(&audio_fifo_out, out + count, memory_order_release);
    if (atomic_load_explicit(&audio_fifo_primed, memory_order_relaxed))
        atomic_fetch_add_explicit(&audio_callback_silence, (uint32_t)frames - count, memory_order_relaxed);
}

static aaudio_data_callback_result_t audio_callback(AAudioStream *s, void *user, void *data, int32_t frames)
{
    (void)user;
    uint32_t count = audio_copy_callback(data, frames);
    audio_commit_callback(count, frames, AAudioStream_getFramesWritten(s) + frames);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static void audio_error_callback(AAudioStream *s, void *user, aaudio_result_t error)
{
    (void)s; (void)user;
    atomic_store(&audio_callback_error, error);
}

uint32_t void_audio_latency_frames(void) { return atomic_load(&audio_queued_frames); }

static void audio_set_paused(int paused)
{
    pthread_mutex_lock(&audio_pause_lock);
    uint32_t now = void_tick_ms();
    if (paused && !audio_paused) audio_pause_start_ms = now;
    if (!paused && audio_paused) audio_pause_total_ms += now - audio_pause_start_ms;
    audio_paused = paused;
    pthread_mutex_unlock(&audio_pause_lock);
}

int void_audio_is_paused(void) { return audio_paused; }

/* Miles' refill timeout uses timeGetTime: background time must not discard
 * already mixed speech when its timer resumes against an unchanged cursor. */
uint32_t void_audio_clock_ms(void)
{
    pthread_mutex_lock(&audio_pause_lock);
    uint32_t value = (audio_paused ? audio_pause_start_ms : void_tick_ms()) - audio_pause_total_ms;
    pthread_mutex_unlock(&audio_pause_lock);
    return value;
}

static void audio_sl_close(void)
{
    if (audio_sl_play) (*audio_sl_play)->SetPlayState(audio_sl_play, SL_PLAYSTATE_STOPPED);
    if (audio_sl_player) (*audio_sl_player)->Destroy(audio_sl_player);
    audio_sl_player = NULL; audio_sl_play = NULL; audio_sl_queue = NULL;
    if (audio_sl_mix) (*audio_sl_mix)->Destroy(audio_sl_mix);
    audio_sl_mix = NULL;
    if (audio_sl_engine) (*audio_sl_engine)->Destroy(audio_sl_engine);
    audio_sl_engine = NULL;
}

static int audio_opened(void) { return stream || audio_sl_player; }

static int64_t audio_device_queued(void)
{
    return stream ? AAudioStream_getFramesWritten(stream) - AAudioStream_getFramesRead(stream) :
        audio_sl_player ? atomic_load(&audio_sl_written) - atomic_load(&audio_sl_completed) : 0;
}

static void audio_close(void)
{
    audio_sl_close();
    atomic_store(&audio_queued_frames, 0);
    if (!stream) return;
    AAudioStream_requestStop(stream);
    AAudioStream_close(stream);
    stream = NULL;
    atomic_store(&audio_queued_frames, 0);
}

static void audio_prepare_fifo(void)
{
    /* PCM not yet handed to the old device survives a route change. */
    if (atomic_load(&audio_fifo_in) == atomic_load(&audio_fifo_out)) {
        memset(audio_fifo, 0, sizeof audio_fifo);
        atomic_store(&audio_fifo_out, 0);
        atomic_store(&audio_fifo_in, audio_fifo_target);
        atomic_store(&audio_fifo_primed, 0);
    }
    atomic_store(&audio_callback_error, 0);
    audio_start_ns = void_monotonic_ns();
    atomic_store(&audio_callback_ns, audio_start_ns);
    atomic_store(&audio_callback_tail, 0);
    audio_xruns = 0;
}

static SLresult audio_sl_enqueue(const int16_t *pcm, uint32_t frames)
{
    uint64_t in = atomic_load_explicit(&audio_sl_in, memory_order_relaxed);
    unsigned slot = (unsigned)(in % AUDIO_SL_BUFFERS);
    memcpy(audio_sl_pcm[slot], pcm, frames * 4u);
    atomic_store_explicit(&audio_sl_lengths[slot], frames, memory_order_release);
    SLresult result = (*audio_sl_queue)->Enqueue(audio_sl_queue, audio_sl_pcm[slot], frames * 4u);
    if (result == SL_RESULT_SUCCESS) {
        atomic_fetch_add(&audio_sl_written, frames);
        atomic_store_explicit(&audio_sl_in, in + 1, memory_order_release);
    }
    return result;
}

static void audio_sl_callback(SLAndroidSimpleBufferQueueItf queue, void *user)
{
    /* The writer supplies real PCM when slots are free; a route's callback
     * batch must not consume an extra FIFO as artificially inserted silence. */
    (void)queue; (void)user;
    uint64_t out = atomic_load_explicit(&audio_sl_out, memory_order_relaxed);
    uint32_t frames = atomic_load_explicit(&audio_sl_lengths[out % AUDIO_SL_BUFFERS], memory_order_acquire);
    atomic_fetch_add(&audio_sl_completed, frames);
    atomic_store(&audio_callback_ns, void_monotonic_ns());
    atomic_store_explicit(&audio_sl_out, out + 1, memory_order_release);
}

static int audio_sl_open(void)
{
    SLEngineItf engine;
    SLresult result;
#define SL_TRY(call) do { result = (call); if (result != SL_RESULT_SUCCESS) goto fail; } while (0)
    SL_TRY(slCreateEngine(&audio_sl_engine, 0, NULL, 0, NULL, NULL));
    SL_TRY((*audio_sl_engine)->Realize(audio_sl_engine, SL_BOOLEAN_FALSE));
    SL_TRY((*audio_sl_engine)->GetInterface(audio_sl_engine, SL_IID_ENGINE, &engine));
    SL_TRY((*engine)->CreateOutputMix(engine, &audio_sl_mix, 0, NULL, NULL));
    SL_TRY((*audio_sl_mix)->Realize(audio_sl_mix, SL_BOOLEAN_FALSE));
    SLDataLocator_AndroidSimpleBufferQueue location = {SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, AUDIO_SL_BUFFERS};
    SLDataFormat_PCM format = {SL_DATAFORMAT_PCM, 2, SL_SAMPLINGRATE_48, SL_PCMSAMPLEFORMAT_FIXED_16,
        SL_PCMSAMPLEFORMAT_FIXED_16, SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT, SL_BYTEORDER_LITTLEENDIAN};
    SLDataSource source = {&location, &format};
    SLDataLocator_OutputMix output = {SL_DATALOCATOR_OUTPUTMIX, audio_sl_mix};
    SLDataSink sink = {&output, NULL};
    SLInterfaceID ids[] = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE, SL_IID_ANDROIDCONFIGURATION};
    SLboolean required[] = {SL_BOOLEAN_TRUE, SL_BOOLEAN_FALSE};
    SL_TRY((*engine)->CreateAudioPlayer(engine, &audio_sl_player, &source, &sink, 2, ids, required));
    SLAndroidConfigurationItf config;
    if ((*audio_sl_player)->GetInterface(audio_sl_player, SL_IID_ANDROIDCONFIGURATION, &config) == SL_RESULT_SUCCESS) {
        SLint32 type = SL_ANDROID_STREAM_MEDIA;
        SLuint32 performance = SL_ANDROID_PERFORMANCE_LATENCY;
        (*config)->SetConfiguration(config, SL_ANDROID_KEY_STREAM_TYPE, &type, sizeof type);
        (*config)->SetConfiguration(config, SL_ANDROID_KEY_PERFORMANCE_MODE, &performance, sizeof performance);
    }
    SL_TRY((*audio_sl_player)->Realize(audio_sl_player, SL_BOOLEAN_FALSE));
    SL_TRY((*audio_sl_player)->GetInterface(audio_sl_player, SL_IID_PLAY, &audio_sl_play));
    SL_TRY((*audio_sl_player)->GetInterface(audio_sl_player, SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &audio_sl_queue));
    SL_TRY((*audio_sl_queue)->RegisterCallback(audio_sl_queue, audio_sl_callback, NULL));
    atomic_store(&audio_callback_error, 0);
    audio_start_ns = void_monotonic_ns();
    atomic_store(&audio_callback_ns, audio_start_ns);
    atomic_store(&audio_sl_in, 0);
    atomic_store(&audio_sl_out, 0);
    atomic_store(&audio_sl_written, 0);
    atomic_store(&audio_sl_completed, 0);
    int16_t silence[AUDIO_WRITE_FRAMES * 2] = {0};
    for (unsigned slot = 0; slot < AUDIO_SL_BUFFERS; ++slot)
        SL_TRY(audio_sl_enqueue(silence, AUDIO_WRITE_FRAMES));
    SL_TRY((*audio_sl_play)->SetPlayState(audio_sl_play, SL_PLAYSTATE_PLAYING));
    ++audio_reopens;
    void_log("[audio] opened OpenSL ES compatibility: rate %d, device queue %d x %d, producer backpressure",
        AUDIO_OUTPUT_RATE, AUDIO_SL_BUFFERS, AUDIO_WRITE_FRAMES);
    return 1;
fail:
    void_log("[audio] OpenSL ES open failed %u", (unsigned)result);
    audio_sl_close();
    return 0;
#undef SL_TRY
}

static int audio_open(void)
{
    if (audio_compat) return audio_sl_open();
    AAudioStreamBuilder *b = NULL;
    aaudio_result_t result = AAudio_createStreamBuilder(&b);
    if (result != AAUDIO_OK) return 0;
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(b, 2);
    AAudioStreamBuilder_setSampleRate(b, AUDIO_OUTPUT_RATE);
    /* AAudioStreamBuilder_setUsage is an API 28 symbol and the port starts on API 26: on Android 8
     * the stream keeps the default usage (media), which takes the same output route. */
    static void (*set_usage)(AAudioStreamBuilder *, int32_t);
    static int usage_resolved;
    if (!usage_resolved) {
        void *library = dlopen("libaaudio.so", RTLD_NOW);
        set_usage = library ? (void (*)(AAudioStreamBuilder *, int32_t))dlsym(library, "AAudioStreamBuilder_setUsage") : NULL;
        usage_resolved = 1;
    }
    if (set_usage) set_usage(b, 14 /* AAUDIO_USAGE_GAME */);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setDataCallback(b, audio_callback, NULL);
    AAudioStreamBuilder_setErrorCallback(b, audio_error_callback, NULL);
    AAudioStreamBuilder_setFramesPerDataCallback(b, AUDIO_WRITE_FRAMES);
    result = AAudioStreamBuilder_openStream(b, &stream);
    AAudioStreamBuilder_delete(b);
    if (result != AAUDIO_OK) { stream = NULL; return 0; }
    int32_t burst = AAudioStream_getFramesPerBurst(stream);
    int32_t capacity = AAudioStream_getBufferCapacityInFrames(stream);
    int32_t rate = AAudioStream_getSampleRate(stream);
    aaudio_performance_mode_t mode = AAudioStream_getPerformanceMode(stream);
    int32_t target = burst > 0 ? 2 * burst : 2400;
    if (target > capacity) target = capacity;
    int32_t size = AAudioStream_setBufferSizeInFrames(stream, target);
    if (size < 0) size = AAudioStream_getBufferSizeInFrames(stream);
    /* Two blocks cover writer jitter; a large legacy burst also needs the
     * fixed-block adapter's partial callback carried over from its last batch. */
    audio_fifo_target = burst > AUDIO_WRITE_FRAMES ?
        (((uint32_t)burst + AUDIO_WRITE_FRAMES - 1) / AUDIO_WRITE_FRAMES + 1) * AUDIO_WRITE_FRAMES : AUDIO_WRITE_FRAMES;
    if (audio_fifo_target < 2 * AUDIO_WRITE_FRAMES) audio_fifo_target = 2 * AUDIO_WRITE_FRAMES;
    if (audio_fifo_target > AUDIO_FIFO_FRAMES)
        audio_fifo_target = AUDIO_FIFO_FRAMES / AUDIO_WRITE_FRAMES * AUDIO_WRITE_FRAMES;
    audio_prepare_fifo();
    result = AAudioStream_requestStart(stream);
    if (result != AAUDIO_OK) { audio_close(); return 0; }
    ++audio_reopens;
    void_log("[audio] opened callback: rate %d, burst %d, buffer %d/%d, FIFO %d, performance %d",
        rate, burst, size, capacity, audio_fifo_target, mode);
    return 1;
}

void void_audio_wait_ready(void)
{
retry:
    if (audio_paused) {
        pthread_mutex_lock(&audio_lock);
        if (audio_sl_play) {
            if ((*audio_sl_play)->SetPlayState(audio_sl_play, SL_PLAYSTATE_PAUSED) != SL_RESULT_SUCCESS) {
                int64_t queued = audio_device_queued();
                if (queued > 0) audio_route_lost += (uint64_t)queued;
                audio_close();
            }
        } else if (stream) {
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
        /* Older MMAP routes advance past unwritten PCM while paused; restarting
         * that clock asks the callback to catch up in a burst. Keep our FIFO. */
        if (stream && AAudioStream_getFramesRead(stream) > AAudioStream_getFramesWritten(stream)) {
            void_log("[audio] paused device cursor passed submitted PCM: reopening");
            audio_close();
        }
        audio_start_ns = void_monotonic_ns();
        atomic_store(&audio_callback_ns, audio_start_ns);
        if (audio_sl_play && (*audio_sl_play)->SetPlayState(audio_sl_play, SL_PLAYSTATE_PLAYING) != SL_RESULT_SUCCESS)
            audio_close();
        if (stream && AAudioStream_requestStart(stream) != AAUDIO_OK) audio_close();
        audio_last_ns = 0;
        pthread_mutex_unlock(&audio_lock);
    }
    /* Apply device backpressure before pulling another block from Miles. */
    for (;;) {
        pthread_mutex_lock(&audio_lock);
        /* Snapshot first: a concurrently updated timestamp must not underflow the age. */
        uint64_t callback_ns = atomic_load(&audio_callback_ns), now = void_monotonic_ns();
        /* Legacy startup may prefill callbacks before the device actually starts. */
        if (audio_opened() && !atomic_load(&audio_callback_error) &&
            now >= callback_ns && now - callback_ns > 500000000u &&
            now >= audio_start_ns && now - audio_start_ns > 2000000000u)
            atomic_store(&audio_callback_error, AAUDIO_ERROR_TIMEOUT);
        if (audio_opened() && atomic_load(&audio_callback_error)) {
            int error = atomic_exchange(&audio_callback_error, 0);
            int64_t queued = audio_device_queued();
            audio_route_lost += queued > 0 ? (uint64_t)queued : 0;
            ++audio_errors;
            void_log("[audio] callback error %d (%s): reopening", error, AAudio_convertResultToText(error));
            audio_close();
        }
        int ready = !audio_opened() && !audio_open();
        if (audio_sl_player)
            ready = atomic_load(&audio_sl_in) - atomic_load_explicit(&audio_sl_out, memory_order_acquire) < AUDIO_SL_BUFFERS;
        else if (stream) ready = atomic_load(&audio_fifo_in) - atomic_load(&audio_fifo_out) <= audio_fifo_target - AUDIO_WRITE_FRAMES;
        pthread_mutex_unlock(&audio_lock);
        if (ready) break;
        usleep(1000);
        if (audio_paused && !audio_shutting_down) goto retry;
    }
}

void void_audio_begin_shutdown(void) { audio_shutting_down = 1; }

void void_audio_drain(void)
{
    pthread_mutex_lock(&audio_lock);
    if (audio_sl_player) {
        uint64_t deadline = void_monotonic_ns() + 500000000u;
        while (atomic_load(&audio_sl_in) != atomic_load(&audio_sl_out) &&
               !atomic_load(&audio_callback_error) && void_monotonic_ns() < deadline) usleep(1000);
        int64_t target = atomic_load(&audio_sl_written), completed = atomic_load(&audio_sl_completed);
        /* A queue callback confirms consumption, not a DAC timestamp. Retain the hardware tail. */
        if (completed >= target) usleep(100000);
        void_log("[audio] OpenSL ES exit drain: written %lld consumed %lld, pending %lld, %s",
            (long long)target, (long long)completed, (long long)(target - completed),
            completed >= target ? "complete" : "timeout");
        audio_close();
    }
    if (stream) {
        uint64_t fifo_deadline = void_monotonic_ns() + 500000000u;
        while (atomic_load(&audio_fifo_in) != atomic_load(&audio_fifo_out) &&
               !atomic_load(&audio_callback_error) && void_monotonic_ns() < fifo_deadline) usleep(1000);
        uint32_t pending = (uint32_t)(atomic_load(&audio_fifo_in) - atomic_load(&audio_fifo_out));
        int64_t target = AAudioStream_getFramesWritten(stream), position = 0, stamp = 0;
        int64_t callback_tail = atomic_load(&audio_callback_tail);
        if (target < callback_tail) target = callback_tail;
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
        void_log("[audio] exit drain: written %lld presented %lld timestamp %d, %.1f ms (read %lld hw %lld, pending %u, %s)",
            (long long)target, (long long)position, timestamp, (void_monotonic_ns() - start) / 1e6,
            (long long)consumed, (long long)hardware, pending, !pending && position >= target ? "complete" : "timeout");
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
    uint32_t sent = 0;
    unsigned retries = 0;
    while (sent < count) {
        if (!audio_opened() && !audio_open()) { ++audio_errors; break; }
        if (audio_sl_player) {
            if (atomic_load(&audio_sl_in) - atomic_load_explicit(&audio_sl_out, memory_order_acquire) < AUDIO_SL_BUFFERS) {
                uint32_t wrote = count - sent < AUDIO_WRITE_FRAMES ? count - sent : AUDIO_WRITE_FRAMES;
                SLresult result = audio_sl_enqueue(frames + 2 * sent, wrote);
                if (result == SL_RESULT_SUCCESS) { sent += wrote; audio_sent += wrote; continue; }
                int64_t queued = audio_device_queued();
                if (queued > 0) audio_route_lost += (uint64_t)queued;
                ++audio_errors;
                void_log("[audio] OpenSL ES enqueue failed %u: reopening", (unsigned)result);
                audio_close();
                if (retries++ == 0) continue;
                break;
            }
            pthread_mutex_unlock(&audio_lock);
            void_audio_wait_ready();
            pthread_mutex_lock(&audio_lock);
            continue;
        }
        uint64_t in = atomic_load_explicit(&audio_fifo_in, memory_order_relaxed);
        uint64_t out = atomic_load_explicit(&audio_fifo_out, memory_order_acquire);
        uint32_t used = (uint32_t)(in - out);
        uint32_t room = used < audio_fifo_target ? audio_fifo_target - used : 0;
        uint32_t wrote = count - sent < room ? count - sent : room;
        uint32_t at = (uint32_t)in % AUDIO_FIFO_FRAMES;
        uint32_t first = wrote < AUDIO_FIFO_FRAMES - at ? wrote : AUDIO_FIFO_FRAMES - at;
        memcpy(audio_fifo[at], frames + 2 * sent, first * 4u);
        memcpy(audio_fifo, frames + 2 * (sent + first), (wrote - first) * 4u);
        atomic_store(&audio_fifo_primed, 1);
        atomic_store_explicit(&audio_fifo_in, in + wrote, memory_order_release);
        if (wrote) {
            sent += wrote; audio_sent += wrote;
            continue;
        }
        pthread_mutex_unlock(&audio_lock);
        void_audio_wait_ready();
        pthread_mutex_lock(&audio_lock);
    }
    audio_dropped += count - sent;
    /* Count PCM published to the callback, never attempted data. */
    static uint32_t level_frames;
    static double level_sum;
    static int level_peak;
    for (uint32_t i = 0; i < sent * 2; ++i) {
        int v = frames[i] < 0 ? -frames[i] : frames[i];
        if (v > level_peak) level_peak = v;
        level_sum += (double)frames[i] * frames[i];
    }
    level_frames += sent;
    int64_t queued = audio_device_queued();
    uint32_t pending = (uint32_t)(atomic_load(&audio_fifo_in) - atomic_load(&audio_fifo_out));
    atomic_store(&audio_queued_frames, pending + (queued > 0 ? (uint32_t)queued : 0));
    if (stream) {
        int32_t xruns = AAudioStream_getXRunCount(stream);
        if (xruns > audio_xruns) {
            int32_t size = AAudioStream_getBufferSizeInFrames(stream);
            int32_t burst = AAudioStream_getFramesPerBurst(stream);
            int32_t capacity = AAudioStream_getBufferCapacityInFrames(stream);
            /* A legacy capacity may have padding; growing into a partial burst
             * creates repeated short batches instead of useful underrun reserve. */
            if (burst > 0 && size <= capacity - burst)
                AAudioStream_setBufferSizeInFrames(stream, size + burst);
        }
        audio_xruns = xruns;
    }
    now = void_monotonic_ns();
    if (now - audio_report_ns >= 10000000000ull) {
        void_log("[audio] %.1f s: requested %llu sent %llu dropped %llu, errors %u opens %u route-lost %llu, queue %lld+%u, max gap %.1f ms, peak %d rms %.0f xruns %d callback-silence %u",
            (now - audio_report_ns) / 1e9, (unsigned long long)audio_requested, (unsigned long long)audio_sent,
            (unsigned long long)audio_dropped, audio_errors, audio_reopens,
            (unsigned long long)audio_route_lost, (long long)queued, pending, audio_max_gap_ns / 1e6,
            level_peak, level_frames ? sqrt(level_sum / (2.0 * level_frames)) : 0,
            stream ? AAudioStream_getXRunCount(stream) : -1, atomic_exchange(&audio_callback_silence, 0));
        audio_requested = audio_sent = audio_dropped = audio_route_lost = 0;
        audio_errors = audio_reopens = 0;
        audio_max_gap_ns = 0;
        audio_report_ns = now;
        level_frames = 0;
        level_sum = 0;
        level_peak = 0;
    }
    pthread_mutex_unlock(&audio_lock);
    if (sent < count) usleep((useconds_t)((uint64_t)(count - sent) * 1000000u / AUDIO_OUTPUT_RATE));
}
#endif
