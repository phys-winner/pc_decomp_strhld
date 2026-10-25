/* Android's font rasterizer supplies Unicode glyph masks for DirectDraw GDI. */
#include "host.h"
#include "display.h"
#include "text_overlay.h"
#ifdef VOID_ANDROID_APP
#include <android/bitmap.h>
#include <jni.h>

static JavaVM *text_vm;
static jclass text_class, raster_class;
static jmethodID text_mask, raster_render;
static pthread_key_t env_key;

static void detach_text_thread(void *value)
{
    if (value && text_vm) (*text_vm)->DetachCurrentThread(text_vm);
}

void void_android_gdi_init(JNIEnv *env)
{
    if (text_vm) return;
    (*env)->GetJavaVM(env, &text_vm);
    jclass cls = (*env)->FindClass(env, "org/stronghold/port/GameActivity");
    if (!cls) void_fatal("GDI: cannot find Java font provider");
    text_class = (*env)->NewGlobalRef(env, cls);
    (*env)->DeleteLocalRef(env, cls);
    text_mask = (*env)->GetStaticMethodID(env, text_class, "textMask", "(Ljava/lang/String;III)Landroid/graphics/Bitmap;");
    if (!text_mask || pthread_key_create(&env_key, detach_text_thread)) void_fatal("GDI: font provider initialization failed");
    /* Crisp text is optional: without its renderer the raster text simply stays. Classes
     * are only visible to the app's class loader here, not on the attached game thread. */
    cls = (*env)->FindClass(env, "org/stronghold/port/TextRaster");
    if (cls) {
        raster_class = (*env)->NewGlobalRef(env, cls);
        (*env)->DeleteLocalRef(env, cls);
        raster_render = (*env)->GetStaticMethodID(env, raster_class, "render", "(Ljava/lang/String;IIIFFFII)Landroid/graphics/Bitmap;");
    }
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    if (!raster_render) void_log("[gdi] crisp text renderer unavailable; raster text only");
}

static JNIEnv *text_env(void)
{
    JNIEnv *env = NULL;
    if (!text_vm) return NULL;
    if ((*text_vm)->GetEnv(text_vm, (void **)&env, JNI_VERSION_1_6) == JNI_EDETACHED) {
        if ((*text_vm)->AttachCurrentThread(text_vm, &env, NULL) != JNI_OK) return NULL;
        pthread_setspecific(env_key, (void *)1);
    }
    return env;
}
#endif

/* The raster half of a run, as the original GDI output: the coverage mask
 * blended into 16/32-bit memory clipped to w x h, stores reported to the UI
 * capture, and the run noted for crisp text (which leaves these pixels alone). */
void void_android_blend(uint32_t dst, int w, int h, int pitch, int bpp, const uint8_t *mask, int mw, int mh, int stride,
    const uint16_t *text, int count, int size, int weight, int italic, int x, int y,
    uint32_t color, uint32_t bg, int opaque)
{
    int noted = bpp == 16 && stronghold_text_overlay_begin(dst, w, h, pitch, x, y, mask, mw, mh, stride,
        text, count, size, weight, italic, color, bg, opaque);
    unsigned cr = color & 255u, cg = color >> 8 & 255u, cb = color >> 16 & 255u;
    unsigned br = bg & 255u, bgc = bg >> 8 & 255u, bb = bg >> 16 & 255u;
    for (int row = 0; row < mh; ++row) {
        if (y + row < 0 || y + row >= h) continue;
        uint8_t *pixels = (uint8_t *)GPTR(dst) + (size_t)(y + row) * pitch;
        const uint8_t *alpha = mask + (size_t)row * stride;
        for (int col = 0; col < mw; ++col) {
            int xx = x + col;
            if (xx < 0 || xx >= w) continue;
            unsigned a = alpha[col], r = br, g = bgc, b = bb;
            if (!a && !opaque) continue;
            if (!opaque) {
                if (bpp == 16) {
                    uint16_t v; memcpy(&v, pixels + xx * 2, 2);
                    r = ((v >> 11) * 255u + 15) / 31u;
                    g = ((v >> 5 & 63u) * 255u + 31) / 63u;
                    b = ((v & 31u) * 255u + 15) / 31u;
                } else { b = pixels[xx * 4]; g = pixels[xx * 4 + 1]; r = pixels[xx * 4 + 2]; }
            }
            r = (cr * a + r * (255u - a) + 127u) / 255u;
            g = (cg * a + g * (255u - a) + 127u) / 255u;
            b = (cb * a + b * (255u - a) + 127u) / 255u;
            if (bpp == 16) {
                uint16_t v = (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | b >> 3);
                memcpy(pixels + xx * 2, &v, 2);
                if(stronghold_display_capture_active){
                    uint32_t va=dst+(uint32_t)((y+row)*pitch+xx*2);
                    if(opaque)stronghold_display_written(va,2);
                    else stronghold_display_blended(va,2,255u-a);
                }
            } else { pixels[xx * 4] = b; pixels[xx * 4 + 1] = g; pixels[xx * 4 + 2] = r; pixels[xx * 4 + 3] = 255; }
        }
    }
    if (noted) stronghold_text_overlay_end();
}

int void_android_text(uint32_t dst, int w, int h, int pitch, int bpp,
    const uint16_t *text, int count, int size, int weight, int italic, int x, int y,
    uint32_t color, uint32_t bg, int opaque, int *extent_w, int *extent_h)
{
    if (count < 0 || count > 65535 || (!text && count)) return 0;
#ifdef VOID_ANDROID_APP
    if (!text_vm || !text_class) return 0;
    JNIEnv *env = text_env();
    if (!env) return 0;
    jstring string = (*env)->NewString(env, (const jchar *)text, count);
    jobject bitmap = string ? (*env)->CallStaticObjectMethod(env, text_class, text_mask, string, size, weight, italic) : NULL;
    if (string) (*env)->DeleteLocalRef(env, string);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionDescribe(env);
        (*env)->ExceptionClear(env);
        if (bitmap) (*env)->DeleteLocalRef(env, bitmap);
        return 0;
    }
    if (!bitmap) return 0;
    AndroidBitmapInfo info;
    if (AndroidBitmap_getInfo(env, bitmap, &info) != ANDROID_BITMAP_RESULT_SUCCESS || info.format != ANDROID_BITMAP_FORMAT_A_8) {
        (*env)->DeleteLocalRef(env, bitmap);
        return 0;
    }
    if (extent_w) *extent_w = count ? (int)info.width : 0;
    if (extent_h) *extent_h = (int)info.height;
    if (!dst || !count) { (*env)->DeleteLocalRef(env, bitmap); return 1; }
    if ((bpp != 16 && bpp != 32) || w <= 0 || h <= 0 || w > 16384 || h > 16384 ||
        pitch < w * (bpp / 8) || (uint64_t)(h - 1) * pitch + (uint64_t)w * (bpp / 8) > UINT32_MAX ||
        !void_vm_readable(dst, (uint32_t)((uint64_t)(h - 1) * pitch + (uint64_t)w * (bpp / 8)))) {
        (*env)->DeleteLocalRef(env, bitmap);
        return 0;
    }
    void *mask;
    if (AndroidBitmap_lockPixels(env, bitmap, &mask) != ANDROID_BITMAP_RESULT_SUCCESS) {
        (*env)->DeleteLocalRef(env, bitmap);
        return 0;
    }
    void_android_blend(dst, w, h, pitch, bpp, (const uint8_t *)mask, (int)info.width, (int)info.height, (int)info.stride,
        text, count, size, weight, italic, x, y, color, bg, opaque);
    AndroidBitmap_unlockPixels(env, bitmap);
    (*env)->DeleteLocalRef(env, bitmap);
    return 1;
#else
    static int warned;
    if (!warned++) void_log("[gdi] Android font provider requires the APK; shell text unavailable");
    (void)dst; (void)w; (void)h; (void)pitch; (void)bpp; (void)size; (void)weight; (void)italic;
    (void)x; (void)y; (void)color; (void)bg; (void)opaque; (void)extent_w; (void)extent_h;
    return 0;
#endif
}

/* Crisp text: the run drawn by TextRaster.render into a tight width x height
 * coverage image (row stride = width). 0 when it is unavailable. */
int void_android_text_raster(const uint16_t *text, int count, int size, int weight, int italic,
    float scale, float dx, float dy, int width, int height, uint8_t *out)
{
    if (count <= 0 || count > 65535 || !text || !out || width <= 0 || height <= 0) return 0;
#ifdef VOID_ANDROID_APP
    if (!raster_render) return 0;
    JNIEnv *env = text_env();
    if (!env) return 0;
    jstring string = (*env)->NewString(env, (const jchar *)text, count);
    jobject bitmap = string ? (*env)->CallStaticObjectMethod(env, raster_class, raster_render, string, size, weight, italic,
        (jfloat)scale, (jfloat)dx, (jfloat)dy, width, height) : NULL;
    if (string) (*env)->DeleteLocalRef(env, string);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionDescribe(env);
        (*env)->ExceptionClear(env);
        if (bitmap) (*env)->DeleteLocalRef(env, bitmap);
        return 0;
    }
    if (!bitmap) return 0;
    AndroidBitmapInfo info;
    void *mask = NULL;
    int ok = AndroidBitmap_getInfo(env, bitmap, &info) == ANDROID_BITMAP_RESULT_SUCCESS &&
             info.format == ANDROID_BITMAP_FORMAT_A_8 && (int)info.width == width && (int)info.height == height &&
             AndroidBitmap_lockPixels(env, bitmap, &mask) == ANDROID_BITMAP_RESULT_SUCCESS;
    if (ok) {
        for (int row = 0; row < height; ++row)
            memcpy(out + (size_t)row * width, (const uint8_t *)mask + (size_t)row * info.stride, (size_t)width);
        AndroidBitmap_unlockPixels(env, bitmap);
    }
    (*env)->DeleteLocalRef(env, bitmap);
    return ok;
#else
    (void)size; (void)weight; (void)italic; (void)scale; (void)dx; (void)dy;
    return 0;
#endif
}

#ifdef VOID_ANDROID_APP
int void_android_gdi_selftest(void)
{
    const uint16_t text[] = {'A', 'g', 0x042F};
    uint32_t pixels = void_galloc(64 * 48 * 4);
    int ew = 0, eh = 0, changed = 0, ok = pixels != 0;
    if (!ok) return 0;
    memset(GPTR(pixels), 0x33, 64 * 48 * 4);
    ok &= void_android_text(pixels, 64, 48, 128, 16, text, 3, -16, 400, 0,
                           2, 2, 0x000000FF, 0, 0, &ew, &eh);
    for (int y = 2; y < 46; ++y) for (int x = 2; x < 62; ++x)
        changed |= MEM16(pixels + (uint32_t)y * 128u + (uint32_t)x * 2u) != 0x3333;
    ok &= ew > 5 && ew < 62 && eh > 5 && eh < 46 && changed && MEM16(pixels) == 0x3333;
    memset(GPTR(pixels), 0, 64 * 48 * 4);
    ok &= void_android_text(pixels, 64, 48, 256, 32, text, 3, 18, 700, 0,
                           2, 2, 0x00FF0000, 0, 1, &ew, &eh);
    changed = 0;
    for (int y = 2; y < 46; ++y) for (int x = 2; x < 62; ++x)
        changed |= MEM32(pixels + (uint32_t)y * 256u + (uint32_t)x * 4u) != 0;
    ok &= changed && MEM32(pixels) == 0;
    ok &= void_android_text(0, 0, 0, 0, 0, text, 0, -16, 400, 0, 0, 0, 0, 0, 0, &ew, &eh) && ew == 0;
    /* The crisp renderer keeps the raster box: twice the size, twice the extent. */
    if (raster_render) {
        int sw = 0, sh = 0, ink = 0;
        void_android_text(0, 0, 0, 0, 0, text, 3, -16, 400, 0, 0, 0, 0, 0, 0, &sw, &sh);
        uint8_t *big = calloc((size_t)(2 * sw + 1) * (2 * sh + 1), 1);
        ok &= big && void_android_text_raster(text, 3, -16, 400, 0, 2.f, 0.5f, 0.f, 2 * sw + 1, 2 * sh + 1, big);
        for (int i = 0; big && i < (2 * sw + 1) * (2 * sh + 1); ++i) ink += big[i] != 0;
        ok &= ink > 8 * sw;
        free(big);
    }
    void_gfree(pixels);
    void_log("[selftest] Android GDI Latin/Cyrillic, RGB565/BGRA, metrics, crisp text: %s", ok ? "PASS" : "FAIL");
    return ok;
}
#endif
