/* Real pthread test for timer lifetime, synchronous cancellation and coalescing. */
#define _GNU_SOURCE
#include <assert.h>
#include <sys/mman.h>
#include "../android/host/system.c"

uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi, g_ebp, g_cur_func;
int void_verbose;
static pthread_mutex_t guest = PTHREAD_MUTEX_INITIALIZER;
static uint32_t calls[4], delay_ms;
static uint64_t previous_call, min_interval;
uint64_t void_monotonic_ns(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}
void void_machine_enter(void) { pthread_mutex_lock(&guest); }
void void_machine_leave(void) { pthread_mutex_unlock(&guest); }
void void_blocking_begin(void) { pthread_mutex_unlock(&guest); }
void void_blocking_end(void) { pthread_mutex_lock(&guest); }
void_object *void_handle_get(uint32_t h, int kind) { (void)h; (void)kind; assert(0); return NULL; }
void void_object_signal(void_object *o) { (void)o; assert(0); }
uint32_t void_call_guest(uint32_t va, int n, const uint32_t *a, int pop)
{
    assert(va < 4 && n == 5 && pop == 1 && a[0]);
    ++calls[va];
    uint64_t now = void_monotonic_ns();
    if (previous_call && now - previous_call < min_interval) min_interval = now - previous_call;
    previous_call = now;
    if (delay_ms) usleep(delay_ms * 1000);
    return 0;
}
void void_log(const char *fmt, ...) { (void)fmt; }
_Noreturn void void_fatal(const char *fmt, ...) { fputs(fmt, stderr); abort(); }
static uint32_t start(uint32_t delay, uint32_t proc, uint32_t flags)
{
    g_esp = 0x1000; memset(GPTR(g_esp), 0, 32);
    ARG(0) = delay; ARG(2) = proc; ARG(4) = flags;
    return timeSetEvent_impl();
}
static uint32_t kill_timer(uint32_t id) { ARG(0) = id; return timeKillEvent_impl(); }
int main(void)
{
    assert(mmap((void *)(uintptr_t)VOID_GUEST_BASE, 0x10000, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == (void *)(uintptr_t)VOID_GUEST_BASE);
    pthread_mutex_lock(&guest);
    uint32_t old = start(100, 1, 1); assert(old && kill_timer(old) == 0);
    uint32_t replacement = start(100, 2, 0x101); assert(replacement && replacement != old);
    pthread_mutex_unlock(&guest); usleep(240000); pthread_mutex_lock(&guest);
    assert(kill_timer(replacement) == 0 && calls[1] == 0 && calls[2] >= 1 && calls[2] <= 3);
    assert(kill_timer(old) == 97);

    /* A queued callback waiting for the guest must be skipped on sync kill. */
    uint32_t queued = start(5, 1, 0x101); assert(queued); usleep(20000);
    assert(kill_timer(queued) == 0 && calls[1] == 0);

    delay_ms = 30; previous_call = 0; min_interval = UINT64_MAX;
    uint32_t slow = start(10, 3, 0x101); assert(slow);
    pthread_mutex_unlock(&guest); usleep(180000); pthread_mutex_lock(&guest);
    assert(kill_timer(slow) == 0 && calls[3] >= 3 && calls[3] <= 5 && min_interval >= 35000000u);
    pthread_mutex_unlock(&guest);
    puts("mmtimer_selfcheck: killed slots cannot resurrect, unique IDs, queued callback sync cancellation, missed-period coalescing PASS");
    return 0;
}
