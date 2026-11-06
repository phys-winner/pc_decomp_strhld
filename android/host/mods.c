/* Android configuration and checked guest-data writes for the pinned UCP AOT adapter. */
#include "host.h"
#include "edition.h"
#include "mods.h"
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>

#define UCP_REVISION "fd326a41d8927eaee42dfb542658843138c15867"
#define COUNT(array) (sizeof(array) / sizeof((array)[0]))
unsigned char stronghold_ucp_enabled[128];
static char mod_roots[32][PATH_MAX], problem[256];
static int mod_count;
int stronghold_mod_count(void) { return mod_count; }
const char *stronghold_mod_root(int index) { return index >= 0 && index < mod_count ? mod_roots[index] : NULL; }

#ifndef STRONGHOLD_ORIGINAL
typedef struct { unsigned feature; uint32_t address, size; const unsigned char *before, *after; } ucp_patch;
typedef struct { unsigned feature; uint32_t address, size; } ucp_region;
#include "ucp_data.h"
extern void stronghold_ucp_register_code(void);
#endif

const char *stronghold_mods_configure(const char *path)
{
    memset(stronghold_ucp_enabled, 0, sizeof stronghold_ucp_enabled); mod_count = 0;
    if (!path || !*path) return NULL;
    FILE *input = fopen(path, "rb");
    if (!input) return errno == ENOENT ? NULL : "Cannot read mod configuration";
    char line[PATH_MAX + 4];
    if (!fgets(line, sizeof line, input) || strcmp(line, "UCP1 " UCP_REVISION "\n")) {
        fclose(input); return "Mod configuration belongs to another UCP revision";
    }
    const char *error = NULL;
    while (fgets(line, sizeof line, input)) {
        size_t length = strlen(line);
        if (!length || line[length-1] != '\n') { error = "Truncated mod configuration"; break; }
        line[--length] = 0;
        if (line[0] == '@') {
            struct stat st;
            if (mod_count == 32 || line[1] != '/' || !realpath(line+1, mod_roots[mod_count]) ||
                stat(mod_roots[mod_count], &st) || !S_ISDIR(st.st_mode)) { error = "Mod folder is unavailable"; break; }
            mod_count++; continue;
        }
#ifndef STRONGHOLD_ORIGINAL
        char *equals = strchr(line, '=');
        if (!equals || strcmp(equals, "=1")) { error = "Invalid UCP feature setting"; break; }
        *equals = 0; size_t index;
        for (index = 0; index < COUNT(ucp_names); index++) if (!strcmp(line, ucp_names[index])) break;
        if (index == COUNT(ucp_names) || !ucp_available[index]) { error = "UCP feature is not available in this Android build"; break; }
        stronghold_ucp_enabled[index] = 1;
#else
        error = "UCP gameplay patches require Crusader or Extreme"; break;
#endif
    }
    if (ferror(input)) error = "Cannot read mod configuration";
    fclose(input);
#ifndef STRONGHOLD_ORIGINAL
    if (!error) for (size_t i=0; i<COUNT(ucp_conflicts); i++) {
        unsigned a=ucp_conflicts[i][0], b=ucp_conflicts[i][1];
        if (stronghold_ucp_enabled[a] && stronghold_ucp_enabled[b]) {
            snprintf(problem, sizeof problem, "Conflicting UCP features: %s / %s", ucp_names[a], ucp_names[b]);
            error = problem; break;
        }
    }
#endif
    if (error) { memset(stronghold_ucp_enabled, 0, sizeof stronghold_ucp_enabled); mod_count = 0; }
    return error;
}

int stronghold_ucp_apply(void)
{
#ifndef STRONGHOLD_ORIGINAL
    unsigned enabled = 0;
    for (size_t i=0; i<COUNT(ucp_names); i++) enabled += stronghold_ucp_enabled[i] != 0;
    if (!enabled) return 1;
    for (size_t i=0; i<COUNT(ucp_patches); i++) {
        const ucp_patch *p = &ucp_patches[i];
        if (stronghold_ucp_enabled[p->feature] && (!void_vm_writable(p->address, p->size) || memcmp(GPTR(p->address), p->before, p->size))) {
            void_log("[ucp] %s: original bytes differ at %08X; refusing patches", ucp_names[p->feature], p->address); return 0;
        }
    }
    for (size_t i=0; i<COUNT(ucp_regions); i++) {
        const ucp_region *p = &ucp_regions[i];
        if (stronghold_ucp_enabled[p->feature] && void_vm_alloc(p->address, p->size, 0x3000, 4) != p->address) {
            void_log("[ucp] guest data allocation failed at %08X", p->address); return 0;
        }
    }
    for (size_t i=0; i<COUNT(ucp_initializers); i++) {
        const ucp_patch *p = &ucp_initializers[i];
        if (stronghold_ucp_enabled[p->feature]) memcpy(GPTR(p->address), p->after, p->size);
    }
    for (size_t i=0; i<COUNT(ucp_patches); i++) {
        const ucp_patch *p = &ucp_patches[i];
        if (stronghold_ucp_enabled[p->feature]) memcpy(GPTR(p->address), p->after, p->size);
    }
    stronghold_ucp_register_code();
    void_log("[ucp] %u AOT features applied; upstream " UCP_REVISION, enabled);
#endif
    return 1;
}
