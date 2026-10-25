/* Standalone actual-runtime checks. Run on both 4 KiB and 16 KiB Android
 * kernels: guest allocation/protection/query granularity remains 4 KiB. */
#define _GNU_SOURCE
#include "../android/host/memory.c"
#include <assert.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

ptrdiff_t g_mem_base = (ptrdiff_t)VOID_GUEST_BASE;
uint32_t g_cur_func;
static uint32_t last_error;
void void_set_last_error(uint32_t e) { last_error = e; }
uint32_t void_get_last_error(void) { return last_error; }
void void_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}
_Noreturn void void_fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    abort();
}

enum { RW = 4, RO = 2, RESERVE = 0x2000, COMMIT = 0x1000,
       DECOMMIT = 0x4000, RELEASE = 0x8000 };
static const uint32_t info = 0x11000000;

static void query(uint32_t address, uint32_t base, uint32_t size,
                  uint32_t state, uint32_t protection)
{
    assert(void_vm_query(address, info) == 28);
    assert(MEM32(info) == (address & ~0xfffu));
    assert(MEM32(info + 4) == base);
    assert(MEM32(info + 12) == size);
    assert(MEM32(info + 16) == state);
    assert(MEM32(info + 20) == protection);
}

static void subpages(void)
{
    uint32_t a = 0x10000000, old;
    assert(void_vm_alloc(a, 0x10000, RESERVE, RW) == a);
    query(a, a, 0x10000, RESERVE, 0);
    assert(void_vm_alloc(a, 0x1000, COMMIT, RW) == a);
    assert(void_vm_alloc(a + 0x1000, 0x1000, COMMIT, RW) == a + 0x1000);
    assert(void_vm_alloc(a + 0x2000, 0x1000, COMMIT, RW) == a + 0x2000);
    memset(GPTR(a), 0x11, 0x1000);
    memset(GPTR(a + 0x1000), 0x22, 0x1000);
    memset(GPTR(a + 0x2000), 0x33, 0x1000);
    assert(void_vm_protect(a + 0x1000, 1, RO, &old) && old == RW);
    query(a, a, 0x1000, COMMIT, RW);
    query(a + 0x1000, a, 0x1000, COMMIT, RO);
    query(a + 0x2000, a, 0x1000, COMMIT, RW);
    assert(void_vm_readable(a + 0x1000, 0x1000));
    assert(!void_vm_writable(a + 0x1000, 1));
    assert(!void_vm_readable(a + 0x3000, 1));
    /* A 16 KiB host protection change must preserve writable siblings. */
    MEM8(a) = 0x41;
    MEM8(a + 0x2fff) = 0x43;
    assert(MEM8(a + 0x1000) == 0x22);
    assert(void_vm_free(a + 0x1000, 0x1000, DECOMMIT));
    query(a + 0x1000, a, 0x1000, RESERVE, 0);
    assert(!void_vm_readable(a + 0x1000, 1));
    assert(MEM8(a) == 0x41 && MEM8(a + 0xfff) == 0x11);
    assert(MEM8(a + 0x2000) == 0x33 && MEM8(a + 0x2fff) == 0x43);
    MEM8(a + 1) = 0x51;
    MEM8(a + 0x2001) = 0x53;
    assert(void_vm_alloc(a + 0x1000, 0x1000, COMMIT, RW) == a + 0x1000);
    for (uint32_t i = 0; i < 0x1000; ++i) assert(MEM8(a + 0x1000 + i) == 0);
    assert(MEM8(a + 1) == 0x51 && MEM8(a + 0x2001) == 0x53);
    assert(void_vm_alloc(a, 0x3000, COMMIT, RW) == a);
    assert(MEM8(a + 1) == 0x51 && MEM8(a + 0x2001) == 0x53);
    /* Decommit/recommit all committed siblings also exercises a whole host page. */
    assert(void_vm_free(a, 0x3000, DECOMMIT));
    assert(void_vm_alloc(a, 0x3000, COMMIT, RW) == a);
    for (uint32_t i = 0; i < 0x3000; ++i) assert(MEM8(a + i) == 0);
    assert(void_vm_free(a, 0, RELEASE));
    assert(!void_vm_readable(a, 1));
}

static void release_and_claim(void)
{
    uint32_t a = 0x10020000, b = a + 0x10000, pe = 0x10050000;
    assert(void_vm_alloc(a, 0x3000, RESERVE | COMMIT, RW) == a);
    assert(void_vm_alloc(b, 0x1000, RESERVE | COMMIT, RW) == b);
    memset(GPTR(a), 0x71, 0x3000);
    memset(GPTR(b), 0x72, 0x1000);
    assert(void_vm_free(a, 0, RELEASE));
    for (uint32_t i = 0; i < 0x1000; ++i) assert(MEM8(b + i) == 0x72);
    MEM8(b) = 0x73;
    assert(void_vm_alloc(a, 0x3000, RESERVE | COMMIT, RW) == a);
    for (uint32_t i = 0; i < 0x3000; ++i) assert(MEM8(a + i) == 0);
    assert(MEM8(b) == 0x73);
    assert(void_vm_free(a, 0, RELEASE));
    assert(void_vm_free(b, 0, RELEASE));
    assert(void_vm_claim(pe, 0x7005, RW));
    query(pe, pe, 0x8000, COMMIT, RW);
    assert(MEM8(pe + 0x7fff) == 0);
    MEM8(pe + 0x7004) = 0x81;
    MEM8(pe + 0x7fff) = 0x82;
    assert(!void_vm_readable(pe + 0x8000, 1));
    assert(void_vm_free(pe, 0, RELEASE));
}

static void file_views(void)
{
    char path[] = "/data/local/tmp/stronghold-vm16-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    assert(unlink(path) == 0);
    assert(ftruncate(fd, 1) == 0);
    unsigned char byte = 0x31;
    assert(pwrite(fd, &byte, 1, 0) == 1);
    uint32_t a = void_vm_map_file(fd, 0, 1, 1, 1);
    assert(a && !(a & 0xffff));
    query(a, a, 0x1000, COMMIT, RW);
    assert(MEM8(a) == 0x31);
    for (uint32_t i = 1; i < 0x1000; ++i) assert(MEM8(a + i) == 0);
    assert(!void_vm_readable(a + 0x1000, 1));
    MEM8(a) = 0x32;
    assert(msync(GPTR(a), 0x1000, MS_SYNC) == 0);
    assert(pread(fd, &byte, 1, 0) == 1 && byte == 0x32);
    struct stat st;
    assert(fstat(fd, &st) == 0 && st.st_size == 1);
    assert(!void_vm_free(a, 0x1000, DECOMMIT));
    assert(void_vm_readable(a, 1) && MEM8(a) == 0x32);
    assert(pread(fd, &byte, 1, 0) == 1 && byte == 0x32);
    assert(void_vm_unmap_file(a));

    assert(ftruncate(fd, 0x10000 + 4097) == 0);
    byte = 0x61;
    assert(pwrite(fd, &byte, 1, 0x10000) == 1);
    byte = 0x62;
    assert(pwrite(fd, &byte, 1, 0x11000) == 1);
    a = void_vm_map_file(fd, 0x10000, 4097, 0, 1);
    assert(a);
    query(a, a, 0x2000, COMMIT, RO);
    assert(MEM8(a) == 0x61 && MEM8(a + 4096) == 0x62);
    for (uint32_t i = 4097; i < 0x2000; ++i) assert(MEM8(a + i) == 0);
    assert(!void_vm_writable(a, 1) && !void_vm_readable(a + 0x2000, 1));
    assert(void_vm_unmap_file(a));
    a = void_vm_map_file(fd, 0x10000, 4097, 1, 0);
    assert(a);
    MEM8(a) = 0x99;
    assert(pread(fd, &byte, 1, 0x10000) == 1 && byte == 0x61);
    assert(!void_vm_free(a, 0x1000, DECOMMIT));
    assert(MEM8(a) == 0x99);
    assert(void_vm_unmap_file(a));
    assert(!void_vm_map_file(fd, 0x1000, 1, 0, 1));
    assert(close(fd) == 0);
}

static void heap_checks(void)
{
    uint32_t heap = void_heap_create(0, 0);
    assert(heap);
    uint32_t a = void_heap_alloc(heap, 1, 1);
    uint32_t b = void_heap_alloc(heap, 70001, 1);
    assert(a && b && MEM8(a) == 0);
    for (uint32_t i = 0; i < 70001; ++i) assert(MEM8(b + i) == 0);
    memset(GPTR(b), 0xa7, 70001);
    b = void_heap_realloc(heap, b, 90003, 1, 0);
    assert(b && void_heap_size(heap, b) == 90003);
    for (uint32_t i = 0; i < 70001; ++i) assert(MEM8(b + i) == 0xa7);
    for (uint32_t i = 70001; i < 90003; ++i) assert(MEM8(b + i) == 0);
    assert(void_heap_free(heap, a) && void_heap_free(heap, b));
    assert(void_heap_destroy(heap));
}

int main(void)
{
    printf("host page size: %ld; guest page size: %u\n", sysconf(_SC_PAGESIZE), GUEST_PAGE);
    assert(void_guest_reserve());
    assert(void_vm_alloc(info, 0x1000, RESERVE | COMMIT, RW) == info);
    subpages();
    release_and_claim();
    file_views();
    heap_checks();
    assert(void_vm_free(info, 0, RELEASE));
    assert(munmap((void *)(uintptr_t)VOID_GUEST_BASE, VOID_GUEST_SPAN) == 0);
    puts("VM host-page aggregation / 4 KiB metadata / decommit zero / release / PE claim / file tail and sharing / heaps PASS");
    return 0;
}
