/* Production mod parser and every generated data patch, on the ARM64 host. */
#include "../android/host/mods.c"
#include <assert.h>
#include <stdarg.h>
#include <sys/mman.h>
#include <unistd.h>

static int allocations, registrations;
static const char *config = "/data/local/tmp/stronghold-mods-selfcheck.cfg";
void void_log(const char *format, ...) { (void)format; }
void stronghold_ucp_register_code(void) { registrations++; }
int void_vm_writable(uint32_t address, uint32_t size)
{ return address >= 0x400000 && (uint64_t)address + size <= 0x4000000; }
uint32_t void_vm_alloc(uint32_t address, uint32_t size, uint32_t type, uint32_t protect)
{
    assert(type==0x3000 && protect==4); allocations++;
    size=(size+4095u)&~4095u;
    return mmap(GPTR(address),size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0)==GPTR(address)?address:0;
}
static void write_config(const char *contents)
{
    FILE *file=fopen(config,"wb");assert(file);
    assert(fwrite(contents,1,strlen(contents),file)==strlen(contents));assert(!fclose(file));
}
int main(void)
{
    assert(mmap(GPTR(0x400000),0x3c00000,PROT_READ|PROT_WRITE,
        MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0)==GPTR(0x400000));
    assert(!stronghold_mods_configure(NULL));
    write_config("UCP1 wrong\n");assert(stronghold_mods_configure(config));
    write_config("UCP1 " UCP_REVISION "\nunknown=1\n");assert(stronghold_mods_configure(config));
    write_config("UCP1 " UCP_REVISION "\n@relative\n");assert(stronghold_mods_configure(config));
    write_config("UCP1 " UCP_REVISION "\n@/data/local/tmp\n");assert(!stronghold_mods_configure(config));
    assert(stronghold_mod_count()==1 && !strcmp(stronghold_mod_root(0),"/data/local/tmp"));
    write_config("UCP1 " UCP_REVISION "\ntruncated");assert(stronghold_mods_configure(config));
    assert(!stronghold_mod_count());
    FILE *file=fopen(config,"wb");assert(file);fprintf(file,"UCP1 " UCP_REVISION "\n");
    unsigned available=0;
    for(size_t i=0;i<COUNT(ucp_names);i++) if(ucp_available[i]){fprintf(file,"%s=1\n",ucp_names[i]);available++;}
    assert(!fclose(file) && available==52 && !stronghold_mods_configure(config));
    for(size_t i=0;i<COUNT(ucp_patches);i++)memcpy(GPTR(ucp_patches[i].address),ucp_patches[i].before,ucp_patches[i].size);
    const ucp_patch *first=&ucp_patches[0];MEM8(first->address)^=1;
    assert(!stronghold_ucp_apply() && !allocations && !registrations);MEM8(first->address)^=1;
    assert(stronghold_ucp_apply() && allocations==COUNT(ucp_regions) && registrations==1);
    for(size_t i=0;i<COUNT(ucp_patches);i++)assert(!memcmp(GPTR(ucp_patches[i].address),ucp_patches[i].after,ucp_patches[i].size));
    for(size_t i=0;i<COUNT(ucp_initializers);i++)assert(!memcmp(GPTR(ucp_initializers[i].address),ucp_initializers[i].after,ucp_initializers[i].size));
    write_config("UCP1 " UCP_REVISION "\n");assert(!stronghold_mods_configure(config));
    for(size_t i=0;i<COUNT(ucp_names);i++)assert(!stronghold_ucp_enabled[i]);
    assert(stronghold_ucp_apply() && registrations==1 && !stronghold_mod_count());
    assert(!unlink(config));
    puts("UCP: parser/pin/path/truncation/reset, 52 enabled features, original-byte rejection before mutation, every allocation/data write and disabled startup PASS");
    return 0;
}
