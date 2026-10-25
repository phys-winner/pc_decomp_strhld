/* advapi32 (registry), shell32/shlwapi (folders, path helpers), winmm (time
 * and multimedia timers), ole32/oleaut32 (COM runtime basics), dbghelp
 * (crash reporting, unavailable) and the small gdi32 surface. */
#define _GNU_SOURCE
#include "host.h"
#include "dplay.h"
#include "input.h"
#include "display.h"
#include <ctype.h>
#include <errno.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/socket.h>

/* ============================================================== registry */
/* This installation starts with no registry values. Missing keys follow Win32 errors. */
#define HKEY_CLASSES_ROOT_ 0x80000000u
#define HKEY_CURRENT_USER_ 0x80000001u
#define HKEY_LOCAL_MACHINE_ 0x80000002u
#define REG_SZ_ 1u
#define REG_DWORD_ 4u

typedef struct { const char *key; const char *name; uint32_t type; uint32_t dword; const char *text; } reg_value;
static const reg_value registry[] = {};
static const char *registry_keys[] = {};

typedef struct { char path[256]; } regkey_t;
static void regkey_destroy(void_object *o) { (void)o; }

static const char *root_name(uint32_t root)
{
    switch (root) {
    case HKEY_CLASSES_ROOT_: return "HKCR";
    case HKEY_CURRENT_USER_: return "HKCU";
    case HKEY_LOCAL_MACHINE_: return "HKLM";
    default: return NULL;
    }
}

static uint32_t reg_open(uint32_t root, const char *sub, uint32_t out)
{
    char path[256];
    const char *base = root_name(root);
    if (!base) {
        void_object *o = void_handle_get(root, VOID_OBJ_OTHER);
        if (!o) return ERROR_INVALID_HANDLE_;
        base = ((regkey_t *)o->data)->path;
    }
    snprintf(path, sizeof path, "%s%s%s", base, sub && *sub ? "\\" : "", sub ? sub : "");
    /* WOW64 view: Software\WOW6432Node\X is Software\X for a 32-bit process. */
    char *wow = strcasestr(path, "\\WOW6432Node");
    if (wow) memmove(wow, wow + 12, strlen(wow + 12) + 1);
    for (size_t i = 0; i < sizeof registry_keys / sizeof registry_keys[0]; ++i) {
        if (!strcasecmp(registry_keys[i], path)) {
            void_object *o = void_object_new(VOID_OBJ_OTHER, sizeof(regkey_t));
            o->destroy = regkey_destroy;
            snprintf(((regkey_t *)o->data)->path, sizeof ((regkey_t *)o->data)->path, "%s", registry_keys[i]);
            MEM32(out) = void_handle_new(o);
            VLOG("[void] RegOpenKey(%s) = %08X", path, MEM32(out));
            return ERROR_SUCCESS_;
        }
    }
    VLOG("[void] RegOpenKey(%s): not found", path);
    return ERROR_FILE_NOT_FOUND_;
}

static uint32_t reg_query(uint32_t key, const char *name, uint32_t type_out, uint32_t data, uint32_t size_ptr, int wide)
{
    void_object *o = void_handle_get(key, VOID_OBJ_OTHER);
    if (!o) return ERROR_INVALID_HANDLE_;
    const char *path = ((regkey_t *)o->data)->path;
    for (size_t i = 0; i < sizeof registry / sizeof registry[0]; ++i) {
        const reg_value *v = &registry[i];
        if (strcasecmp(v->key, path) || strcasecmp(v->name, name ? name : "")) continue;
        uint32_t need = v->type == REG_DWORD_ ? 4u : (uint32_t)((strlen(v->text) + 1) * (wide ? 2 : 1));
        if (type_out) MEM32(type_out) = v->type;
        if (!size_ptr) return data ? ERROR_INVALID_PARAMETER_ : ERROR_SUCCESS_;
        uint32_t cap = MEM32(size_ptr);
        MEM32(size_ptr) = need;
        if (!data) return ERROR_SUCCESS_;
        if (cap < need) return ERROR_MORE_DATA_;
        if (v->type == REG_DWORD_) MEM32(data) = v->dword;
        else if (wide) void_utf8_to_gw(v->text, data, need / 2);
        else void_utf8_to_ga(v->text, data, need);
        VLOG("[void] RegQueryValue(%s\\%s) served", path, name);
        return ERROR_SUCCESS_;
    }
    VLOG("[void] RegQueryValue(%s\\%s): not found", path, name ? name : "");
    return ERROR_FILE_NOT_FOUND_;
}

#define ADV "advapi32.dll"
WINAPI_EXPORT(ADV, RegOpenKeyExW, 5)
{
    char *sub = void_wstr_utf8(ARG(1));
    uint32_t r = reg_open(ARG(0), sub, ARG(4));
    free(sub);
    return r;
}
WINAPI_EXPORT(ADV, RegOpenKeyExA, 5)
{
    char *sub = void_astr_utf8(ARG(1));
    uint32_t r = reg_open(ARG(0), sub, ARG(4));
    free(sub);
    return r;
}
WINAPI_EXPORT(ADV, RegOpenKeyA, 3)
{
    char *sub = void_astr_utf8(ARG(1));
    uint32_t r = reg_open(ARG(0), sub, ARG(2));
    free(sub);
    return r;
}
WINAPI_EXPORT(ADV, RegQueryValueExW, 6)
{
    char *name = void_wstr_utf8(ARG(1));
    uint32_t r = reg_query(ARG(0), name, ARG(3), ARG(4), ARG(5), 1);
    free(name);
    return r;
}
WINAPI_EXPORT(ADV, RegQueryValueExA, 6)
{
    char *name = void_astr_utf8(ARG(1));
    uint32_t r = reg_query(ARG(0), name, ARG(3), ARG(4), ARG(5), 0);
    free(name);
    return r;
}
WINAPI_EXPORT(ADV, RegCloseKey, 1)
{
    if (root_name(ARG(0))) return ERROR_SUCCESS_;
    return void_handle_close(ARG(0)) ? ERROR_SUCCESS_ : ERROR_INVALID_HANDLE_;
}
WINAPI_EXPORT(ADV, InitializeSecurityDescriptor, 2)
{
    memset(GPTR(ARG(0)), 0, 20);
    MEM8(ARG(0)) = 1;                    /* SECURITY_DESCRIPTOR_REVISION */
    return 1;
}
WINAPI_EXPORT(ADV, SetSecurityDescriptorDacl, 4)
{
    MEM16(ARG(0) + 2) |= 4;              /* SE_DACL_PRESENT */
    MEM32(ARG(0) + 16) = ARG(2);
    return 1;
}

/* ================================================================ shell32 */
#define SH "shell32.dll"
static const char *folder_path(uint32_t csidl)
{
    switch (csidl & 0xFFu) {
    case 0x05: return "C:\\Users\\Player\\Documents";          /* CSIDL_PERSONAL */
    case 0x1A: return "C:\\Users\\Player\\AppData\\Roaming";   /* CSIDL_APPDATA */
    case 0x1C: return "C:\\Users\\Player\\AppData\\Local";     /* CSIDL_LOCAL_APPDATA */
    case 0x23: return "C:\\ProgramData";                       /* CSIDL_COMMON_APPDATA */
    case 0x2E: return "C:\\Users\\Public\\Documents";          /* CSIDL_COMMON_DOCUMENTS */
    case 0x28: return "C:\\Users\\Player";                     /* CSIDL_PROFILE */
    case 0x24: return "C:\\Windows";                           /* CSIDL_WINDOWS */
    case 0x25: return "C:\\Windows\\system32";                 /* CSIDL_SYSTEM */
    case 0x26: return "C:\\Program Files";                     /* CSIDL_PROGRAM_FILES */
    default: return NULL;
    }
}

WINAPI_EXPORT(SH, SHGetFolderPathW, 5)
{
    const char *path = folder_path(ARG(1));
    VLOG("[void] SHGetFolderPathW(%X) = %s", ARG(1), path ? path : "(unsupported)");
    if (!path) return E_INVALIDARG_;
    char *host = void_path_to_host(path);
    if (host) { mkdir(host, 0775); free(host); }
    void_utf8_to_gw(path, ARG(4), 260);
    return S_OK_;
}

WINAPI_EXPORT(SH, SHGetFolderPathA, 5)
{
    const char *path = folder_path(ARG(1));
    if (!path) return E_INVALIDARG_;
    char *host = void_path_to_host(path);
    if (host) { mkdir(host, 0775); free(host); }
    void_utf8_to_ga(path, ARG(4), 260);
    return S_OK_;
}

WINAPI_EXPORT(SH, ShellExecuteA, 6)
{
    char *file = void_astr_utf8(ARG(2));
    void_log("[shell32] ShellExecuteA(%s): no Android association", file ? file : "");
    free(file);
    void_set_last_error(ERROR_NOT_SUPPORTED_);
    return 31; /* SE_ERR_NOASSOC */
}

/* ================================================================ shlwapi */
#define SHL "shlwapi.dll"
/* Path helpers on guest strings; `w` selects UTF-16 (2) or ANSI (1) units. */
static uint32_t unit_at(uint32_t s, uint32_t i, int w) { return w == 2 ? MEM16(s + 2 * i) : MEM8(s + i); }
static void unit_put(uint32_t s, uint32_t i, uint32_t c, int w) { if (w == 2) MEM16(s + 2 * i) = (uint16_t)c; else MEM8(s + i) = (uint8_t)c; }
static uint32_t units_len(uint32_t s, int w) { uint32_t n = 0; while (unit_at(s, n, w)) ++n; return n; }

static char *read_units(uint32_t s, int w) { return w == 2 ? void_wstr_utf8(s) : void_astr_utf8(s); }
static uint32_t write_units(const char *text, uint32_t dst, uint32_t cap, int w)
{
    return w == 2 ? void_utf8_to_gw(text, dst, cap) : void_utf8_to_ga(text, dst, cap);
}

/* PathCanonicalize-style "." / ".." removal, keeping the drive or root. */
static void canonicalize(char *path)
{
    char out[1024];
    size_t n = 0;
    char *parts[128];
    int count = 0;
    int rooted = 0;
    char prefix[8] = "";
    char *p = path;
    if (((p[0] | 0x20) >= 'a' && (p[0] | 0x20) <= 'z') && p[1] == ':') {
        prefix[0] = p[0]; prefix[1] = ':'; prefix[2] = 0;
        p += 2;
    }
    if (*p == '\\') { rooted = 1; ++p; }
    char *save = NULL;
    for (char *tok = strtok_r(p, "\\", &save); tok; tok = strtok_r(NULL, "\\", &save)) {
        if (!strcmp(tok, ".")) continue;
        if (!strcmp(tok, "..")) { if (count) --count; continue; }
        if (count < 128) parts[count++] = tok;
    }
    n += (size_t)snprintf(out + n, sizeof out - n, "%s%s", prefix, rooted ? "\\" : "");
    for (int i = 0; i < count; ++i) n += (size_t)snprintf(out + n, sizeof out - n, "%s%s", i ? "\\" : "", parts[i]);
    strcpy(path, out);
}

static uint32_t path_combine(int w)
{
    uint32_t dst = ARG(0);
    char *dir = read_units(ARG(1), w), *file = read_units(ARG(2), w);
    char combined[1024];
    if (!dir && !file) { free(dir); free(file); return 0; }
    if (!file || !*file) snprintf(combined, sizeof combined, "%s", dir ? dir : "");
    else if (!dir || !*dir || file[0] == '\\' || (file[0] && file[1] == ':'))
        snprintf(combined, sizeof combined, "%s", file);
    else
        snprintf(combined, sizeof combined, "%s%s%s", dir, dir[strlen(dir) - 1] == '\\' ? "" : "\\", file);
    for (char *c = combined; *c; ++c) if (*c == '/') *c = '\\';
    canonicalize(combined);
    free(dir); free(file);
    if (strlen(combined) >= 260) { unit_put(dst, 0, 0, w); return 0; }
    write_units(combined, dst, 260, w);
    return dst;
}

WINAPI_EXPORT(SHL, PathCombineW, 3) { return path_combine(2); }
WINAPI_EXPORT(SHL, PathCombineA, 3) { return path_combine(1); }

static uint32_t find_extension(uint32_t s, int w)
{
    uint32_t n = units_len(s, w), dot = n;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t c = unit_at(s, i, w);
        if (c == '.') dot = i;
        else if (c == '\\' || c == ' ') dot = n;
    }
    return s + dot * (uint32_t)w;
}

static uint32_t find_file_name(uint32_t s, int w)
{
    uint32_t n = units_len(s, w), start = 0;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t c = unit_at(s, i, w);
        if ((c == '\\' || c == ':' || c == '/') && i + 1 < n) start = i + 1;
    }
    return s + start * (uint32_t)w;
}

WINAPI_EXPORT(SHL, PathFindExtensionW, 1) { return find_extension(ARG(0), 2); }
WINAPI_EXPORT(SHL, PathFindExtensionA, 1) { return find_extension(ARG(0), 1); }
WINAPI_EXPORT(SHL, PathFindFileNameW, 1) { return find_file_name(ARG(0), 2); }
WINAPI_EXPORT(SHL, PathFindFileNameA, 1) { return find_file_name(ARG(0), 1); }

WINAPI_EXPORT(SHL, PathRemoveExtensionW, 1)
{
    uint32_t ext = find_extension(ARG(0), 2);
    if (MEM16(ext)) MEM16(ext) = 0;
    return 0;
}

WINAPI_EXPORT(SHL, PathRenameExtensionW, 2)
{
    uint32_t s = ARG(0), ext = find_extension(s, 2);
    uint32_t base_units = (ext - s) / 2, add = units_len(ARG(1), 2);
    if (base_units + add >= 260) return 0;
    for (uint32_t i = 0; i <= add; ++i) MEM16(ext + 2 * i) = MEM16(ARG(1) + 2 * i);
    return 1;
}

static uint32_t remove_file_spec(uint32_t s, int w)
{
    uint32_t n = units_len(s, w);
    int32_t last = -1;
    for (uint32_t i = 0; i < n; ++i) if (unit_at(s, i, w) == '\\') last = (int32_t)i;
    if (last < 0) {
        if (n) { unit_put(s, 0, 0, w); return 1; }
        return 0;
    }
    /* Keep the root separator: "C:\file" -> "C:\". */
    if (last == 2 && unit_at(s, 1, w) == ':') { if (n > 3) { unit_put(s, 3, 0, w); return 1; } return 0; }
    if (last == 0) { if (n > 1) { unit_put(s, 1, 0, w); return 1; } return 0; }
    unit_put(s, (uint32_t)last, 0, w);
    return 1;
}

WINAPI_EXPORT(SHL, PathRemoveFileSpecA, 1) { return remove_file_spec(ARG(0), 1); }
WINAPI_EXPORT(SHL, PathRemoveFileSpecW, 1) { return remove_file_spec(ARG(0), 2); }

/* PathRelativePathTo(out, from, fromAttr, to, toAttr). */
WINAPI_EXPORT(SHL, PathRelativePathToW, 5)
{
    char *from = void_wstr_utf8(ARG(1)), *to = void_wstr_utf8(ARG(3));
    uint32_t from_dir = ARG(2) & 0x10u;
    if (!from || !to) { free(from); free(to); return 0; }
    for (char *c = from; *c; ++c) if (*c == '/') *c = '\\';
    for (char *c = to; *c; ++c) if (*c == '/') *c = '\\';
    if (!from_dir) { char *s = strrchr(from, '\\'); if (s) *s = 0; }
    /* Common prefix by components, case-insensitively. */
    char a[1024], b[1024];
    snprintf(a, sizeof a, "%s\\", from);
    snprintf(b, sizeof b, "%s", to);
    size_t common = 0, i = 0;
    while (a[i] && b[i] && tolower((unsigned char)a[i]) == tolower((unsigned char)b[i])) {
        if (a[i] == '\\') common = i + 1;
        ++i;
    }
    if (!a[i] && (b[i] == '\\' || !b[i])) common = i;
    if (!common) { free(from); free(to); return 0; }
    char out[1024] = "";
    size_t n = 0;
    int ups = 0;
    for (size_t k = common; a[k]; ++k) if (a[k] == '\\') ++ups;
    if (!ups) n += (size_t)snprintf(out + n, sizeof out - n, ".");
    for (int k = 0; k < ups; ++k) n += (size_t)snprintf(out + n, sizeof out - n, "%s..", k ? "\\" : "");
    const char *rest = b + common;
    while (*rest == '\\') ++rest;
    if (*rest) n += (size_t)snprintf(out + n, sizeof out - n, "\\%s", rest);
    void_utf8_to_gw(out, ARG(0), 260);
    free(from); free(to);
    return 1;
}

/* ================================================================== winmm */
#define WINMM "winmm.dll"
extern int void_audio_is_paused(void) __attribute__((weak));
extern uint32_t void_audio_clock_ms(void) __attribute__((weak));
WINAPI_EXPORT(WINMM, timeGetTime, 0) { return void_audio_clock_ms ? void_audio_clock_ms() : void_tick_ms(); }
WINAPI_EXPORT(WINMM, timeGetDevCaps, 2)
{
    if (!ARG(0) || ARG(1) < 8) return 97; /* TIMERR_NOCANDO */
    MEM32(ARG(0)) = 1;
    MEM32(ARG(0) + 4) = 65535;
    return 0;
}
WINAPI_EXPORT(WINMM, timeBeginPeriod, 1) { return 0; }
WINAPI_EXPORT(WINMM, timeEndPeriod, 1) { return 0; }

typedef struct {
    uint32_t id, delay, callback, user, flags;
    int active, running;
    pthread_t thread;
} mmtimer_t;
static mmtimer_t mm_timers[16];
static pthread_mutex_t mm_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t mm_cond;
static pthread_once_t mm_once = PTHREAD_ONCE_INIT;
static uint32_t next_mm_id = 1;

static void mm_init(void)
{
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    if (pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) || pthread_cond_init(&mm_cond, &attr))
        void_fatal("multimedia timer condition initialization failed");
    pthread_condattr_destroy(&attr);
}

static void *mm_timer_thread(void *arg)
{
    mmtimer_t *t = (mmtimer_t *)arg;
    uint64_t period = (uint64_t)t->delay * 1000000u;
    uint64_t next = void_monotonic_ns() + period;
    uint64_t report = next, max_late = 0;
    uint32_t callbacks = 0, coalesced = 0;
    pthread_mutex_lock(&mm_lock);
    while (t->active) {
        struct timespec deadline = { (time_t)(next / 1000000000u), (long)(next % 1000000000u) };
        int result = 0;
        while (t->active && result != ETIMEDOUT) {
            result = pthread_cond_timedwait(&mm_cond, &mm_lock, &deadline);
            if (result && result != ETIMEDOUT) { t->active = 0; break; }
        }
        if (!t->active) break;
        pthread_mutex_unlock(&mm_lock);
        if (void_audio_is_paused && void_audio_is_paused()) {
            pthread_mutex_lock(&mm_lock);
            next = void_monotonic_ns() + period;
            continue;
        }
        if (t->flags & 0x30u) {                 /* TIME_CALLBACK_EVENT_SET / PULSE */
            void_object *o = void_handle_get(t->callback, VOID_OBJ_EVENT);
            if (o) void_object_signal(o);
        } else {
            void_machine_enter();
            pthread_mutex_lock(&mm_lock);
            int active = t->active;
            pthread_mutex_unlock(&mm_lock);
            if (active && !(void_audio_is_paused && void_audio_is_paused())) {
                uint64_t actual = void_monotonic_ns();
                if (actual > next && actual - next > max_late) max_late = actual - next;
                uint32_t args[5] = { t->id, 0, t->user, 0, 0 };
                void_call_guest(t->callback, 5, args, 1);
                ++callbacks;
            }
            void_machine_leave();
        }
        pthread_mutex_lock(&mm_lock);
        if (!(t->flags & 1u)) t->active = 0; /* TIME_ONESHOT */
        next += period;
        uint64_t now = void_monotonic_ns();
        /* Coalesce missed periods after a slow guest callback; do not burst
         * through stale audio service calls faster than the guest can fill. */
        if (next <= now) {
            uint64_t skipped = (now - next) / period + 1;
            coalesced += (uint32_t)skipped; next += skipped * period;
        }
        if (now - report >= 10000000000ull) {
            void_log("[winmm] timer %u/%u ms: callbacks %u, coalesced %u, max machine/scheduler delay %.1f ms",
                t->id, t->delay, callbacks, coalesced, max_late / 1e6);
            callbacks = coalesced = 0; max_late = 0; report = now;
        }
    }
    t->running = 0;
    pthread_cond_broadcast(&mm_cond);
    pthread_mutex_unlock(&mm_lock);
    return NULL;
}

WINAPI_EXPORT(WINMM, timeSetEvent, 5)
{
    pthread_once(&mm_once, mm_init);
    pthread_mutex_lock(&mm_lock);
    for (uint32_t i = 0; i < 16; ++i) {
        mmtimer_t *t = &mm_timers[i];
        if (t->running) continue;
        t->id = next_mm_id++; if (!next_mm_id) next_mm_id = 1;
        t->delay = ARG(0) ? ARG(0) : 1; t->callback = ARG(2); t->user = ARG(3); t->flags = ARG(4);
        t->active = t->running = 1;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 8u << 20);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        int error = pthread_create(&t->thread, &attr, mm_timer_thread, t);
        pthread_attr_destroy(&attr);
        if (error) { t->active = t->running = 0; pthread_mutex_unlock(&mm_lock); return 0; }
        pthread_mutex_unlock(&mm_lock);
        VLOG("[void] timeSetEvent(%u ms, %08X, flags %X) = %u", t->delay, t->callback, t->flags, t->id);
        return t->id;
    }
    pthread_mutex_unlock(&mm_lock);
    return 0;
}

WINAPI_EXPORT(WINMM, timeKillEvent, 1)
{
    uint32_t id = ARG(0);
    pthread_once(&mm_once, mm_init);
    pthread_mutex_lock(&mm_lock);
    mmtimer_t *t = NULL;
    for (size_t i = 0; i < sizeof mm_timers / sizeof mm_timers[0]; ++i)
        if (mm_timers[i].id == id && mm_timers[i].active) { t = &mm_timers[i]; break; }
    if (!t) { pthread_mutex_unlock(&mm_lock); return 97; } /* TIMERR_NOCANDO */
    t->active = 0;
    int synchronous = (t->flags & 0x100u) && !pthread_equal(t->thread, pthread_self());
    pthread_cond_broadcast(&mm_cond);
    pthread_mutex_unlock(&mm_lock);
    if (synchronous) {
        void_blocking_begin();
        pthread_mutex_lock(&mm_lock);
        while (t->id == id && t->running) pthread_cond_wait(&mm_cond, &mm_lock);
        pthread_mutex_unlock(&mm_lock);
        void_blocking_end();
    }
    return 0;
}

/* ============================================================== ole32 */
#define OLE "ole32.dll"
WINAPI_EXPORT(OLE, CoInitialize, 1) { return S_OK_; }
WINAPI_EXPORT(OLE, CoInitializeEx, 2) { return S_OK_; }
WINAPI_EXPORT(OLE, CoUninitialize, 0) { return 0; }
WINAPI_EXPORT(OLE, CoFreeUnusedLibraries, 0) { return 0; }
WINAPI_EXPORT(OLE, CoTaskMemAlloc, 1) { return void_heap_alloc(void_process_heap(), ARG(0) ? ARG(0) : 1, 0); }
WINAPI_EXPORT(OLE, CoTaskMemFree, 1) { if (ARG(0)) void_heap_free(void_process_heap(), ARG(0)); return 0; }

static void format_guid(uint32_t g, char *out)
{
    sprintf(out, "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", MEM32(g), MEM16(g + 4), MEM16(g + 6),
        MEM8(g + 8), MEM8(g + 9), MEM8(g + 10), MEM8(g + 11), MEM8(g + 12), MEM8(g + 13), MEM8(g + 14), MEM8(g + 15));
}

WINAPI_EXPORT(OLE, CoCreateInstance, 5)
{
    uint32_t network=stronghold_dplay_create(ARG(0),ARG(3),ARG(4));
    if(network!=REGDB_E_CLASSNOTREG_)return network;
    char clsid[48], iid[48];
    format_guid(ARG(0), clsid);
    format_guid(ARG(3), iid);
    void_log("[void] CoCreateInstance(%s, iid %s) from %08X: class not registered", clsid, iid, g_cur_func);
    if (ARG(4)) MEM32(ARG(4)) = 0;
    return REGDB_E_CLASSNOTREG_;
}

WINAPI_EXPORT(OLE, CoCreateGuid, 1)
{
    uint32_t g = ARG(0);
    for (int i = 0; i < 16; ++i) MEM8(g + (uint32_t)i) = (uint8_t)(rand() & 0xFF);
    MEM8(g + 7) = (uint8_t)((MEM8(g + 7) & 0x0F) | 0x40);
    MEM8(g + 8) = (uint8_t)((MEM8(g + 8) & 0x3F) | 0x80);
    return S_OK_;
}

#define OLEAUT "oleaut32.dll"
WINAPI_EXPORT_AS(OLEAUT, VariantInit, "VariantInit", 8, 1) { memset(GPTR(ARG(0)), 0, 16); return 0; }
WINAPI_EXPORT_AS(OLEAUT, VariantClear, "VariantClear", 9, 1) { memset(GPTR(ARG(0)), 0, 16); return S_OK_; }

/* ================================================================ dbghelp */
#define DBG "dbghelp.dll"
WINAPI_EXPORT(DBG, SymInitialize, 3) { return 0; }
WINAPI_EXPORT(DBG, SymCleanup, 1) { return 1; }
WINAPI_EXPORT(DBG, SymGetOptions, 0) { return 0; }
WINAPI_EXPORT(DBG, SymSetOptions, 1) { return ARG(0); }
WINAPI_EXPORT(DBG, SymFromAddr, 4) { return 0; }
WINAPI_EXPORT(DBG, SymGetLineFromAddr64, 5) { return 0; }
WINAPI_EXPORT(DBG, SymGetModuleBase64, 3) { g_edx = 0; return 0; }
WINAPI_EXPORT(DBG, SymGetModuleInfo64, 4) { return 0; }
WINAPI_EXPORT(DBG, SymFunctionTableAccess64, 3) { return 0; }
WINAPI_EXPORT(DBG, SymEnumSymbols, 6) { return 0; }
WINAPI_EXPORT(DBG, SymEnumerateModules64, 3) { return 0; }
WINAPI_EXPORT(DBG, SymGetTypeInfo, 6) { return 0; }
WINAPI_EXPORT(DBG, SymSetContext, 3) { return 0; }
WINAPI_EXPORT(DBG, StackWalk64, 9) { return 0; }
WINAPI_EXPORT(DBG, MiniDumpWriteDump, 7)
{
    void_log("[void] MiniDumpWriteDump requested by %08X (unavailable)", g_cur_func);
    void_dump_state("guest crash handler requested a minidump");
    return 0;
}

/* ================================================================== gdi32 */
#define GDI "gdi32.dll"
WINAPI_EXPORT(GDI, GetDeviceCaps, 2)
{
    int w, h;
    void_input_screen_size(&w, &h);
    switch (ARG(1)) {
    case 0: return 0x400; /* DRIVERVERSION */
    case 2: return 1; /* DT_RASDISPLAY */
    case 4: return (uint32_t)(w * 254 / 960); /* HORZSIZE mm at 96dpi */
    case 6: return (uint32_t)(h * 254 / 960);
    case 8: case 118: return (uint32_t)w;
    case 10: case 117: return (uint32_t)h;
    case 12: return 16; /* BITSPIXEL: DirectDraw RGB565 */
    case 14: return 1;
    case 24: return 0xFFFFFFFFu; /* NUMCOLORS: true color */
    case 88: case 90: return 96;
    case 116: return 60;
    default: return 0;
    }
}
/* shortcut: GDI video surfaces are unavailable; implement if Bink chooses its GDI backend. */
WINAPI_EXPORT(GDI, GetDIBits, 7) { void_set_last_error(ERROR_NOT_SUPPORTED_); return 0; }
WINAPI_EXPORT(GDI, SetStretchBltMode, 2) { void_set_last_error(ERROR_NOT_SUPPORTED_); return 0; }
WINAPI_EXPORT(GDI, StretchBlt, 11) { void_set_last_error(ERROR_NOT_SUPPORTED_); return 0; }
WINAPI_EXPORT(GDI, CreateCompatibleDC, 1) { void_set_last_error(ERROR_NOT_SUPPORTED_); return 0; }
WINAPI_EXPORT(GDI, CreateCompatibleBitmap, 3) { void_set_last_error(ERROR_NOT_SUPPORTED_); return 0; }
WINAPI_EXPORT(GDI, CreateDIBSection, 6)
{
    if (ARG(3)) MEM32(ARG(3)) = 0;
    void_set_last_error(ERROR_NOT_SUPPORTED_); return 0;
}
WINAPI_EXPORT(GDI, DeleteDC, 1) { void_set_last_error(ERROR_INVALID_HANDLE_); return 0; }
WINAPI_EXPORT(GDI, GetPixel, 3) { return 0xFFFFFFFFu; }
WINAPI_EXPORT(GDI, SetPixel, 4) { return 0xFFFFFFFFu; }
WINAPI_EXPORT(GDI, GetStockObject, 1) { return 0x00050000u + (ARG(0) & 0xFFu); }
typedef struct { uint32_t handle; int height, weight, italic; } gdi_font;
typedef struct { uint32_t handle, font, color, background; int mode; } gdi_dc;
static gdi_font gdi_fonts[128];
static gdi_dc gdi_dcs[64];
static uint32_t next_font = 0x60000u;
extern int void_ddraw_dc_pixels(uint32_t dc, uint32_t *dst, int *w, int *h, int *pitch, int *bpp);
extern int void_android_text(uint32_t dst, int w, int h, int pitch, int bpp, const uint16_t *text,
    int count, int size, int weight, int italic, int x, int y, uint32_t color, uint32_t bg, int opaque,
    int *extent_w, int *extent_h);

static gdi_dc *gdi_dc_get(uint32_t dc)
{
    if (!dc) return NULL;
    for (size_t i = 0; i < sizeof gdi_dcs / sizeof gdi_dcs[0]; ++i)
        if (gdi_dcs[i].handle == dc) return &gdi_dcs[i];
    for (size_t i = 0; i < sizeof gdi_dcs / sizeof gdi_dcs[0]; ++i)
        if (!gdi_dcs[i].handle) {
            gdi_dcs[i] = (gdi_dc){ dc, 0x5000Du, 0, 0xFFFFFFu, 2 };
            return &gdi_dcs[i];
        }
    void_set_last_error(ERROR_NOT_ENOUGH_MEMORY_);
    return NULL;
}

static gdi_font *gdi_font_get(uint32_t handle)
{
    static gdi_font stock = { 0, 16, 400, 0 };
    if (!handle) return NULL;
    if (handle >= 0x50000u && handle <= 0x50013u) return &stock;
    for (size_t i = 0; i < sizeof gdi_fonts / sizeof gdi_fonts[0]; ++i)
        if (gdi_fonts[i].handle == handle) return &gdi_fonts[i];
    return NULL;
}

WINAPI_EXPORT(GDI, DeleteObject, 1)
{
    uint32_t handle = ARG(0);
    if (handle >= 0x50000u && handle <= 0x50013u) return 1;
    gdi_font *font = gdi_font_get(handle);
    if (!font) return 0;
    for (size_t i = 0; i < sizeof gdi_dcs / sizeof gdi_dcs[0]; ++i)
        if (gdi_dcs[i].handle && gdi_dcs[i].font == handle) return 0;
    memset(font, 0, sizeof *font);
    return 1;
}
WINAPI_EXPORT(GDI, SelectObject, 2)
{
    gdi_dc *dc = gdi_dc_get(ARG(0));
    if (!dc || !gdi_font_get(ARG(1))) { void_set_last_error(ERROR_INVALID_HANDLE_); return 0; }
    uint32_t old = dc->font; dc->font = ARG(1); return old;
}
WINAPI_EXPORT(GDI, SetBkColor, 2)
{
    gdi_dc *dc = gdi_dc_get(ARG(0)); if (!dc) return 0xFFFFFFFFu;
    uint32_t old = dc->background; dc->background = ARG(1); return old;
}
WINAPI_EXPORT(GDI, SetTextColor, 2)
{
    gdi_dc *dc = gdi_dc_get(ARG(0)); if (!dc) return 0xFFFFFFFFu;
    uint32_t old = dc->color; dc->color = ARG(1); return old;
}
WINAPI_EXPORT(GDI, SetBkMode, 2)
{
    gdi_dc *dc = gdi_dc_get(ARG(0));
    if (!dc || (ARG(1) != 1 && ARG(1) != 2)) return 0;
    int old = dc->mode; dc->mode = (int)ARG(1); return (uint32_t)old;
}
WINAPI_EXPORT(GDI, CreateFontA, 14)
{
    int32_t height = (int32_t)ARG(0);
    if (height == INT32_MIN || height < -1024 || height > 1024) { void_set_last_error(ERROR_INVALID_PARAMETER_); return 0; }
    for (size_t i = 0; i < sizeof gdi_fonts / sizeof gdi_fonts[0]; ++i)
        if (!gdi_fonts[i].handle) {
            gdi_fonts[i] = (gdi_font){ next_font++, height ? height : 16, (int)ARG(4), ARG(5) != 0 };
            return gdi_fonts[i].handle;
        }
    void_set_last_error(ERROR_NOT_ENOUGH_MEMORY_);
    return 0;
}

static int gdi_measure(gdi_dc *dc, const uint16_t *text, uint32_t count, int *width, int *height)
{
    gdi_font *font = dc ? gdi_font_get(dc->font) : NULL;
    return font && void_android_text(0, 0, 0, 0, 0, text, (int)count, font->height,
        font->weight, font->italic, 0, 0, dc->color, dc->background, 0, width, height);
}

static uint16_t *gdi_text(uint32_t guest, uint32_t count, int wide)
{
    if (count > 65535u || (count && (!guest || !void_vm_readable(guest, count * (wide ? 2 : 1))))) return NULL;
    uint16_t *text = (uint16_t *)malloc((count + 1u) * sizeof *text);
    if (!text) return NULL;
    for (uint32_t i = 0; i < count; ++i) text[i] = wide ? MEM16(guest + 2 * i) : void_cp1251_to_u16(MEM8(guest + i));
    text[count] = 0;
    return text;
}

static uint32_t gdi_text_extent(int wide)
{
    gdi_dc *dc = gdi_dc_get(ARG(0));
    uint32_t count = ARG(2), out = ARG(3);
    uint16_t *text = gdi_text(ARG(1), count, wide);
    if (!text || !out || !void_vm_writable(out, 8)) { free(text); void_set_last_error(ERROR_INVALID_PARAMETER_); return 0; }
    int width, height;
    int ok = gdi_measure(dc, text, count, &width, &height);
    free(text);
    if (!ok) { void_set_last_error(ERROR_NOT_SUPPORTED_); return 0; }
    MEM32(out) = (uint32_t)width; MEM32(out + 4) = (uint32_t)height;
    return 1;
}
WINAPI_EXPORT(GDI, GetTextExtentPoint32W, 4) { return gdi_text_extent(1); }
WINAPI_EXPORT(GDI, GetTextExtentPoint32A, 4) { return gdi_text_extent(0); }
WINAPI_EXPORT(GDI, GetTextExtentExPointW, 7)
{
    uint32_t n = ARG(2), max_extent = ARG(3), fit = ARG(4), dx = ARG(5), size = ARG(6), count = 0;
    gdi_dc *dc = gdi_dc_get(ARG(0));
    uint16_t *text = gdi_text(ARG(1), n, 1);
    if (!text || !size || !void_vm_writable(size, 8) || (fit && !void_vm_writable(fit, 4)) ||
        (dx && !void_vm_writable(dx, n * 4))) { free(text); return 0; }
    int width = 0, height = 0;
    if (!gdi_measure(dc, text, n, &width, &height)) { free(text); return 0; }
    MEM32(size) = (uint32_t)width; MEM32(size + 4) = (uint32_t)height;
    for (uint32_t i = 0; i < n; ++i) {
        if (!gdi_measure(dc, text, i + 1, &width, &height)) { free(text); return 0; }
        if (dx) MEM32(dx + 4 * i) = (uint32_t)width;
        if (width >= 0 && (uint32_t)width <= max_extent) count = i + 1;
    }
    if (fit) MEM32(fit) = count;
    free(text);
    return 1;
}

static uint16_t gdi_rgb565(uint32_t color)
{
    return (uint16_t)(((color & 0xF8u) << 8) | ((color & 0xFC00u) >> 5) | ((color & 0xF80000u) >> 19));
}

static uint32_t gdi_text_out(int wide)
{
    gdi_dc *dc = gdi_dc_get(ARG(0));
    gdi_font *font = dc ? gdi_font_get(dc->font) : NULL;
    uint32_t dst = 0, options = ARG(3), rect = ARG(4), n = ARG(6), dx = ARG(7);
    int w, h, pitch, bpp, width, height, x = (int32_t)ARG(1), y = (int32_t)ARG(2);
    uint16_t *text = gdi_text(ARG(5), n, wide);
    if (!font || !text || (options & ~6u) || !void_ddraw_dc_pixels(ARG(0), &dst, &w, &h, &pitch, &bpp) ||
        bpp != 16 || (dx && !void_vm_readable(dx, n * 4))) {
        free(text); void_set_last_error(ERROR_NOT_SUPPORTED_); return 0;
    }
    if ((options & 6u) && rect) {
        if (!void_vm_readable(rect, 16)) { free(text); return 0; }
        int left = (int32_t)MEM32(rect), top = (int32_t)MEM32(rect + 4);
        int right = (int32_t)MEM32(rect + 8), bottom = (int32_t)MEM32(rect + 12);
        if (left < 0) left = 0; if (top < 0) top = 0;
        if (right > w) right = w; if (bottom > h) bottom = h;
        if (right <= left || bottom <= top) { free(text); return 1; }
        if (options & 2u) {
            uint16_t color = gdi_rgb565(dc->background);
            for (int yy = top; yy < bottom; ++yy) {
                for (int xx = left; xx < right; ++xx) MEM16(dst + (uint32_t)(yy * pitch + xx * 2)) = color;
                if (stronghold_display_capture_active)
                    stronghold_display_written(dst + (uint32_t)(yy * pitch + left * 2), (unsigned)(right - left) * 2);
            }
        }
        if (options & 4u) {
            dst += (uint32_t)(top * pitch + left * 2); w = right - left; h = bottom - top;
            x -= left; y -= top;
        }
    }
    int ok = 1;
    if (dx) {
        for (uint32_t i = 0; i < n && ok; ++i) {
            ok = void_android_text(dst, w, h, pitch, bpp, text + i, 1, font->height, font->weight,
                font->italic, x, y, dc->color, dc->background, dc->mode == 2, &width, &height);
            x += (int32_t)MEM32(dx + 4 * i);
        }
    } else if (n) ok = void_android_text(dst, w, h, pitch, bpp, text, (int)n, font->height, font->weight,
        font->italic, x, y, dc->color, dc->background, dc->mode == 2, &width, &height);
    free(text);
    if (!ok) void_set_last_error(ERROR_NOT_SUPPORTED_);
    return (uint32_t)ok;
}
WINAPI_EXPORT(GDI, ExtTextOutA, 8) { return gdi_text_out(0); }
WINAPI_EXPORT(GDI, ExtTextOutW, 8) { return gdi_text_out(1); }

WINAPI_ORDINAL("wsock32.dll", WSAStartup, 115, 2)
{
    if (!(ARG(0) & 0xFFu)) return 10092; /* WSAVERNOTSUPPORTED */
    if (!ARG(1)) return 10014; /* WSAEFAULT */
    uint32_t out = ARG(1);
    memset(GPTR(out), 0, 400);
    MEM16(out) = 0x0101; MEM16(out + 2) = 0x0202;
    strcpy((char *)GPTR(out + 4), "Stronghold Android sockets");
    strcpy((char *)GPTR(out + 261), "Running");
    MEM16(out + 390) = 32767; MEM16(out + 392) = 65467;
    return 0;
}
WINAPI_ORDINAL("wsock32.dll", WSACleanup, 116, 0) { return 0; }
WINAPI_EXPORT_AS("wsock32.dll", winsock_gethostname, "gethostname", 57, 2)
{
    uint32_t len = ARG(1);
    char name[256];
    if (!ARG(0) || !len || gethostname(name, sizeof name)) {
        void_set_last_error(10014); return 0xFFFFFFFFu;
    }
    name[sizeof name - 1] = 0;
    if (strlen(name) + 1 > len) { void_set_last_error(10014); return 0xFFFFFFFFu; }
    strcpy((char *)GPTR(ARG(0)), name);
    return 0;
}
WINAPI_EXPORT_AS("wsock32.dll", winsock_gethostbyname, "gethostbyname", 52, 1)
{
    if (!ARG(0)) { void_set_last_error(10014); return 0; }
    const char *requested_name=(const char *)GPTR(ARG(0));
    char hostname[256]={0},ip[64];struct in_addr local;
    int own=!gethostname(hostname,sizeof hostname)&&!strcmp(requested_name,hostname);
    struct hostent *h = own?NULL:gethostbyname(requested_name);
    if(own){stronghold_lan_address(ip);inet_pton(AF_INET,ip,&local);}
    if (!own&&(!h || h->h_addrtype != AF_INET || h->h_length != 4 || !h->h_addr_list[0])) {
        void_set_last_error(11001); return 0;
    }
    static uint32_t result;
    if (!result) result = void_galloc(1024);
    if (!result) { void_set_last_error(10055); return 0; }
    memset(GPTR(result), 0, 1024);
    MEM32(result) = result + 32; MEM32(result + 4) = result + 16;
    MEM16(result + 8) = AF_INET; MEM16(result + 10) = 4;
    MEM32(result + 12) = result + 20; MEM32(result + 20) = result + 28;
    memcpy(GPTR(result + 28), own?(const void *)&local:h->h_addr_list[0], 4);
    snprintf((char *)GPTR(result + 32), 992, "%s", own?hostname:h->h_name);
    return result;
}
