/* APK-only XZ payload decoder; the pinned liblzma is shared with the GOG reader. */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include <lzma.h>

#define MAX_CHUNKS 64
#define MAX_CHUNK (32U << 20)

static long long unpack(const uint8_t *input, size_t input_size, int fd, uint64_t total,
                        uint32_t chunk, const uint32_t *sizes, int count)
{
    if (!total || total > (1ULL << 30) || !chunk || chunk > MAX_CHUNK || count < 1 ||
        count > MAX_CHUNKS || (total + chunk - 1) / chunk != (uint64_t)count) return -400;
    uint64_t packed = 0;
    for (int i = 0; i < count; i++) {
        if (!sizes[i]) return -400;
        packed += sizes[i];
    }
    if (packed != input_size) return -400;
    if (ftruncate(fd, (off_t)total)) return -100 - errno;
    uint8_t *output = mmap(NULL, (size_t)total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (output == MAP_FAILED) return -100 - errno;
    long long result = (long long)total;
    size_t at = 0;
    for (int i = 0; i < count; i++) {
        size_t in_pos = 0, out_pos = 0, length = total - (uint64_t)i * chunk;
        if (length > chunk) length = chunk;
        /* One decoder bounds dictionary memory independently of the device's core count. */
        uint64_t memory = MAX_CHUNK + (8U << 20);
        lzma_ret status = lzma_stream_buffer_decode(&memory, 0, NULL, input + at, &in_pos, sizes[i],
                                                   output + (size_t)i * chunk, &out_pos, length);
        if (status != LZMA_OK || in_pos != sizes[i] || out_pos != length) {
            result = -1LL - (status == LZMA_OK ? LZMA_DATA_ERROR : status);
            break;
        }
        at += sizes[i];
    }
    if (result >= 0 && msync(output, (size_t)total, MS_SYNC)) result = -100 - errno;
    munmap(output, (size_t)total);
    return result;
}

#ifdef UNPACK_MAIN
#include <stdio.h>
#include <sys/stat.h>

/* xzunpack IN.xz OUT TOTAL CHUNK SIZE1,SIZE2,... */
int main(int argc, char **argv)
{
    if (argc != 6) return 2;
    uint32_t sizes[MAX_CHUNKS];
    int count = 0;
    char *next = argv[5];
    while (*next && count < MAX_CHUNKS) {
        char *end;
        unsigned long value = strtoul(next, &end, 10);
        if (end == next || value > UINT32_MAX || (*end && *end != ',')) return 2;
        sizes[count++] = (uint32_t)value;
        next = *end ? end + 1 : end;
    }
    if (*next) return 2;
    int source = open(argv[1], O_RDONLY);
    struct stat info;
    if (source < 0 || fstat(source, &info) || info.st_size <= 0) return 1;
    const uint8_t *input = mmap(NULL, (size_t)info.st_size, PROT_READ, MAP_PRIVATE, source, 0);
    if (input == MAP_FAILED) return 1;
    int target = open(argv[2], O_RDWR | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (target < 0) return 1;
    long long result = unpack(input, (size_t)info.st_size, target, strtoull(argv[3], NULL, 10),
                              (uint32_t)strtoul(argv[4], NULL, 10), sizes, count);
    if (close(target) && result >= 0) result = -100 - errno;
    munmap((void *)input, (size_t)info.st_size);
    close(source);
    printf("unpacked: %lld\n", result);
    return result < 0;
}
#else
#include <jni.h>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>

JNIEXPORT jlong JNICALL Java_org_stronghold_port_NativeLibrary_nativeUnpack(JNIEnv *env, jclass cls,
        jobject manager, jstring name, jstring path, jlong total, jint chunk, jintArray sizes)
{
    (void)cls;
    jsize count = sizes ? (*env)->GetArrayLength(env, sizes) : 0;
    if (!manager || !name || !path || count < 1 || count > MAX_CHUNKS || total <= 0 || chunk <= 0) return -400;
    uint32_t table[MAX_CHUNKS];
    (*env)->GetIntArrayRegion(env, sizes, 0, count, (jint *)table);
    if ((*env)->ExceptionCheck(env)) return -400;
    AAssetManager *assets = AAssetManager_fromJava(env, manager);
    const char *asset_name = (*env)->GetStringUTFChars(env, name, NULL);
    AAsset *asset = assets && asset_name ? AAssetManager_open(assets, asset_name, AASSET_MODE_BUFFER) : NULL;
    if (asset_name) (*env)->ReleaseStringUTFChars(env, name, asset_name);
    if (!asset) return -200;
    const uint8_t *input = AAsset_getBuffer(asset);
    off64_t length = AAsset_getLength64(asset);
    if (!input || length <= 0) { AAsset_close(asset); return -200; }
    const char *file = (*env)->GetStringUTFChars(env, path, NULL);
    int fd = file ? open(file, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600) : -1;
    if (file) (*env)->ReleaseStringUTFChars(env, path, file);
    if (fd < 0) { AAsset_close(asset); return -300; }
    long long result = unpack(input, (size_t)length, fd, (uint64_t)total, (uint32_t)chunk, table, count);
    if (result >= 0 && fsync(fd)) result = -100 - errno;
    if (close(fd) && result >= 0) result = -100 - errno;
    AAsset_close(asset);
    return result;
}
#endif
