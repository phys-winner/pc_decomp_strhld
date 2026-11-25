#ifndef PORT_RENDERER_FAILURE_H
#define PORT_RENDERER_FAILURE_H
/* The shared launcher's renderer failure policy for native code (owner's decision, 24 November 2026).

   When the selected renderer fails, the game stops: it never switches to another driver or renderer by
   itself. It records the failure here and ends; the launcher then offers to start the next available
   renderer of its list (RendererFailure.java, LauncherActivity.rendererFailureOffer). Report:
     device_lost = 0  the renderer did not start: the driver library is missing or refused, the driver's
                      identity is wrong, creating the instance, device, surface, swapchain or EGL context
                      failed, a required feature is missing at run time (before the first presented frame);
     device_lost = 1  the GPU device was lost: VK_ERROR_DEVICE_LOST, an EGL context loss
                      (EGL_CONTEXT_LOST), a GL robustness reset (glGetGraphicsResetStatus != GL_NO_ERROR).
   The launcher counts a device loss before the first presented frame or within renderers.failureWindowSeconds
   (port.json, default 30) after it; later it is an ordinary crash, shown as "renderer failure: <reason>".

   Use:
     at startup, after Launch.attach (normal thread)   port_renderer_failure_init();
     after the first successful present                 port_renderer_presented();
     on the failure (any thread, also a signal handler) port_renderer_failed("vkCreateDevice: -3", 0);
   then end the game: from a thread that can call Java, prefer Launch.rendererFailed(activity, reason,
   deviceLost), which writes the same record, opens the launcher and ends the process; otherwise _exit(), and
   the launcher shows the offer when it is opened next.

   The record (files/launcher/renderer-failure-<distribution>.txt, path in PORT_RENDERER_FAILURE) is written
   with open/write/close only: no allocation, no locks, no stdio. The state below is one object shared by all
   translation units of a library (a weak definition); call presented and failed from the same library. */
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PORT_RENDERER_FAILURE_PATH_MAX 512
#define PORT_RENDERER_FAILURE_REASON_MAX 480
/* Android has both; the fallbacks only let the PC test (tests/renderer_failure_test.c) build the same code. */
#ifdef O_CLOEXEC
#define PORT_RENDERER_FAILURE_OPEN (O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC)
#else
#define PORT_RENDERER_FAILURE_OPEN (O_CREAT | O_WRONLY | O_TRUNC)
#endif
#ifdef O_BINARY
#define PORT_RENDERER_FAILURE_FLAGS (PORT_RENDERER_FAILURE_OPEN | O_BINARY)
#else
#define PORT_RENDERER_FAILURE_FLAGS PORT_RENDERER_FAILURE_OPEN
#endif

typedef struct port_renderer_failure_state {
    char path[PORT_RENDERER_FAILURE_PATH_MAX];
    char renderer[64];
    int64_t presented_ns;        /* first present, CLOCK_BOOTTIME; 0: none (atomic) */
    int ready;                   /* port_renderer_failure_init copied the environment */
} port_renderer_failure_state;

__attribute__((weak)) port_renderer_failure_state port_renderer_failure_global;

static inline int64_t port_renderer_failure_clock(clockid_t clock)
{
    struct timespec ts;
    if (clock_gettime(clock, &ts) != 0) return 0;
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static inline int64_t port_renderer_failure_uptime(void)
{
#ifdef CLOCK_BOOTTIME
    return port_renderer_failure_clock(CLOCK_BOOTTIME);
#else
    return port_renderer_failure_clock(CLOCK_MONOTONIC);
#endif
}

/* Copies at most cap - 1 bytes and terminates; 0 when src is null. */
static inline size_t port_renderer_failure_copy(char *dst, size_t cap, const char *src)
{
    size_t n = 0;
    if (src) while (src[n] && n + 1 < cap) { dst[n] = src[n]; ++n; }
    dst[n] = 0;
    return n;
}

/* Normal thread, at startup after Launch.attach: the environment's path and renderer, so that
   port_renderer_failed reads no environment later. */
static inline void port_renderer_failure_init(void)
{
    port_renderer_failure_state *s = &port_renderer_failure_global;
    port_renderer_failure_copy(s->path, sizeof s->path, getenv("PORT_RENDERER_FAILURE"));
    port_renderer_failure_copy(s->renderer, sizeof s->renderer, getenv("PORT_RENDERER"));
    __atomic_store_n(&s->ready, 1, __ATOMIC_RELEASE);
}

/* After the first successful present (eglSwapBuffers true; vkQueuePresentKHR VK_SUCCESS or
   VK_SUBOPTIMAL_KHR). Any thread; only the first call counts. */
static inline void port_renderer_presented(void)
{
    int64_t *presented = &port_renderer_failure_global.presented_ns;
    if (__atomic_load_n(presented, __ATOMIC_RELAXED) != 0) return;
    int64_t now = port_renderer_failure_uptime();
    /* Two threads presenting their first frame at once both store about the same time. */
    __atomic_store_n(presented, now ? now : 1, __ATOMIC_RELAXED);
}

/* Appends a literal as it is (up to cap). */
static inline size_t port_renderer_failure_raw(char *out, size_t pos, size_t cap, const char *literal)
{
    for (size_t i = 0; literal[i] && pos < cap; ++i) out[pos++] = literal[i];
    return pos;
}

/* Appends one line of text: control characters become spaces; a cut never splits a UTF-8 sequence. */
static inline size_t port_renderer_failure_text(char *out, size_t pos, size_t cap, const char *text, size_t limit)
{
    size_t start = pos, n = 0;
    if (!text) return pos;
    for (; text[n] && n < limit && pos < cap; ++n) {
        unsigned char c = (unsigned char)text[n];
        out[pos++] = (char)(c < 0x20 || c == 0x7f ? ' ' : c);
    }
    if (text[n]) {
        size_t lead = pos;
        while (lead > start && ((unsigned char)out[lead - 1] & 0xC0) == 0x80) --lead;
        if (lead > start) {
            unsigned char c = (unsigned char)out[lead - 1];
            size_t need = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
            if (pos - (lead - 1) < need) pos = lead - 1;
        }
    }
    return pos;
}

static inline size_t port_renderer_failure_number(char *out, size_t pos, size_t cap, int64_t value)
{
    char digits[24];
    size_t n = 0;
    uint64_t magnitude = value < 0 ? (uint64_t)0 - (uint64_t)value : (uint64_t)value;
    if (value < 0 && pos < cap) out[pos++] = '-';
    do { digits[n++] = (char)('0' + magnitude % 10); magnitude /= 10; } while (magnitude && n < sizeof digits);
    while (n && pos < cap) out[pos++] = digits[--n];
    return pos;
}

/* Records the renderer failure for the launcher. 0 when written, else a negative errno; errno itself is
   kept. Any thread, async-signal-safe (open, write, close, getpid, clock_gettime). Without
   port_renderer_failure_init it reads the environment, which is safe only on a normal thread. */
static inline int port_renderer_failed(const char *reason, int device_lost)
{
    port_renderer_failure_state *s = &port_renderer_failure_global;
    int saved = errno, result = 0;
    const char *path = s->path, *renderer = s->renderer;
    if (!__atomic_load_n(&s->ready, __ATOMIC_ACQUIRE)) {
        path = getenv("PORT_RENDERER_FAILURE");
        renderer = getenv("PORT_RENDERER");
    }
    if (!path || !*path) {
        errno = saved;
        return -ENOENT;
    }
    int64_t presented = __atomic_load_n(&s->presented_ns, __ATOMIC_RELAXED);
    int64_t since = presented ? (port_renderer_failure_uptime() - presented) / 1000000 : -1;
    if (presented && since < 0) since = 0;
    /* At most 24 + 25 + 11 + 73 + 29 + 38 + 8 + 480 bytes: the buffer never cuts a field. */
    char record[PORT_RENDERER_FAILURE_REASON_MAX + 256];
    size_t cap = sizeof record, pos = 0;
    pos = port_renderer_failure_raw(record, pos, cap, "port-renderer-failure 1\npid=");
    pos = port_renderer_failure_number(record, pos, cap, (int64_t)getpid());
    pos = port_renderer_failure_raw(record, pos, cap, device_lost ? "\nkind=lost\nrenderer=" : "\nkind=start\nrenderer=");
    pos = port_renderer_failure_text(record, pos, cap, renderer, 63);
    pos = port_renderer_failure_raw(record, pos, cap, "\ntime_ms=");
    pos = port_renderer_failure_number(record, pos, cap, port_renderer_failure_clock(CLOCK_REALTIME) / 1000000);
    pos = port_renderer_failure_raw(record, pos, cap, "\nsince_present_ms=");
    pos = port_renderer_failure_number(record, pos, cap, since);
    pos = port_renderer_failure_raw(record, pos, cap, "\nreason=");
    pos = port_renderer_failure_text(record, pos, cap - 1, reason, PORT_RENDERER_FAILURE_REASON_MAX);
    pos = port_renderer_failure_raw(record, pos, cap, "\n");
    int fd;
    do fd = open(path, PORT_RENDERER_FAILURE_FLAGS, 0600); while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        result = -errno;
    } else {
        size_t done = 0;
        while (done < pos) {
            ssize_t n = write(fd, record + done, pos - done);
            if (n > 0) done += (size_t)n;
            else if (n < 0 && errno == EINTR) continue;
            else { result = n < 0 ? -errno : -EIO; break; }
        }
        if (close(fd) != 0 && result == 0 && errno != EINTR) result = -errno;
    }
    errno = saved;
    return result;
}

#ifdef __cplusplus
}
#endif
#endif
