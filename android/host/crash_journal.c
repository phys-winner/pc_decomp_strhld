/* Pre-opened signal journal, following the UT3 port's crash.inc. */
#include "host.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdatomic.h>
#include <unistd.h>

static int crash_fd = -1;
static atomic_int crash_written, crash_presented;
static char presented_path[1100];

void void_crash_open(const char *data)
{
    char path[1100];
    int size = snprintf(path, sizeof path, "%s/stronghold-crash.txt", data);
    if (size < 0 || (size_t)size >= sizeof path) return;
    crash_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    size = snprintf(presented_path, sizeof presented_path, "%s/stronghold-first-frame", data);
    if (size < 0 || (size_t)size >= sizeof presented_path) presented_path[0] = 0;
    else unlink(presented_path);
}

/* An empty file tells the next launcher start that this session drew: a kill that leaves no
 * signal record (low memory, ANR) is then not reported as a failed start of the renderer. */
void void_crash_presented(void)
{
    if (crash_fd < 0 || atomic_exchange_explicit(&crash_presented, 1, memory_order_relaxed)) return;
    if (!presented_path[0]) return;
    int fd = open(presented_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd >= 0) close(fd);
}

static void write_crash(const char *text, size_t size)
{
    if (crash_fd < 0 || atomic_exchange_explicit(&crash_written, 1, memory_order_relaxed)) return;
    while (size) {
        ssize_t count = write(crash_fd, text, size);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        text += count;
        size -= (size_t)count;
    }
    fsync(crash_fd);
}

void void_crash_exit(uint32_t code)
{
    if (crash_fd < 0) return;
    char text[48];
    int size = snprintf(text, sizeof text, "Game exit: %u\n", code);
    if (size > 0 && (size_t)size < sizeof text) write_crash(text, (size_t)size);
}

static char *hex_field(char *out, const char *name, uintptr_t value)
{
    while (*name) *out++ = *name++;
    *out++ = '0';
    *out++ = 'x';
    for (int shift = (int)(sizeof value * 8) - 4; shift >= 0; shift -= 4)
        *out++ = "0123456789abcdef"[(value >> shift) & 15];
    *out++ = '\n';
    return out;
}

int void_crash_signal(int signal_number, int code, uintptr_t fault, uintptr_t pc, uintptr_t lr,
                      uintptr_t sp, uint32_t guest_function, uint32_t guest_stack)
{
    if (crash_fd < 0) return 0;
    char text[512], *end = text;
    end = hex_field(end, "Native signal: ", signal_number);
    end = hex_field(end, "Signal code: ", (intptr_t)code);
    end = hex_field(end, "Fault address: ", fault);
    end = hex_field(end, "Host PC: ", pc);
    end = hex_field(end, "Host LR: ", lr);
    end = hex_field(end, "Host SP: ", sp);
    end = hex_field(end, "Guest function: ", guest_function);
    end = hex_field(end, "Guest stack: ", guest_stack);
    end = hex_field(end, "First frame presented: ", (uintptr_t)atomic_load_explicit(&crash_presented, memory_order_relaxed));
    write_crash(text, (size_t)(end - text));
    return 1;
}
