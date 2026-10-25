/* Files: Windows paths over host directories, file handles, directory
 * enumeration and file mappings.
 *
 * The guest sees one drive. GAME_INSTALL ("C:\Games\Stronghold Crusader Extreme HD",
 * or "C:\Games\Stronghold HD" for the original) is the game installation;
 * profile folders and the rest of C: are writable directories
 * under the host data root. Every component is resolved case-insensitively,
 * as NTFS lookups are. */
#define _GNU_SOURCE
#include "host.h"
#include "edition.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define WIN_MAX_PATH 260

typedef struct { const char *win; char host[1024]; } mount_t;
static mount_t mounts[8];
static int mount_count;
static char game_root[1024], data_root[1024];
static char cwd[WIN_MAX_PATH] = GAME_INSTALL;
static pthread_mutex_t cwd_lock = PTHREAD_MUTEX_INITIALIZER;

/* Asset directories are reused by Miles for every track, including absent
 * legacy names. Keep a bounded directory index, validated by its generation. */
#define CASE_DIRS 8
#define CASE_NAMES 4096
typedef struct { char *path; char **names; size_t count; int complete; struct stat stamp; } case_dir_t;
static case_dir_t case_dirs[CASE_DIRS];
static unsigned case_next;
static pthread_mutex_t case_lock = PTHREAD_MUTEX_INITIALIZER;

static void case_clear(case_dir_t *d)
{
    for (size_t i = 0; i < d->count; ++i) free(d->names[i]);
    free(d->names); free(d->path); memset(d, 0, sizeof *d);
}

static int same_directory(const struct stat *a, const struct stat *b)
{
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino &&
        a->st_mtim.tv_sec == b->st_mtim.tv_sec && a->st_mtim.tv_nsec == b->st_mtim.tv_nsec &&
        a->st_ctim.tv_sec == b->st_ctim.tv_sec && a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
}

const char *void_root_dir(void) { return game_root; }
const char *void_data_dir(void) { return data_root; }

static void mkdirs(const char *path)
{
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; ++p) {
        if (*p == '/') { *p = 0; mkdir(tmp, 0775); *p = '/'; }
    }
    mkdir(tmp, 0775);
}

static void add_mount(const char *win, const char *host_fmt, const char *base)
{
    mount_t *m = &mounts[mount_count++];
    m->win = win;
    snprintf(m->host, sizeof m->host, host_fmt, base);
    mkdirs(m->host);
}

int void_files_init(const char *game, const char *data)
{
    struct stat st;
    if (stat(game, &st) || !S_ISDIR(st.st_mode)) {
        void_log("[void] game root %s is missing", game);
        return 0;
    }
    snprintf(game_root, sizeof game_root, "%s", game);
    snprintf(data_root, sizeof data_root, "%s", data);
    pthread_mutex_lock(&case_lock);
    for (int i = 0; i < CASE_DIRS; ++i) case_clear(&case_dirs[i]);
    case_next = 0;
    pthread_mutex_unlock(&case_lock);
    mount_count = 0;
    /* Longest prefixes first. */
    add_mount("C:\\Users\\Player\\AppData\\Local\\Temp", "%s/Temp", data);
    add_mount("C:\\Users\\Player\\Documents", "%s/Documents", data);
    add_mount("C:\\Users\\Player\\AppData", "%s/AppData", data);
    add_mount("C:\\Users\\Public\\Documents", "%s/PublicDocuments", data);
    add_mount("C:\\ProgramData", "%s/ProgramData", data);
    add_mount(GAME_INSTALL, "%s", game);
    add_mount("C:", "%s/drive_c", data);
    char profile[1200];
    snprintf(profile, sizeof profile, "%s/Documents/" GAME_PROFILE "/" GAME_SAVES, data);
    mkdirs(profile);
    return 1;
}

/* Normalize a Windows path to "C:\a\b" (no trailing separator, no . or ..).
 * Returns 0 if it cannot be represented. */
static int normalize(const char *in, char *out, size_t cap)
{
    char work[1024];
    size_t n = 0;
    const char *p = in;
    if (!strncmp(p, "\\\\?\\", 4)) p += 4;
    if (((p[0] | 0x20) >= 'a' && (p[0] | 0x20) <= 'z') && p[1] == ':') {
        if (p[2] == '\\' || p[2] == '/') {
            snprintf(work, sizeof work, "C:%s", p + 2);
        } else {
            pthread_mutex_lock(&cwd_lock);
            snprintf(work, sizeof work, "%s\\%s", cwd, p + 2);
            pthread_mutex_unlock(&cwd_lock);
        }
    } else if (p[0] == '\\' || p[0] == '/') {
        snprintf(work, sizeof work, "C:%s", p);
    } else {
        pthread_mutex_lock(&cwd_lock);
        snprintf(work, sizeof work, "%s\\%s", cwd, p);
        pthread_mutex_unlock(&cwd_lock);
    }
    /* Split and rebuild. */
    char *parts[128];
    int count = 0;
    for (char *s = work; *s; ++s) if (*s == '/') *s = '\\';
    char *save = NULL;
    char *tok = strtok_r(work + 2, "\\", &save);
    while (tok) {
        if (!strcmp(tok, ".") || !*tok) {
        } else if (!strcmp(tok, "..")) {
            if (count) --count;
        } else {
            /* Windows strips trailing dots and spaces from components. */
            size_t len = strlen(tok);
            while (len > 1 && (tok[len - 1] == '.' || tok[len - 1] == ' ')) tok[--len] = 0;
            if (count == 128) return 0;
            parts[count++] = tok;
        }
        tok = strtok_r(NULL, "\\", &save);
    }
    n = (size_t)snprintf(out, cap, "C:");
    for (int i = 0; i < count; ++i) {
        n += (size_t)snprintf(out + n, n < cap ? cap - n : 0, "\\%s", parts[i]);
        if (n >= cap) return 0;
    }
    if (count == 0) snprintf(out, cap, "C:\\");
    return 1;
}

/* Append component `name` to host path `base`, matching an existing entry
 * case-insensitively. Returns 1 if the entry exists. */
static int resolve_component(char *base, size_t cap, const char *name)
{
    size_t len = strlen(base);
    snprintf(base + len, cap - len, "/%s", name);
    struct stat st;
    if (!lstat(base, &st)) return 1;
    base[len] = 0;
    size_t root_len = strlen(game_root);
    int cacheable = root_len && !strncmp(base, game_root, root_len) && (base[root_len] == 0 || base[root_len] == '/');
    struct stat before;
    cacheable = cacheable && !stat(base, &before);
    if (cacheable) {
        int known = 0, found = 0;
        char actual[512];
        pthread_mutex_lock(&case_lock);
        for (int i = 0; i < CASE_DIRS; ++i) {
            case_dir_t *c = &case_dirs[i];
            if (!c->path || strcmp(c->path, base)) continue;
            if (!same_directory(&c->stamp, &before)) { case_clear(c); break; }
            known = c->complete;
            for (size_t k = 0; k < c->count; ++k) if (!strcasecmp(c->names[k], name)) {
                snprintf(actual, sizeof actual, "%s", c->names[k]); known = found = 1; break;
            }
            break;
        }
        pthread_mutex_unlock(&case_lock);
        if (known) {
            snprintf(base + len, cap - len, "/%s", found ? actual : name);
            if (!found || !lstat(base, &st)) return found;
            base[len] = 0; /* A cached child disappeared: rebuild rather than trust it. */
        }
    }
    DIR *d = opendir(len ? base : "/");
    int found = 0;
    case_dir_t index = {0};
    if (d && cacheable) {
        index.path = strdup(base); index.names = calloc(CASE_NAMES, sizeof *index.names);
        index.complete = index.path && index.names;
    }
    if (d) {
        struct dirent *e;
        for (;;) {
            errno = 0;
            e = readdir(d);
            if (!e) { if (errno) index.complete = 0; break; }
            if (index.complete && strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) {
                char *copy = index.count < CASE_NAMES ? strdup(e->d_name) : NULL;
                if (copy) index.names[index.count++] = copy; else index.complete = 0;
            }
            if (!found && !strcasecmp(e->d_name, name)) {
                snprintf(base + len, cap - len, "/%s", e->d_name);
                found = 1;
                if (!cacheable) break;
            }
        }
        closedir(d);
    }
    if (index.path && index.names) {
        struct stat after;
        if (!stat(index.path, &after) && same_directory(&before, &after)) {
            index.stamp = after;
            pthread_mutex_lock(&case_lock);
            unsigned slot = case_next++ % CASE_DIRS;
            for (int i = 0; i < CASE_DIRS; ++i) if (case_dirs[i].path && !strcmp(case_dirs[i].path, index.path)) { slot = (unsigned)i; break; }
            case_clear(&case_dirs[slot]); case_dirs[slot] = index;
            memset(&index, 0, sizeof index);
            pthread_mutex_unlock(&case_lock);
        }
    }
    case_clear(&index);
    if (!found) snprintf(base + len, cap - len, "/%s", name);
    return found;
}

/* Windows path (UTF-8) -> host path. *exists_parent reports whether every
 * component but the last exists (ERROR_PATH_NOT_FOUND vs FILE_NOT_FOUND). */
static char *to_host(const char *windows_path, int *parent_exists, char *normalized_out)
{
    char norm[1024];
    if (!windows_path || !normalize(windows_path, norm, sizeof norm)) return NULL;
    if (normalized_out) snprintf(normalized_out, 1024, "%s", norm);
    const mount_t *best = NULL;
    size_t best_len = 0;
    for (int i = 0; i < mount_count; ++i) {
        size_t l = strlen(mounts[i].win);
        if (!strncasecmp(norm, mounts[i].win, l) && (norm[l] == 0 || norm[l] == '\\') && l > best_len) {
            best = &mounts[i];
            best_len = l;
        }
    }
    if (!best) return NULL;
    char *host = (char *)malloc(2048);
    snprintf(host, 2048, "%s", best->host);
    const char *rest = norm + best_len;
    int ok = 1;
    char component[512];
    while (*rest) {
        while (*rest == '\\') ++rest;
        if (!*rest) break;
        size_t k = 0;
        while (rest[k] && rest[k] != '\\' && k < sizeof component - 1) { component[k] = rest[k]; ++k; }
        component[k] = 0;
        rest += k;
        int last = !*rest || (rest[0] == '\\' && !rest[1]);
        if (!ok) {
            size_t len = strlen(host);
            snprintf(host + len, 2048 - len, "/%s", component);
        } else {
            int exists = resolve_component(host, 2048, component);
            if (!exists && !last) ok = 0;
        }
    }
    if (parent_exists) *parent_exists = ok;
    return host;
}

char *void_path_to_host(const char *windows_path) { return to_host(windows_path, NULL, NULL); }

uint32_t void_cwd_get(char *out, uint32_t cap)
{
    pthread_mutex_lock(&cwd_lock);
    uint32_t n = (uint32_t)snprintf(out, cap, "%s", cwd);
    pthread_mutex_unlock(&cwd_lock);
    return n;
}

int void_cwd_set(const char *windows_path)
{
    char norm[1024];
    if (!normalize(windows_path, norm, sizeof norm)) return 0;
    char *host = to_host(norm, NULL, NULL);
    struct stat st;
    int ok = host && !stat(host, &st) && S_ISDIR(st.st_mode);
    free(host);
    if (!ok) return 0;
    pthread_mutex_lock(&cwd_lock);
    snprintf(cwd, sizeof cwd, "%s", norm);
    pthread_mutex_unlock(&cwd_lock);
    return 1;
}

uint32_t void_errno_to_win32(int e)
{
    switch (e) {
    case ENOENT: return ERROR_FILE_NOT_FOUND_;
    case ENOTDIR: return ERROR_PATH_NOT_FOUND_;
    case EACCES: case EPERM: case EROFS: return ERROR_ACCESS_DENIED_;
    case EEXIST: return ERROR_FILE_EXISTS_;
    case ENOMEM: return ERROR_NOT_ENOUGH_MEMORY_;
    case EBADF: return ERROR_INVALID_HANDLE_;
    case EINVAL: return ERROR_INVALID_PARAMETER_;
    case ENOSPC: return 112u;          /* ERROR_DISK_FULL */
    case ENOTEMPTY: return 145u;       /* ERROR_DIR_NOT_EMPTY */
    default: return ERROR_ACCESS_DENIED_;
    }
}

/* ----------------------------------------------------------- file objects */
typedef struct {
    int fd;
    int std_stream;       /* 0 none, 1 stdin, 2 stdout, 3 stderr */
    int is_dir;
    char *host_path;
    int delete_on_close;
    char line[512];       /* buffered guest console output */
    size_t line_len;
} file_t;

static void file_destroy(void_object *o)
{
    file_t *f = (file_t *)o->data;
    if (f->fd >= 0 && !f->std_stream) close(f->fd);
    if (f->delete_on_close && f->host_path) unlink(f->host_path);
    free(f->host_path);
}

static uint32_t file_handle(int fd, const char *host_path, int std_stream)
{
    void_object *o = void_object_new(VOID_OBJ_FILE, sizeof(file_t));
    file_t *f = (file_t *)o->data;
    f->fd = fd;
    f->std_stream = std_stream;
    f->host_path = host_path ? strdup(host_path) : NULL;
    o->destroy = file_destroy;
    o->signaled = 1;
    return void_handle_new(o);
}

static file_t *file_get(uint32_t h)
{
    void_object *o = void_handle_get(h, VOID_OBJ_FILE);
    return o ? (file_t *)o->data : NULL;
}

int void_file_host_fd(uint32_t h)
{
    file_t *f = file_get(h);
    return f ? f->fd : -1;
}

static uint32_t std_handles[3];

void void_files_std_init(void)
{
    std_handles[0] = file_handle(0, NULL, 1);
    std_handles[1] = file_handle(1, NULL, 2);
    std_handles[2] = file_handle(2, NULL, 3);
}

#define GENERIC_READ_ 0x80000000u
#define GENERIC_WRITE_ 0x40000000u
#define FILE_WRITE_DATA_ 0x0002u
#define FILE_APPEND_DATA_ 0x0004u
#define CREATE_NEW_ 1u
#define CREATE_ALWAYS_ 2u
#define OPEN_EXISTING_ 3u
#define OPEN_ALWAYS_ 4u
#define TRUNCATE_EXISTING_ 5u
#define FILE_FLAG_DELETE_ON_CLOSE_ 0x04000000u
#define FILE_FLAG_BACKUP_SEMANTICS_ 0x02000000u

static int skip_movies;
void void_files_skip_movies(int enabled) { skip_movies = enabled; }

static uint32_t create_file(const char *path, uint32_t access, uint32_t disposition, uint32_t flags)
{
    if (!path || !*path) { void_set_last_error(ERROR_PATH_NOT_FOUND_); return WIN_INVALID_HANDLE; }
    if (!strncasecmp(path, "\\\\.\\", 4) || !strcasecmp(path, "CONOUT$") || !strcasecmp(path, "CONIN$")) {
        void_log("[void] CreateFile(%s): device paths are not supported", path);
        void_set_last_error(ERROR_FILE_NOT_FOUND_);
        return WIN_INVALID_HANDLE;
    }
    /* Snapshot the relative path/CWD before another guest can change it. The
     * caller already copied A/W guest text; no guest memory is read unlocked. */
    char norm[1024];
    if (!normalize(path, norm, sizeof norm)) { void_set_last_error(ERROR_PATH_NOT_FOUND_); return WIN_INVALID_HANDLE; }
    const char *movie = strrchr(norm, '\\');
    /* Fullscreen startup movies only: Bink also draws advisor/AI portraits. */
    if (skip_movies && disposition == OPEN_EXISTING_ && movie &&
        (!strcasecmp(movie + 1, "intro.bik") || !strcasecmp(movie + 1, "SHCX_Intro3.bik"))) {
        void_log("[launcher] skipped startup movie: %s", norm);
        void_set_last_error(ERROR_FILE_NOT_FOUND_); return WIN_INVALID_HANDLE;
    }
    int parent_ok = 0, fd = -1, error = 0, directory = 0, exists = 0;
    char *host = NULL;
    void_blocking_begin();
    host = to_host(norm, &parent_ok, NULL);
    if (!host) { error = ERROR_PATH_NOT_FOUND_; goto opened; }
    struct stat st;
    exists = !stat(host, &st);
    if (exists && S_ISDIR(st.st_mode)) {
        directory = 1;
        if (!(flags & FILE_FLAG_BACKUP_SEMANTICS_)) { error = ERROR_ACCESS_DENIED_; goto opened; }
        fd = open(host, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    } else {
        int write = (access & (GENERIC_WRITE_ | FILE_WRITE_DATA_ | FILE_APPEND_DATA_ | 0x10000000u)) != 0;
        int read = (access & (GENERIC_READ_ | 0x0001u | 0x10000000u)) != 0 || !write;
        int oflags = O_CLOEXEC | (write ? (read ? O_RDWR : O_WRONLY) : O_RDONLY);
        switch (disposition) {
        case CREATE_NEW_: oflags |= O_CREAT | O_EXCL; break;
        case CREATE_ALWAYS_: oflags |= O_CREAT | O_TRUNC; break;
        case OPEN_EXISTING_: break;
        case OPEN_ALWAYS_: oflags |= O_CREAT; break;
        case TRUNCATE_EXISTING_: oflags |= O_TRUNC; break;
        default: error = ERROR_INVALID_PARAMETER_; goto opened;
        }
        if ((oflags & O_CREAT) && !write) oflags = (oflags & ~O_ACCMODE) | O_RDWR;
        fd = open(host, oflags, 0664);
    }
    if (fd < 0) {
        int e = errno;
        error = e == ENOENT && !parent_ok ? ERROR_PATH_NOT_FOUND_ : (int)void_errno_to_win32(e);
    }
opened:
    void_blocking_end();
    if (fd < 0) {
        VLOG("[void] CreateFile(%s -> %s) failed: error %d", path, host ? host : "?", error);
        free(host); void_set_last_error((uint32_t)error); return WIN_INVALID_HANDLE;
    }
    uint32_t h = file_handle(fd, host, 0);
    file_t *f = file_get(h);
    f->is_dir = directory;
    if (flags & FILE_FLAG_DELETE_ON_CLOSE_) f->delete_on_close = 1;
    VLOG("[void] CreateFile(%s) -> %s handle %08X", path, host, h);
    free(host);
    if (!directory) void_set_last_error((exists && (disposition == CREATE_ALWAYS_ || disposition == OPEN_ALWAYS_)) ? ERROR_ALREADY_EXISTS_ : 0);
    return h;
}

#define K32 "kernel32.dll"

static uint32_t set_current_directory(uint32_t path_va, int wide)
{
    char *path = wide ? void_wstr_utf8(path_va) : void_astr_utf8(path_va);
    uint32_t ok = path && *path && void_cwd_set(path);
    if (!ok) void_set_last_error(ERROR_PATH_NOT_FOUND_);
    free(path);
    return ok;
}

WINAPI_EXPORT(K32, SetCurrentDirectoryA, 1) { return set_current_directory(ARG(0), 0); }
WINAPI_EXPORT(K32, SetCurrentDirectoryW, 1) { return set_current_directory(ARG(0), 1); }

WINAPI_EXPORT(K32, CreateFileA, 7)
{
    char *path = void_astr_utf8(ARG(0));
    uint32_t h = create_file(path, ARG(1), ARG(4), ARG(5));
    free(path);
    return h;
}

WINAPI_EXPORT(K32, CreateFileW, 7)
{
    char *path = void_wstr_utf8(ARG(0));
    uint32_t h = create_file(path, ARG(1), ARG(4), ARG(5));
    free(path);
    return h;
}

static uint32_t open_file(const char *path, uint32_t info, uint32_t style)
{
    if (!void_vm_writable(info, 136)) { void_set_last_error(998u); return WIN_INVALID_HANDLE; }
    char norm[1024];
    char *reopened = (style & 0x8000u) ? void_astr_utf8(info + 8) : NULL;
    if (reopened) path = reopened;
    int valid = path && normalize(path, norm, sizeof norm);
    memset(GPTR(info), 0, 136);
    MEM8(info) = 136;
    MEM8(info + 1) = 1;
    uint32_t h = WIN_INVALID_HANDLE;
    if (!valid) void_set_last_error(ERROR_PATH_NOT_FOUND_);
    else {
        uint32_t length = void_utf8_to_ga(norm, info + 8, 128);
        if (length >= 128) void_set_last_error(206u);
        else if (style & 0x100u) h = 0;  /* OF_PARSE */
        else if (style & 0x200u) {
            char *host = to_host(norm, NULL, NULL);
            if (host && !unlink(host)) h = 0;
            else void_set_last_error(void_errno_to_win32(errno));
            free(host);
        } else {
            uint32_t access = (style & 3u) == 2 ? GENERIC_READ_ | GENERIC_WRITE_ :
                              (style & 3u) == 1 ? GENERIC_WRITE_ : GENERIC_READ_;
            h = create_file(norm, access, (style & 0x1000u) ? CREATE_ALWAYS_ : OPEN_EXISTING_, 0);
            if (h != WIN_INVALID_HANDLE && (style & 0x4000u)) { void_handle_close(h); h = 0; }
        }
    }
    if (h == WIN_INVALID_HANDLE) MEM16(info + 2) = (uint16_t)void_get_last_error();
    free(reopened);
    return h;
}

WINAPI_EXPORT(K32, OpenFile, 3)
{
    char *path = void_astr_utf8(ARG(0));
    uint32_t h = open_file(path, ARG(1), ARG(2));
    free(path);
    return h;
}

static uint32_t profile_string(uint32_t section, uint32_t key, uint32_t value, uint32_t out, uint32_t cap)
{
    if (!cap) return 0;
    if (!void_vm_writable(out, cap)) { void_set_last_error(998u); return 0; }
    /* shortcut: the port has no win.ini overrides; add parsing when a title supplies one. */
    uint32_t n = value ? (uint32_t)void_gstrlen(value) : 0;
    while (n && MEM8(value + n - 1) == ' ') --n;
    if (!section || !key) { MEM8(out) = 0; if (cap > 1) MEM8(out + 1) = 0; return 0; }
    if (n >= cap) n = cap - 1;
    if (n) memmove(GPTR(out), GPTR(value), n);
    MEM8(out + n) = 0;
    return n;
}

WINAPI_EXPORT(K32, GetProfileStringA, 5) { return profile_string(ARG(0), ARG(1), ARG(2), ARG(3), ARG(4)); }

static void console_write(file_t *f, const uint8_t *data, uint32_t n)
{
    for (uint32_t i = 0; i < n; ++i) {
        char c = (char)data[i];
        if (c == '\n' || f->line_len == sizeof f->line - 1) {
            f->line[f->line_len] = 0;
            void_log("[guest-%s] %s", f->std_stream == 3 ? "stderr" : "stdout", f->line);
            f->line_len = 0;
            if (c == '\n') continue;
        }
        if (c != '\r') f->line[f->line_len++] = c;
    }
}

WINAPI_EXPORT(K32, ReadFile, 5)
{
    uint32_t h = ARG(0), buffer = ARG(1), size = ARG(2), read_out = ARG(3), overlapped = ARG(4);
    file_t *f = file_get(h);
    if (read_out) MEM32(read_out) = 0;
    if (!f || f->is_dir) { void_set_last_error(ERROR_INVALID_HANDLE_); return 0; }
    if (f->std_stream) return 1;
    if (size && !void_vm_writable(buffer, size)) { void_set_last_error(998u); return 0; }  /* ERROR_NOACCESS */
    ssize_t got;
    int blocking = size >= 65536;
    if (blocking) void_blocking_begin();
    if (overlapped) {
        off_t offset = (off_t)MEM32(overlapped + 8) | ((off_t)MEM32(overlapped + 12) << 32);
        got = pread(f->fd, GPTR(buffer), size, offset);
    } else {
        size_t total = 0;
        got = 0;
        while (total < size) {
            ssize_t r = read(f->fd, (uint8_t *)GPTR(buffer) + total, size - total);
            if (r < 0) { if (errno == EINTR) continue; got = -1; break; }
            if (r == 0) break;
            total += (size_t)r;
        }
        if (got >= 0) got = (ssize_t)total;
    }
    int e = errno;
    if (blocking) void_blocking_end();
    if (got < 0) { void_set_last_error(void_errno_to_win32(e)); return 0; }
    if (read_out) MEM32(read_out) = (uint32_t)got;
    if (overlapped) { MEM32(overlapped) = 0; MEM32(overlapped + 4) = (uint32_t)got; }
    return 1;
}

WINAPI_EXPORT(K32, WriteFile, 5)
{
    uint32_t h = ARG(0), buffer = ARG(1), size = ARG(2), written_out = ARG(3), overlapped = ARG(4);
    file_t *f = file_get(h);
    if (written_out) MEM32(written_out) = 0;
    if (!f || f->is_dir) { void_set_last_error(ERROR_INVALID_HANDLE_); return 0; }
    if (f->std_stream) {
        console_write(f, (const uint8_t *)GPTR(buffer), size);
        if (written_out) MEM32(written_out) = size;
        return 1;
    }
    ssize_t put;
    if (overlapped) {
        off_t offset = (off_t)MEM32(overlapped + 8) | ((off_t)MEM32(overlapped + 12) << 32);
        put = pwrite(f->fd, GPTR(buffer), size, offset);
    } else {
        put = write(f->fd, GPTR(buffer), size);
    }
    if (put < 0) { void_set_last_error(void_errno_to_win32(errno)); return 0; }
    if (written_out) MEM32(written_out) = (uint32_t)put;
    return 1;
}

WINAPI_EXPORT(K32, SetFilePointer, 4)
{
    file_t *f = file_get(ARG(0));
    if (!f) { void_set_last_error(ERROR_INVALID_HANDLE_); return 0xFFFFFFFFu; }
    int64_t distance = ARG(2) ? (int64_t)(((uint64_t)MEM32(ARG(2)) << 32) | ARG(1)) : (int64_t)(int32_t)ARG(1);
    int whence = ARG(3) == 0 ? SEEK_SET : ARG(3) == 1 ? SEEK_CUR : SEEK_END;
    if (f->std_stream) return 0;
    off_t pos = lseek(f->fd, (off_t)distance, whence);
    if (pos < 0) { void_set_last_error(131u); return 0xFFFFFFFFu; }   /* ERROR_NEGATIVE_SEEK */
    if (ARG(2)) MEM32(ARG(2)) = (uint32_t)((uint64_t)pos >> 32);
    void_set_last_error(0);
    return (uint32_t)pos;
}

WINAPI_EXPORT(K32, SetFilePointerEx, 5)
{
    file_t *f = file_get(ARG(0));
    if (!f) { void_set_last_error(ERROR_INVALID_HANDLE_); return 0; }
    int64_t distance = (int64_t)(((uint64_t)ARG(2) << 32) | ARG(1));
    int whence = ARG(4) == 0 ? SEEK_SET : ARG(4) == 1 ? SEEK_CUR : SEEK_END;
    off_t pos = lseek(f->fd, (off_t)distance, whence);
    if (pos < 0) { void_set_last_error(131u); return 0; }
    if (ARG(3)) { MEM32(ARG(3)) = (uint32_t)pos; MEM32(ARG(3) + 4) = (uint32_t)((uint64_t)pos >> 32); }
    return 1;
}

WINAPI_EXPORT(K32, GetFileSize, 2)
{
    file_t *f = file_get(ARG(0));
    struct stat st;
    if (!f || fstat(f->fd, &st)) { void_set_last_error(ERROR_INVALID_HANDLE_); return 0xFFFFFFFFu; }
    if (ARG(1)) MEM32(ARG(1)) = (uint32_t)((uint64_t)st.st_size >> 32);
    void_set_last_error(0);
    return (uint32_t)st.st_size;
}

WINAPI_EXPORT(K32, GetFileSizeEx, 2)
{
    file_t *f = file_get(ARG(0));
    struct stat st;
    if (!f || fstat(f->fd, &st)) { void_set_last_error(ERROR_INVALID_HANDLE_); return 0; }
    MEM32(ARG(1)) = (uint32_t)st.st_size;
    MEM32(ARG(1) + 4) = (uint32_t)((uint64_t)st.st_size >> 32);
    return 1;
}

WINAPI_EXPORT(K32, GetFileType, 1)
{
    file_t *f = file_get(ARG(0));
    if (!f) { void_set_last_error(ERROR_INVALID_HANDLE_); return 0; }
    return f->std_stream ? 2u : 1u;    /* FILE_TYPE_CHAR / FILE_TYPE_DISK */
}

WINAPI_EXPORT(K32, FlushFileBuffers, 1) { return file_get(ARG(0)) ? 1u : 0u; }

WINAPI_EXPORT(K32, SetEndOfFile, 1)
{
    file_t *f = file_get(ARG(0));
    if (!f) { void_set_last_error(ERROR_INVALID_HANDLE_); return 0; }
    off_t pos = lseek(f->fd, 0, SEEK_CUR);
    if (pos < 0 || ftruncate(f->fd, pos)) { void_set_last_error(void_errno_to_win32(errno)); return 0; }
    return 1;
}

WINAPI_EXPORT(K32, LockFile, 5) { return 1; }
WINAPI_EXPORT(K32, UnlockFile, 5) { return 1; }
WINAPI_EXPORT(K32, LockFileEx, 6) { return 1; }

WINAPI_EXPORT(K32, GetStdHandle, 1)
{
    switch (ARG(0)) {
    case 0xFFFFFFF6u: return std_handles[0];
    case 0xFFFFFFF5u: return std_handles[1];
    case 0xFFFFFFF4u: return std_handles[2];
    default: void_set_last_error(ERROR_INVALID_HANDLE_); return WIN_INVALID_HANDLE;
    }
}

WINAPI_EXPORT(K32, SetStdHandle, 2)
{
    switch (ARG(0)) {
    case 0xFFFFFFF6u: std_handles[0] = ARG(1); return 1;
    case 0xFFFFFFF5u: std_handles[1] = ARG(1); return 1;
    case 0xFFFFFFF4u: std_handles[2] = ARG(1); return 1;
    default: return 0;
    }
}

static uint32_t attributes_of(const char *path)
{
    if (!path) { void_set_last_error(ERROR_PATH_NOT_FOUND_); return 0xFFFFFFFFu; }
    int parent_ok = 0;
    char *host = to_host(path, &parent_ok, NULL);
    struct stat st;
    if (!host || stat(host, &st)) {
        void_set_last_error(parent_ok ? ERROR_FILE_NOT_FOUND_ : ERROR_PATH_NOT_FOUND_);
        VLOG("[void] GetFileAttributes(%s): missing", path);
        free(host);
        return 0xFFFFFFFFu;
    }
    free(host);
    if (S_ISDIR(st.st_mode)) return 0x10u;
    return 0x20u;                       /* FILE_ATTRIBUTE_ARCHIVE */
}

WINAPI_EXPORT(K32, GetFileAttributesA, 1)
{
    char *path = void_astr_utf8(ARG(0));
    uint32_t r = attributes_of(path);
    free(path);
    return r;
}

WINAPI_EXPORT(K32, GetFileAttributesW, 1)
{
    char *path = void_wstr_utf8(ARG(0));
    uint32_t r = attributes_of(path);
    free(path);
    return r;
}

static uint32_t delete_file(const char *path)
{
    char *host = to_host(path, NULL, NULL);
    int ok = host && !unlink(host);
    if (!ok) void_set_last_error(void_errno_to_win32(errno));
    free(host);
    return ok;
}

WINAPI_EXPORT(K32, DeleteFileA, 1) { char *p = void_astr_utf8(ARG(0)); uint32_t r = delete_file(p); free(p); return r; }
WINAPI_EXPORT(K32, DeleteFileW, 1) { char *p = void_wstr_utf8(ARG(0)); uint32_t r = delete_file(p); free(p); return r; }

static uint32_t move_file(const char *from, const char *to)
{
    char *a = to_host(from, NULL, NULL), *b = to_host(to, NULL, NULL);
    struct stat st;
    int ok = 0;
    if (a && b) {
        if (!stat(b, &st)) void_set_last_error(ERROR_ALREADY_EXISTS_);
        else if (rename(a, b)) void_set_last_error(void_errno_to_win32(errno));
        else ok = 1;
    }
    free(a); free(b);
    return ok;
}

WINAPI_EXPORT(K32, MoveFileA, 2)
{
    char *a = void_astr_utf8(ARG(0)), *b = void_astr_utf8(ARG(1));
    uint32_t r = move_file(a, b);
    free(a); free(b);
    return r;
}

WINAPI_EXPORT(K32, MoveFileW, 2)
{
    char *a = void_wstr_utf8(ARG(0)), *b = void_wstr_utf8(ARG(1));
    uint32_t r = move_file(a, b);
    free(a); free(b);
    return r;
}

WINAPI_EXPORT(K32, CreateDirectoryW, 2)
{
    char *path = void_wstr_utf8(ARG(0));
    char *host = to_host(path, NULL, NULL);
    uint32_t ok = 0;
    if (host) {
        if (!mkdir(host, 0775)) ok = 1;
        else void_set_last_error(errno == EEXIST ? ERROR_ALREADY_EXISTS_ : void_errno_to_win32(errno));
    } else {
        void_set_last_error(ERROR_PATH_NOT_FOUND_);
    }
    VLOG("[void] CreateDirectoryW(%s) = %u", path, ok);
    free(host); free(path);
    return ok;
}

WINAPI_EXPORT(K32, CreateDirectoryA, 2)
{
    char *path = void_astr_utf8(ARG(0));
    char *host = to_host(path, NULL, NULL);
    uint32_t ok = 0;
    if (host) {
        if (!mkdir(host, 0775)) ok = 1;
        else void_set_last_error(errno == EEXIST ? ERROR_ALREADY_EXISTS_ : void_errno_to_win32(errno));
    }
    free(host); free(path);
    return ok;
}

/* GetFullPathName: the normalized absolute Windows path. */
static uint32_t full_path(const char *in, uint32_t buffer, uint32_t capacity, uint32_t file_part, int wide)
{
    char norm[1024];
    if (!in || !normalize(in, norm, sizeof norm)) { void_set_last_error(ERROR_INVALID_PARAMETER_); return 0; }
    /* Keep a trailing separator the caller supplied. */
    size_t in_len = strlen(in);
    if (in_len && (in[in_len - 1] == '\\' || in[in_len - 1] == '/') && strlen(norm) > 3) strcat(norm, "\\");
    uint32_t written = wide ? void_utf8_to_gw(norm, buffer, capacity) : void_utf8_to_ga(norm, buffer, capacity);
    uint32_t units = 0;
    if (wide) { if (written + 1 > capacity) return written; units = written; }
    else { if (written + 1 > capacity) return written; units = written; }
    if (file_part) {
        uint32_t last = 0;
        for (uint32_t i = 0; i < units; ++i) {
            uint32_t c = wide ? MEM16(buffer + 2 * i) : MEM8(buffer + i);
            if (c == '\\') last = i + 1;
        }
        MEM32(file_part) = last < units ? buffer + (wide ? 2 * last : last) : 0;
    }
    return units;
}

WINAPI_EXPORT(K32, GetFullPathNameA, 4)
{
    char *p = void_astr_utf8(ARG(0));
    uint32_t r = full_path(p, ARG(2), ARG(1), ARG(3), 0);
    free(p);
    return r;
}

WINAPI_EXPORT(K32, GetFullPathNameW, 4)
{
    char *p = void_wstr_utf8(ARG(0));
    uint32_t r = full_path(p, ARG(2), ARG(1), ARG(3), 1);
    free(p);
    return r;
}

static const char temp_dir[] = "C:\\Users\\Player\\AppData\\Local\\Temp\\";

WINAPI_EXPORT(K32, GetTempPathA, 2)
{
    uint32_t n = void_utf8_to_ga(temp_dir, ARG(1), ARG(0));
    return n;
}

WINAPI_EXPORT(K32, GetTempPathW, 2)
{
    uint32_t n = void_utf8_to_gw(temp_dir, ARG(1), ARG(0));
    return n;
}

static uint32_t temp_file_name(const char *dir, const char *prefix, uint32_t unique, char *out, size_t cap)
{
    static uint32_t counter;
    uint32_t number = unique ? unique : (uint32_t)(void_tick_ms() + ++counter) & 0xFFFFu;
    for (int attempt = 0; attempt < 65536; ++attempt) {
        if (!number) number = 1;
        snprintf(out, cap, "%s%s%.3s%X.tmp", dir, (dir[0] && dir[strlen(dir) - 1] == '\\') ? "" : "\\", prefix ? prefix : "", number);
        if (unique) return number;
        char *host = to_host(out, NULL, NULL);
        int fd = host ? open(host, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0664) : -1;
        free(host);
        if (fd >= 0) { close(fd); return number; }
        number = (number + 1) & 0xFFFFu;
    }
    return 0;
}

WINAPI_EXPORT(K32, GetTempFileNameA, 4)
{
    char *dir = void_astr_utf8(ARG(0)), *prefix = void_astr_utf8(ARG(1));
    char name[1024];
    uint32_t r = temp_file_name(dir ? dir : "", prefix, ARG(2), name, sizeof name);
    if (r) void_utf8_to_ga(name, ARG(3), WIN_MAX_PATH);
    free(dir); free(prefix);
    return r;
}

WINAPI_EXPORT(K32, GetTempFileNameW, 4)
{
    char *dir = void_wstr_utf8(ARG(0)), *prefix = void_wstr_utf8(ARG(1));
    char name[1024];
    uint32_t r = temp_file_name(dir ? dir : "", prefix, ARG(2), name, sizeof name);
    if (r) void_utf8_to_gw(name, ARG(3), WIN_MAX_PATH);
    free(dir); free(prefix);
    return r;
}

/* ------------------------------------------------------------ find files */
typedef struct {
    char **names;
    int count, next;
    char dir[2048];
} find_t;

static void find_destroy(void_object *o)
{
    find_t *f = (find_t *)o->data;
    for (int i = 0; i < f->count; ++i) free(f->names[i]);
    free(f->names);
}

static uint64_t unix_to_filetime(time_t t, long ns)
{
    return (uint64_t)t * 10000000ull + (uint64_t)ns / 100u + 116444736000000000ull;
}

static int fill_find_data(find_t *f, const char *name, uint32_t out, int wide)
{
    char host[4096];
    snprintf(host, sizeof host, "%s/%s", f->dir, name);
    struct stat st;
    memset(GPTR(out), 0, wide ? 592 : 320);
    if (stat(host, &st)) return 0;
    MEM32(out) = S_ISDIR(st.st_mode) ? 0x10u : 0x20u;
    uint64_t ct = unix_to_filetime(st.st_ctim.tv_sec, st.st_ctim.tv_nsec);
    uint64_t at = unix_to_filetime(st.st_atim.tv_sec, st.st_atim.tv_nsec);
    uint64_t mt = unix_to_filetime(st.st_mtim.tv_sec, st.st_mtim.tv_nsec);
    MEM64(out + 4) = ct; MEM64(out + 12) = at; MEM64(out + 20) = mt;
    MEM32(out + 28) = S_ISDIR(st.st_mode) ? 0 : (uint32_t)((uint64_t)st.st_size >> 32);
    MEM32(out + 32) = S_ISDIR(st.st_mode) ? 0 : (uint32_t)st.st_size;
    if (wide) void_utf8_to_gw(name, out + 44, 260);
    else void_utf8_to_ga(name, out + 44, 260);
    return 1;
}

static uint32_t find_first(const char *pattern_path, uint32_t out, int wide)
{
    if (!void_vm_writable(out, wide ? 592 : 320)) { void_set_last_error(998u); return WIN_INVALID_HANDLE; }
    if (!pattern_path) { void_set_last_error(ERROR_PATH_NOT_FOUND_); return WIN_INVALID_HANDLE; }
    char norm[1024];
    if (!normalize(pattern_path, norm, sizeof norm)) { void_set_last_error(ERROR_PATH_NOT_FOUND_); return WIN_INVALID_HANDLE; }
    char *slash = strrchr(norm, '\\');
    char pattern[512];
    snprintf(pattern, sizeof pattern, "%s", slash ? slash + 1 : norm);
    if (slash) *slash = 0;
    if (strlen(norm) == 2) strcat(norm, "\\");
    int parent_ok = 0;
    char *host_dir = to_host(norm, &parent_ok, NULL);
    DIR *d = host_dir ? opendir(host_dir) : NULL;
    if (!d) {
        VLOG("[void] FindFirstFile(%s): no directory", pattern_path);
        free(host_dir);
        void_set_last_error(ERROR_PATH_NOT_FOUND_);
        return WIN_INVALID_HANDLE;
    }
    void_object *o = void_object_new(VOID_OBJ_FIND, sizeof(find_t));
    find_t *f = (find_t *)o->data;
    o->destroy = find_destroy;
    snprintf(f->dir, sizeof f->dir, "%s", host_dir);
    /* "*.*" matches every name, as on Windows. */
    int match_all = !strcmp(pattern, "*") || !strcmp(pattern, "*.*");
    struct dirent *e;
    while ((e = readdir(d))) {
        if (match_all || !fnmatch(pattern, e->d_name, FNM_CASEFOLD) ||
            (pattern[0] && pattern[strlen(pattern) - 1] == '.' && !strchr(e->d_name, '.'))) {
            f->names = (char **)realloc(f->names, sizeof(char *) * (size_t)(f->count + 1));
            f->names[f->count++] = strdup(e->d_name);
        }
    }
    closedir(d);
    free(host_dir);
    VLOG("[void] FindFirstFile(%s): %d matches", pattern_path, f->count);
    while (f->next < f->count && !fill_find_data(f, f->names[f->next], out, wide)) ++f->next;
    if (f->next >= f->count) {
        void_object_release(o);
        void_set_last_error(ERROR_FILE_NOT_FOUND_);
        return WIN_INVALID_HANDLE;
    }
    ++f->next;
    return void_handle_new(o);
}

static uint32_t find_next(uint32_t handle, uint32_t out, int wide)
{
    if (!void_vm_writable(out, wide ? 592 : 320)) { void_set_last_error(998u); return 0; }
    void_object *o = void_handle_get(handle, VOID_OBJ_FIND);
    if (!o) { void_set_last_error(ERROR_INVALID_HANDLE_); return 0; }
    find_t *f = (find_t *)o->data;
    while (f->next < f->count) {
        if (fill_find_data(f, f->names[f->next++], out, wide)) return 1;
    }
    void_set_last_error(ERROR_NO_MORE_FILES_);
    return 0;
}

WINAPI_EXPORT(K32, FindFirstFileA, 2)
{
    char *path = void_astr_utf8(ARG(0));
    uint32_t h = find_first(path, ARG(1), 0);
    free(path);
    return h;
}

WINAPI_EXPORT(K32, FindFirstFileW, 2)
{
    char *path = void_wstr_utf8(ARG(0));
    uint32_t h = find_first(path, ARG(1), 1);
    free(path);
    return h;
}

WINAPI_EXPORT(K32, FindNextFileA, 2) { return find_next(ARG(0), ARG(1), 0); }
WINAPI_EXPORT(K32, FindNextFileW, 2) { return find_next(ARG(0), ARG(1), 1); }

WINAPI_EXPORT(K32, FindClose, 1)
{
    if (!void_handle_get(ARG(0), VOID_OBJ_FIND)) { void_set_last_error(ERROR_INVALID_HANDLE_); return 0; }
    void_handle_close(ARG(0));
    return 1;
}

/* ------------------------------------------------------------- mappings */
typedef struct { int fd; uint64_t size; uint32_t protect; } mapping_t;

static void mapping_destroy(void_object *o)
{
    mapping_t *m = (mapping_t *)o->data;
    if (m->fd >= 0) close(m->fd);
}

WINAPI_EXPORT(K32, CreateFileMappingA, 6)
{
    uint32_t file = ARG(0), protect = ARG(2);
    uint64_t size = ((uint64_t)ARG(3) << 32) | ARG(4);
    int fd;
    if (file == WIN_INVALID_HANDLE) {
        char path[1100];
        snprintf(path, sizeof path, "%s/Temp/.mapping-XXXXXX", data_root);
        fd = mkstemp(path);
        if (fd < 0 || !size) { if (fd >= 0) close(fd); void_set_last_error(ERROR_INVALID_PARAMETER_); return 0; }
        unlink(path);
        if (ftruncate(fd, (off_t)size)) { close(fd); void_set_last_error(ERROR_NOT_ENOUGH_MEMORY_); return 0; }
    } else {
        file_t *f = file_get(file);
        if (!f) { void_set_last_error(ERROR_INVALID_HANDLE_); return 0; }
        fd = dup(f->fd);
        struct stat st;
        if (fd < 0 || fstat(fd, &st)) {
            int e = errno;
            if (fd >= 0) close(fd);
            void_set_last_error(void_errno_to_win32(e));
            return 0;
        }
        if (!size) size = (uint64_t)st.st_size;
        else if (size > (uint64_t)st.st_size) {
            if ((protect & 0xFFu) != 0x04u || ftruncate(fd, (off_t)size)) {
                close(fd); void_set_last_error(ERROR_ACCESS_DENIED_); return 0;
            }
        }
    }
    void_object *o = void_object_new(VOID_OBJ_MAPPING, sizeof(mapping_t));
    mapping_t *m = (mapping_t *)o->data;
    m->fd = fd;
    m->size = size;
    m->protect = protect;
    o->destroy = mapping_destroy;
    void_set_last_error(0);
    return void_handle_new(o);
}

WINAPI_EXPORT(K32, OpenFileMappingA, 3)
{
    void_set_last_error(ERROR_FILE_NOT_FOUND_);
    return 0;
}

WINAPI_EXPORT(K32, MapViewOfFile, 5)
{
    void_object *o = void_handle_get(ARG(0), VOID_OBJ_MAPPING);
    if (!o) { void_set_last_error(ERROR_INVALID_HANDLE_); return 0; }
    mapping_t *m = (mapping_t *)o->data;
    uint32_t access = ARG(1);
    uint64_t offset = ((uint64_t)ARG(2) << 32) | ARG(3);
    uint64_t size = ARG(4) ? ARG(4) : m->size - offset;
    if (offset > m->size || size > m->size - offset || size > 0x7FFF0000u || !size) { void_set_last_error(ERROR_INVALID_PARAMETER_); return 0; }
    int writable = (access & 2u) != 0 || (access & 1u) != 0;
    int shared = !(access & 1u);           /* FILE_MAP_COPY is private */
    uint32_t va = void_vm_map_file(m->fd, offset, (uint32_t)size, writable, shared);
    VLOG("[void] MapViewOfFile(%08X, off %llX, %llX) = %08X", ARG(0), (unsigned long long)offset,
        (unsigned long long)size, va);
    return va;
}

WINAPI_EXPORT(K32, UnmapViewOfFile, 1) { return (uint32_t)void_vm_unmap_file(ARG(0)); }

int void_files_selftest(void)
{
    int failures = 0;
#define FILE_CHECK(ok, label) do { if (!(ok)) { ++failures; void_log("[selftest] files FAIL: %s (error %u)", label, void_get_last_error()); } } while (0)
    char expected[1200];
    snprintf(expected, sizeof expected, "%s/" GAME_EXE, game_root);
    char *path = to_host(".\\gfx\\..\\" GAME_EXE, NULL, NULL);
    FILE_CHECK(path && !strcmp(path, expected), "relative path normalization");
    free(path);
    snprintf(expected, sizeof expected, "%s/Documents/" GAME_PROFILE "/" GAME_SAVES, data_root);
    path = to_host("C:\\Users\\Player\\Documents\\" GAME_PROFILE "\\" GAME_SAVES, NULL, NULL);
    FILE_CHECK(path && !strcmp(path, expected), "save profile mount");
    free(path);
    uint32_t out = void_galloc(640);
    char saved_cwd[WIN_MAX_PATH], current_cwd[WIN_MAX_PATH];
    void_cwd_get(saved_cwd, sizeof saved_cwd);
    uint32_t documents = void_gstrdup("C:\\Users\\Player\\Documents");
    FILE_CHECK(set_current_directory(documents, 0), "SetCurrentDirectoryA");
    void_cwd_get(current_cwd, sizeof current_cwd);
    FILE_CHECK(!strcmp(current_cwd, "C:\\Users\\Player\\Documents"), "ANSI current directory");
    void_utf8_to_gw(saved_cwd, out, 320);
    FILE_CHECK(set_current_directory(out, 1), "SetCurrentDirectoryW restore");
    void_cwd_get(current_cwd, sizeof current_cwd);
    FILE_CHECK(!strcmp(current_cwd, saved_cwd), "wide current directory restored");
    void_gfree(documents);
    for (int wide = 0; wide < 2; ++wide) {
        memset(GPTR(out), 0xA5, 640);
        uint32_t h = find_first(GAME_PROBE_UPPER, out, wide);
        FILE_CHECK(h != WIN_INVALID_HANDLE, wide ? "FindFirstFileW" : "FindFirstFileA");
        if (h == WIN_INVALID_HANDLE) continue;
        char *name = wide ? void_wstr_utf8(out + 44) : void_astr_utf8(out + 44);
        FILE_CHECK(!strcmp(name, GAME_PROBE) && MEM32(out + (wide ? 592 : 320)) == 0xA5A5A5A5u,
                   wide ? "WIN32_FIND_DATAW layout" : "WIN32_FIND_DATAA layout");
        free(name);
        FILE_CHECK(!find_next(h, out, wide) && void_get_last_error() == ERROR_NO_MORE_FILES_, "FindNextFile exhaustion");
        void_handle_close(h);
    }
    uint32_t h = open_file(GAME_PROBE, out, 0);
    FILE_CHECK(h != WIN_INVALID_HANDLE, "OpenFile existing asset");
    if (h != WIN_INVALID_HANDLE) {
        char magic[2];
        FILE_CHECK(read(void_file_host_fd(h), magic, 2) == 2 && !memcmp(magic, GAME_PROBE_MAGIC, 2) && !MEM16(out + 2), "OpenFile data and OFSTRUCT");
        void_handle_close(h);
    }
    FILE_CHECK(open_file("missing-runtime-selftest.file", out, 0) == WIN_INVALID_HANDLE && MEM16(out + 2), "OpenFile missing asset");
    uint32_t value = void_gstrdup("fallback   ");
    FILE_CHECK(profile_string(1, 1, value, out, 5) == 4 && !memcmp(GPTR(out), "fall\0", 5), "GetProfileStringA truncation");
    FILE_CHECK(profile_string(1, 1, value, out, 640) == 8 && !memcmp(GPTR(out), "fallback\0", 9), "GetProfileStringA trailing blanks");
    void_gfree(value);
    void_gfree(out);
    void_log("[selftest] files: %d failures", failures);
#undef FILE_CHECK
    return failures;
}
