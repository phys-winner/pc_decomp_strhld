#include "host.h"
#include <assert.h>
#include <signal.h>
#include <unistd.h>

static void record_signal(int signo)
{
    void_crash_signal(signo, -1, 0x1234, 0x5678, 0, 0, 0x401000, 0x2000);
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    void_crash_open(argv[1]);
    if (argv[2][0] == 's') {
        assert(signal(SIGUSR1, record_signal) != SIG_ERR);
        assert(raise(SIGUSR1) == 0);
    }
    void_crash_exit(0);

    char path[1100], text[512] = {0};
    assert(snprintf(path, sizeof path, "%s/stronghold-crash.txt", argv[1]) < (int)sizeof path);
    FILE *file = fopen(path, "rb");
    assert(file && fread(text, 1, sizeof text - 1, file) > 0);
    fclose(file);
    if (argv[2][0] == 's') {
        assert(strstr(text, "Native signal: "));
        assert(strstr(text, "Host PC: 0x0000000000005678"));
        assert(strstr(text, "Guest function: 0x0000000000401000"));
        assert(strstr(text, "First frame presented: 0x0000000000000000"));
        assert(!strstr(text, "Game exit"));
    } else {
        assert(strcmp(text, "Game exit: 0\n") == 0);
    }
}
