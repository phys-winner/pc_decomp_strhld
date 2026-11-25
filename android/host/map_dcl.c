/* Native PKWARE blocks at the editions' five-argument map/save ABI. */
#include "host.h"
#include <zlib.h>
#include "../../reference/pkware-blast/blast.c"
#include "../../reference/stormlib-pklib/tables.c"
#include "../../reference/stormlib-pklib/implode.c"

typedef struct {
    uint8_t *dst;
    uint32_t capacity;
    uint32_t written;
} map_block;

static unsigned map_block_eof(void *unused, unsigned char **buffer)
{
    (void)unused;
    (void)buffer;
    return 0;
}

static int map_block_write(void *out, unsigned char *buffer, unsigned size)
{
    map_block *block = out;
    if (size > block->capacity - block->written) return 1;
    memcpy(block->dst + block->written, buffer, size);
    block->written += size;
    return 0;
}

void stronghold_map_decode(void)
{
    uint32_t checksum = ARG(0), source = ARG(1), packed_size = ARG(2);
    uint32_t target = ARG(3), target_size = ARG(4);
    g_esp += 24;
    g_eax = 0;
    if (packed_size <= 4 || !void_vm_readable(source, packed_size) ||
        !void_vm_writable(checksum, 4) || (target_size && !void_vm_writable(target, target_size)))
        return;
    if (target_size && (uint64_t)source < (uint64_t)target + target_size &&
        (uint64_t)target < (uint64_t)source + packed_size)
        return;

    unsigned char *input = GPTR(source);
    unsigned available = packed_size;
    map_block out = {GPTR(target), target_size, 0};
    /* The guest machine serializes calls, including blast's first table setup. */
    if (blast(map_block_eof, NULL, map_block_write, &out, &available, &input) != 0)
        return;
    MEM32(checksum) = (uint32_t)crc32(0, out.dst, out.written);
    g_eax = 1;
}

typedef struct {
    const uint8_t *src;
    uint32_t remaining;
    map_block output;
    int failed;
} map_encode;

static unsigned map_block_read(char *buffer, unsigned *size, void *opaque)
{
    map_encode *block = opaque;
    if (block->failed) return 0;
    unsigned count = *size < block->remaining ? *size : block->remaining;
    memcpy(buffer, block->src, count);
    block->src += count;
    block->remaining -= count;
    return count;
}

static void map_encoded_write(char *buffer, unsigned *size, void *opaque)
{
    map_encode *block = opaque;
    if (!block->failed)
        block->failed = map_block_write(&block->output, (unsigned char *)buffer, *size);
}

int stronghold_map_encode(void)
{
    uint32_t checksum = ARG(0), written = ARG(1);
    uint32_t source = ARG(2), target = ARG(3), size = ARG(4);
    if (!size || !void_vm_readable(g_ecx, 8) || !void_vm_readable(source, size) ||
        !void_vm_writable(target, size) || !void_vm_writable(checksum, 4) || !void_vm_writable(written, 4) ||
        ((uint64_t)source < (uint64_t)target + size && (uint64_t)target < (uint64_t)source + size))
        return 0;

    unsigned mode = MEM32(g_ecx), dictionary = MEM32(g_ecx + 4);
    char *work = calloc(1, CMP_BUFFER_SIZE);
    if (!work) return 0;
    map_encode block = {GPTR(source), size, {GPTR(target), size, 0}, 0};
    unsigned result = implode(map_block_read, map_encoded_write, work, &block, &mode, &dictionary);
    free(work);

    g_eax = result == CMP_NO_ERROR && !block.failed;
    g_esp += 24;
    if (g_eax) {
        MEM32(checksum) = (uint32_t)crc32(0, GPTR(source), size);
        MEM32(written) = block.output.written;
    }
    return 1;
}
