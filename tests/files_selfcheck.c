/* Actual file resolver/open code with a forced FUSE-like pause and a second guest. */
#define _GNU_SOURCE
#include <assert.h>
#include <dirent.h>
#include <sched.h>
#include <sys/mman.h>
static struct dirent *counting_readdir(DIR *d);
#define readdir counting_readdir
#include "../android/host/files.c"
#undef readdir

uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi, g_ebp, g_cur_func;
int void_verbose;
static uint32_t last_error;
static pthread_mutex_t machine = PTHREAD_MUTEX_INITIALIZER;
static __thread int owned;
static __thread uint32_t saved_esp;
static int scans, pause_scan, scan_started, worker_ran, fail_scan;
static void_object *objects[64];
static int mapping_calls;
static uint32_t mapping_size;

/* Mapping bounds are the file layer's responsibility; the VM syscall path
 * is independently exercised by vm16_selfcheck.c against the real kernel. */
uint32_t void_vm_map_file(int fd, uint64_t offset, uint32_t size, int writable, int shared) {
    (void)fd; (void)offset; (void)writable; (void)shared;
    ++mapping_calls; mapping_size = size; return 0x10000000;
}

void void_log(const char *fmt, ...) { (void)fmt; }
void void_set_last_error(uint32_t code) { assert(owned); last_error = code; }
uint32_t void_get_last_error(void) { return last_error; }
void void_blocking_begin(void) { assert(owned); saved_esp = g_esp; owned = 0; assert(!pthread_mutex_unlock(&machine)); }
void void_blocking_end(void) { assert(!owned); assert(!pthread_mutex_lock(&machine)); owned = 1; g_esp = saved_esp; }
char *void_astr_utf8(uint32_t va) { assert(owned); return strdup((char *)GPTR(va)); }
void_object *void_object_new(int kind, size_t extra) {
    assert(owned); void_object *o = calloc(1, sizeof *o + extra); assert(o); o->kind = kind; o->data = o + 1; return o;
}
uint32_t void_handle_new(void_object *o) {
    assert(owned); for (int i = 1; i < 64; ++i) if (!objects[i]) { objects[i] = o; return (uint32_t)i; }
    assert(0); return 0;
}
void_object *void_handle_get(uint32_t h, int kind) { assert(owned); return h < 64 && objects[h] && objects[h]->kind == kind ? objects[h] : NULL; }
int void_handle_close(uint32_t h) {
    assert(owned && h < 64 && objects[h]); void_object *o = objects[h];
    if (o->destroy) o->destroy(o); free(o); objects[h] = NULL; return 1;
}
static struct dirent *counting_readdir(DIR *d) {
    ++scans;
    if (fail_scan) { fail_scan = 0; errno = EIO; return NULL; }
    if (__atomic_exchange_n(&pause_scan, 0, __ATOMIC_ACQ_REL)) {
        assert(!owned); __atomic_store_n(&scan_started, 1, __ATOMIC_RELEASE);
        struct timespec start, now; clock_gettime(CLOCK_MONOTONIC, &start);
        while (!__atomic_load_n(&worker_ran, __ATOMIC_ACQUIRE)) {
            sched_yield(); clock_gettime(CLOCK_MONOTONIC, &now);
            assert(now.tv_sec - start.tv_sec < 3);
        }
    }
    return readdir(d);
}
static void *other_guest(void *arg) {
    (void)arg;
    while (!__atomic_load_n(&scan_started, __ATOMIC_ACQUIRE)) sched_yield();
    assert(!pthread_mutex_lock(&machine)); owned = 1;
    pthread_mutex_lock(&cwd_lock); snprintf(cwd, sizeof cwd, "C:\\Users\\Player\\Documents"); pthread_mutex_unlock(&cwd_lock);
    strcpy((char *)GPTR(0x2000), "changed-by-other-guest.file"); g_esp = 0x4000;
    __atomic_store_n(&worker_ran, 1, __ATOMIC_RELEASE);
    owned = 0; assert(!pthread_mutex_unlock(&machine)); return NULL;
}
static void write_fixture(const char *path, const char *text) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600); assert(fd >= 0);
    assert(write(fd, text, strlen(text)) == (ssize_t)strlen(text)); assert(!close(fd));
}
static void expect_data(uint32_t h, const char *text) {
    assert(h != WIN_INVALID_HANDLE); char data[32] = {0};
    assert(read(void_file_host_fd(h), data, sizeof data) == (ssize_t)strlen(text) && !strcmp(data, text)); void_handle_close(h);
}
static uint32_t asset(const char *name) {
    char path[512]; snprintf(path, sizeof path, GAME_INSTALL "\\FX\\MUSIC\\%s", name);
    return create_file(path, GENERIC_READ_, OPEN_EXISTING_, 0);
}
static void mapping_bounds(void) {
    uint32_t file = asset("crusader.wav"); assert(file != WIN_INVALID_HANDLE);
    ARG(0) = file; ARG(2) = 2; ARG(3) = 0; ARG(4) = 9;
    assert(CreateFileMappingA_impl() == 0 && last_error == ERROR_ACCESS_DENIED_);
    ARG(4) = 0;
    uint32_t mapping = CreateFileMappingA_impl(); assert(mapping);
    ARG(0) = mapping; ARG(1) = 4; ARG(2) = ARG(3) = 0; ARG(4) = 9;
    assert(MapViewOfFile_impl() == 0 && !mapping_calls && last_error == ERROR_INVALID_PARAMETER_);
    ARG(4) = 0;
    assert(MapViewOfFile_impl() == 0x10000000 && mapping_calls == 1 && mapping_size == 8);
    ARG(3) = 0x10000;
    assert(MapViewOfFile_impl() == 0 && mapping_calls == 1);
    void_handle_close(mapping); void_handle_close(file);
}
int main(void) {
    assert(mmap((void *)(uintptr_t)VOID_GUEST_BASE, 0x10000, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == (void *)(uintptr_t)VOID_GUEST_BASE);
    char root[] = "/data/local/tmp/stronghold-files-XXXXXX"; assert(mkdtemp(root));
    char game[1200], data[1200], music[1200], file[1400];
    snprintf(game, sizeof game, "%s/game", root); snprintf(data, sizeof data, "%s/data", root);
    snprintf(music, sizeof music, "%s/fx/music", game); mkdirs(music);
    snprintf(file, sizeof file, "%s/Crusader.WAV", music); write_fixture(file, "original");
    snprintf(file, sizeof file, "%s/Glory.MP3", music); write_fixture(file, "second");
    assert(void_files_init(game, data));
    assert(!pthread_mutex_lock(&machine)); owned = 1;
    char movies[1200]; snprintf(movies, sizeof movies, "%s/binks", game); mkdirs(movies);
    const char *movie_names[] = {"intro.bik", "SHCX_Intro3.bik", "richard_anger.bik"};
    for (int i = 0; i < 3; ++i) {
        snprintf(file, sizeof file, "%s/%s", movies, movie_names[i]); write_fixture(file, "movie");
    }
    void_files_skip_movies(1);
    assert(create_file(GAME_INSTALL "\\binks\\INTRO.BIK", GENERIC_READ_, OPEN_EXISTING_, 0) == WIN_INVALID_HANDLE && last_error == ERROR_FILE_NOT_FOUND_);
    assert(create_file(GAME_INSTALL "\\binks\\shcx_intro3.bik", GENERIC_READ_, OPEN_EXISTING_, 0) == WIN_INVALID_HANDLE);
    expect_data(create_file(GAME_INSTALL "\\binks\\richard_anger.bik", GENERIC_READ_, OPEN_EXISTING_, 0), "movie");
    void_files_skip_movies(0);
    expect_data(create_file(GAME_INSTALL "\\binks\\intro.bik", GENERIC_READ_, OPEN_EXISTING_, 0), "movie");
    g_esp = 0x1000; ARG(0) = 0x2000; ARG(1) = GENERIC_READ_; ARG(4) = OPEN_EXISTING_; ARG(5) = 0;
    strcpy((char *)GPTR(0x2000), ".\\FX\\MUSIC\\CRUSADER.wav");
    pause_scan = 1; pthread_t worker; assert(!pthread_create(&worker, NULL, other_guest, NULL));
    expect_data(CreateFileA_impl(), "original"); assert(worker_ran && g_esp == 0x1000); pthread_join(worker, NULL);
    int warm = scans;
    expect_data(asset("crusader.wav"), "original"); expect_data(asset("glory.mp3"), "second");
    assert(scans == warm); /* The index covers another track, not just a cached full filename. */
    assert(asset("absent-legacy.wav") == WIN_INVALID_HANDLE && last_error == ERROR_FILE_NOT_FOUND_);
    assert(asset("absent-legacy.wav") == WIN_INVALID_HANDLE && scans == warm);
    assert(create_file(GAME_INSTALL "\\FX\\missing\\track.wav", GENERIC_READ_, OPEN_EXISTING_, 0) == WIN_INVALID_HANDLE && last_error == ERROR_PATH_NOT_FOUND_);
    snprintf(file, sizeof file, "%s/Fresh.MP3", music); write_fixture(file, "new");
    /* Fast flash can give adjacent mutations the same timestamp tick. */
    struct stat stamp; assert(!stat(music, &stamp));
    struct timespec changed[2] = {stamp.st_atim, stamp.st_mtim}; ++changed[1].tv_sec;
    assert(!utimensat(AT_FDCWD, music, changed, 0));
    expect_data(asset("fresh.mp3"), "new"); assert(scans > warm); /* Parent mutation invalidates the index. */
    char renamed[1400]; snprintf(file, sizeof file, "%s/Glory.MP3", music); snprintf(renamed, sizeof renamed, "%s/GLORY.mp3", music);
    assert(!rename(file, renamed));
    /* Simulate a filesystem whose generation metadata did not expose the rename. */
    for (int i = 0; i < CASE_DIRS; ++i) if (case_dirs[i].path && !strcmp(case_dirs[i].path, music)) assert(!stat(music, &case_dirs[i].stamp));
    warm = scans; expect_data(asset("glory.mp3"), "second"); assert(scans > warm);
    snprintf(file, sizeof file, "%s/Documents/User.txt", data); write_fixture(file, "profile");
    warm = scans;
    expect_data(create_file("C:\\Users\\Player\\Documents\\USER.TXT", GENERIC_READ_, OPEN_EXISTING_, 0), "profile");
    int first = scans; expect_data(create_file("C:\\Users\\Player\\Documents\\USER.TXT", GENERIC_READ_, OPEN_EXISTING_, 0), "profile");
    assert(first > warm && scans > first); /* Writable profiles never use the asset index. */
    assert(create_file(GAME_INSTALL "\\FX", GENERIC_READ_, OPEN_EXISTING_, 0) == WIN_INVALID_HANDLE && last_error == ERROR_ACCESS_DENIED_);
    uint32_t dir = create_file(GAME_INSTALL "\\FX", GENERIC_READ_, OPEN_EXISTING_, FILE_FLAG_BACKUP_SEMANTICS_);
    assert(dir != WIN_INVALID_HANDLE && file_get(dir)->is_dir); void_handle_close(dir);
    assert(create_file(GAME_INSTALL "\\FX\\MUSIC\\crusader.wav", GENERIC_WRITE_, CREATE_NEW_, 0) == WIN_INVALID_HANDLE && last_error == ERROR_FILE_EXISTS_);
    for (int i = 0; i < CASE_DIRS; ++i) case_clear(&case_dirs[i]);
    fail_scan = 1;
    assert(asset("crusader.wav") == WIN_INVALID_HANDLE);
    expect_data(asset("crusader.wav"), "original"); /* A failed readdir cannot become a complete absent-file index. */
    mapping_bounds();
    owned = 0; pthread_mutex_unlock(&machine);
    puts("files_selfcheck: unlocked lookup/CWD+guest snapshot, directory index, invalidation, errors, userdata and mapping EOF bounds PASS");
    return 0;
}
