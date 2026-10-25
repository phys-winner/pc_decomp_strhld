/* Command-line entry for adb-shell bring-up on the device:
 *   void-arm64 --game DIR --data DIR --images DIR [--selftest] [-v]
 * The APK uses the same initialization through void_game_main. */
#define _GNU_SOURCE
#include "host.h"
#include "input.h"
#include "edition.h"
#include "touch.h"
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

void void_files_std_init(void);
int void_files_selftest(void);
int void_ddraw_selftest(void);
int void_modules_selftest(void);
static int exiting;

void void_process_exit(uint32_t code, int run_detach)
{
    if (!exiting && run_detach) {
        void_dump_state("guest requested process exit, before DLL cleanup");
        exiting = 1;
        void_modules_detach_all(1);
    }
    void void_dsound_shutdown(void);
    void_dsound_shutdown();
    void_log("[void] process exit %u", code);
    void_log_flush();
#ifdef VOID_ANDROID_APP
    void void_android_process_exit(uint32_t code);
    void_android_process_exit(code);
#endif
    _exit((int)code);
}

static const struct {
    const char *name, *export;
    uint32_t base;
} lifted_dlls[] = {
    { "Mss32.dll", "_AIL_startup@0", 0x21100000u },
    { "binkw32.dll", "_BinkOpen@8", 0x30000000u },
};

static int attach_dlls(void)
{
    for (size_t i = 0; i < sizeof lifted_dlls / sizeof lifted_dlls[0]; ++i) {
        uint32_t h = void_module_load(lifted_dlls[i].name);
        if (h != lifted_dlls[i].base) {
            void_log("[void] %s: startup attach failed (%08X)", lifted_dlls[i].name, h);
            return 0;
        }
    }
    return 1;
}

static int self_test(void)
{
    int failures = void_files_selftest() + void_ddraw_selftest() + void_modules_selftest() +
                   stronghold_touch_selftest();
    for (int cycle = 0; cycle < 2; ++cycle) {
        for (size_t i = 0; i < sizeof lifted_dlls / sizeof lifted_dlls[0]; ++i) {
            uint32_t h = void_module_load(lifted_dlls[i].name);
            if (h != lifted_dlls[i].base) { ++failures; continue; }
            uint32_t name = void_gstrdup(lifted_dlls[i].export);
            uint32_t address = void_module_proc(h, name);
            void_gfree(name);
            void_log("[selftest] %s cycle %d: base %08X export %08X", lifted_dlls[i].name, cycle, h, address);
            if (!address) ++failures;
            if (!void_module_free(h)) ++failures;
        }
    }
    void_log("[selftest] %s: %d failures", failures ? "FAIL" : "PASS", failures);
    return failures ? 71 : 0;
}

int void_game_main(const char *game, const char *data, const char *images, int selftest)
{
    void_install_fault_handlers();
    if (mkdir(data, 0775) && errno != EEXIST) return 66;
    char log_path[1024];
    snprintf(log_path, sizeof log_path, "%s/stronghold.log", data);
    void_log_open(log_path);
    void_log("[void] " GAME_TITLE " ARM64: game=%s data=%s images=%s", game, data, images);
    if (!void_guest_reserve()) return 65;
    void_threads_init();
    void_dispatch_init();
    if (!void_files_init(game, data)) return 66;
    void_files_std_init();
    if (!void_modules_init(images)) return 67;
    void_machine_enter();
#ifdef VOID_ANDROID_APP
    int void_android_gdi_selftest(void);
    if (!void_android_gdi_selftest()) void_fatal("Android GDI selftest failed");
#endif
    int code;
    if (selftest) {
        code = self_test();
    } else if (!attach_dlls()) {
        void_modules_detach_all(0);
        code = 68;
    } else {
        void_input_script_start();
        void_log("[void] starting lifted " GAME_EXE " at %08X", GAME_ENTRY);
        code = (int)void_call_guest(GAME_ENTRY, 0, NULL, 1);
        void_log("[void] EXE entry returned %d", code);
        void_modules_detach_all(0);
    }
    void_machine_leave();
    void_log_flush();
    return code;
}

#ifndef VOID_ANDROID_APP
int main(int argc, char **argv)
{
    const char *game = GAME_ROOT_DEFAULT, *data = GAME_DATA_DEFAULT;
    const char *images = GAME_IMAGES_DEFAULT;
    int selftest = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--game") && i + 1 < argc) game = argv[++i];
        else if (!strcmp(argv[i], "--data") && i + 1 < argc) data = argv[++i];
        else if (!strcmp(argv[i], "--images") && i + 1 < argc) images = argv[++i];
        else if (!strcmp(argv[i], "--selftest")) selftest = 1;
        else if (!strcmp(argv[i], "-v")) void_verbose = 1;
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 64; }
    }
    setvbuf(stderr, NULL, _IONBF, 0);
    int code = void_game_main(game, data, images, selftest);
    void_process_exit((uint32_t)code, 0);
    return code;
}
#endif
