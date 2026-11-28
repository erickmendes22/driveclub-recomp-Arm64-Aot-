/* Memory contracts and the real Driveclub boot requests without game files or
 * a renderer. Only this test wraps explicit mmap addresses to emulate a host
 * that refuses ranges beyond 39 bits; production has no VA-width override.
 * cc -std=c11 -O2 -ffunction-sections -fdata-sections -pthread
 *    tests/test_runtime_memory.c -Wl,--gc-sections -o out/test-runtime-memory */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/mman.h>
#include <assert.h>
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

static uintptr_t test_host_ceiling;
static int fail_metadata;
static unsigned explicit_mmap_calls;
static void *test_mmap(void *address, size_t size, int prot, int flags, int fd, off_t offset) {
    if (address) {
        ++explicit_mmap_calls;
        if (test_host_ceiling && ((uintptr_t)address>=test_host_ceiling || size>test_host_ceiling-(uintptr_t)address)) {
            errno=ENOMEM;
            return MAP_FAILED;
        }
    }
    return mmap(address,size,prot,flags,fd,offset);
}
static void *test_realloc(void *pointer, size_t size) {
    if (fail_metadata) { errno=ENOMEM; return NULL; }
    return realloc(pointer,size);
}

/* The override also permits a regression replay against a pristine old source
 * copied to a temporary directory, without editing that source or this repo. */
#ifndef RUNTIME_MEMORY_SOURCE
#define RUNTIME_MEMORY_SOURCE "../src/runtime_memory.c"
#endif
#define mmap test_mmap
#define realloc test_realloc
#include RUNTIME_MEMORY_SOURCE
#undef mmap
#undef realloc

/* The two range-name NIDs must resolve even before the runtime's name table is
 * installed. All unrelated HLE exports are intentionally absent from this test. */
uintptr_t runtime_lookup(const RuntimeExport *entries, size_t count, const char *name) {
    (void)entries; (void)count; (void)name;
    assert(!"unexpected symbol lookup");
    return 0;
}

static void test_names(void) {
    typedef int32_t (ABI *SetName)(const void *, uint64_t, const char *);
    typedef int32_t (ABI *ClearName)(const void *, uint64_t);
    SetName set_name=(SetName)runtime_memory_resolve("DGMG3JshrZU#BD#E");
    ClearName clear_name=(ClearName)runtime_memory_resolve("mkgXxsoxWHg#BD#E");
    assert(set_name && clear_name);

    int64_t physical=-1;
    assert(!direct_allocate(0,POOL_SIZE,3*PAGE,PAGE,3,&physical));
    void *base=NULL;
    assert(!direct_map(&base,3*PAGE,3,0,physical,PAGE));
    uintptr_t start=(uintptr_t)base;
    assert(!set_name(base,3*PAGE,"Driveclub heap"));
    VirtualQueryInfo query;
    assert(!virtual_query(base,0,&query,sizeof(query)));
    assert(!strcmp(query.name,"Driveclub heap"));

    /* Naming a subrange changes neither its neighbors nor physical offsets. */
    assert(!set_name((void *)(start+PAGE+7),1,"Texture heap"));
    assert(!virtual_query((void *)(start+PAGE),0,&query,sizeof(query)));
    assert(query.start==start+PAGE && query.end==start+2*PAGE);
    assert(query.offset==(uint64_t)physical+PAGE && !strcmp(query.name,"Texture heap"));
    assert(!virtual_query(base,0,&query,sizeof(query)));
    assert(query.end==start+PAGE && !strcmp(query.name,"Driveclub heap"));
    assert(!virtual_query((void *)(start+2*PAGE),0,&query,sizeof(query)));
    assert(query.offset==(uint64_t)physical+2*PAGE && !strcmp(query.name,"Driveclub heap"));

    assert(set_name(base,PAGE,NULL)==BAD_ADDRESS);
    assert(set_name(base,PAGE,"12345678901234567890123456789012")==NAME_TOO_LONG);
    assert(set_name((void *)(start+3*PAGE),PAGE,"missing")==INVALID);
    assert(set_name(base,UINT64_MAX,"overflow")==INVALID);
    assert(!clear_name((void *)(start+PAGE),PAGE));
    assert(!virtual_query((void *)(start+PAGE),0,&query,sizeof(query)) && query.name[0]==0);
    assert(!direct_release(physical,3*PAGE));
    assert(set_name(base,PAGE,"released")==INVALID);

    /* Splitting a flexible allocation must retain the backing offset too. */
    base=NULL;
    assert(!map_flexible(&base,3*PAGE,3,0));
    start=(uintptr_t)base;
    assert(!set_name((void *)(start+PAGE),PAGE,"flexible middle"));
    read_lock();
    size_t first=vma_index(start), middle=vma_index(start+PAGE);
    assert(vmas[middle].phys==vmas[first].phys+PAGE);
    read_unlock();
    assert(!release_flexible(base,3*PAGE));
    assert(!flexible_bytes);
}

static unsigned mapped_hooks;
static void mapped_hook(uintptr_t address, uint64_t size) { (void)address; (void)size; ++mapped_hooks; }
static void assert_free(uintptr_t address, size_t size) {
    void *reservation=mmap((void *)address,size,PROT_NONE,
                            MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE|MAP_FIXED_NOREPLACE,-1,0);
    assert(reservation==(void *)address);
    assert(!munmap(reservation,size));
}
static void assert_disjoint(uintptr_t first, uint64_t first_size, uintptr_t second, uint64_t second_size) {
    assert(first_size<=UINTPTR_MAX-first && second_size<=UINTPTR_MAX-second);
    assert(first+first_size<=second || second+second_size<=first);
}

static void test_driveclub_boot_replay(void) {
    const uint64_t alignment=2*1024*1024;
    const uint64_t video_bytes=3852468224, main_bytes=555745280, flex_bytes=354418688, magic_bytes=184549376;
    const uintptr_t video_hint=UINT64_C(0x5000000000), main_hint=UINT64_C(0x2000000000);
    const uintptr_t flex_hint=UINT64_C(0xf000000000), magic_hint=UINT64_C(0xa000000000);
    test_host_ceiling=UINT64_C(1)<<39;
    assert(POOL_SIZE==UINT64_C(5056)*1024*1024);
    assert(!vma_count && !live_bytes && !flexible_bytes);
    int64_t video_phys,main_phys,magic_phys;
    void *video=(void *)video_hint, *main_heap=(void *)main_hint, *flex=(void *)flex_hint, *magic=(void *)magic_hint;
    assert(!direct_allocate(0,POOL_SIZE,video_bytes,alignment,3,&video_phys));
    assert(!direct_map(&video,video_bytes,0x32,0,video_phys,alignment));
    assert(video==(void *)video_hint);
    assert(!direct_allocate(0,POOL_SIZE,main_bytes,alignment,0,&main_phys));
    assert(!direct_map(&main_heap,main_bytes,3,0,main_phys,alignment));
    assert(main_heap==(void *)main_hint);
    int32_t result=map_flexible(&flex,flex_bytes,3,0);
    if (result) fprintf(stderr,"REGRESSION: non-fixed Flex hint 0xf000000000 failed under the test's 39-bit host limit (0x%x)\n",(unsigned)result);
    assert(!result);
    assert(!direct_allocate(0,POOL_SIZE,magic_bytes,alignment,0,&magic_phys));
    assert(!direct_map(&magic,magic_bytes,0x33,0,magic_phys,alignment));
    assert((uintptr_t)flex!=flex_hint && (uintptr_t)magic!=magic_hint);
    assert((uintptr_t)flex>=UINT64_C(0x6000000000) && flex_bytes<=test_host_ceiling-(uintptr_t)flex);
    assert((uintptr_t)magic>=UINT64_C(0x6000000000) && magic_bytes<=test_host_ceiling-(uintptr_t)magic);
    assert(!((uintptr_t)flex%PAGE) && !((uintptr_t)magic%alignment));
    const uintptr_t bases[]={(uintptr_t)video,(uintptr_t)main_heap,(uintptr_t)flex,(uintptr_t)magic};
    const uint64_t sizes[]={video_bytes,main_bytes,flex_bytes,magic_bytes};
    for (unsigned i=0;i<4;++i) for (unsigned j=i+1;j<4;++j) assert_disjoint(bases[i],sizes[i],bases[j],sizes[j]);
    assert_free(UINT64_C(0x1000000000),PAGE); /* Later fixed PRT alias remains available. */
    assert(live_bytes==video_bytes+main_bytes+magic_bytes && flexible_bytes==flex_bytes);

    /* The device fault was a native libc write through this offset in Flex. */
    unsigned char *file_system=(unsigned char *)flex+UINT64_C(0x020000cc);
    const char marker[]="relocated file-system allocation";
    memset(file_system,0x5a,sizeof(marker));
    memcpy(file_system,marker,sizeof(marker));
    assert(!memcmp(file_system,marker,sizeof(marker)));
    VirtualQueryInfo query;
    assert(!virtual_query(file_system,0,&query,sizeof(query)));
    assert(query.start==(uintptr_t)flex && query.end==(uintptr_t)flex+flex_bytes && query.offset==0 && (query.flags&1));
    char copied[sizeof(marker)];
    assert(runtime_memory_read_backing((uintptr_t)file_system,copied,sizeof(copied)));
    assert(!memcmp(copied,marker,sizeof(marker)));
    assert(!virtual_query(magic,0,&query,sizeof(query)) && query.offset==(uint64_t)magic_phys && query.protection==0x33);

    size_t before_vmas=vma_count, before_maps=maps, before_flexible_maps=flexible_maps, before_hooks=mapped_hooks;
    unsigned before_calls=explicit_mmap_calls;
    void *fixed=(void *)magic_hint;
    assert(direct_map(&fixed,PAGE,3,MAP_FIXED_FLAG,magic_phys,PAGE)==NO_MEMORY);
    assert(fixed==(void *)magic_hint && explicit_mmap_calls==before_calls+1);
    assert(vma_count==before_vmas && maps==before_maps && mapped_hooks==before_hooks);
    fixed=(void *)flex_hint;
    assert(map_flexible(&fixed,PAGE,3,MAP_FIXED_FLAG)==NO_MEMORY);
    assert(fixed==(void *)flex_hint && flexible_bytes==flex_bytes && flexible_maps==before_flexible_maps);
    assert(vma_count==before_vmas && mapped_hooks==before_hooks);
    fixed=(void *)(UINTPTR_MAX-PAGE+1);
    before_calls=explicit_mmap_calls;
    assert(direct_map(&fixed,PAGE,3,MAP_FIXED_FLAG,magic_phys,PAGE)==INVALID);
    assert(fixed==(void *)(UINTPTR_MAX-PAGE+1) && explicit_mmap_calls==before_calls);

    assert(!release_flexible(flex,flex_bytes));
    assert(!direct_release(video_phys,video_bytes));
    assert(!direct_release(main_phys,main_bytes));
    assert(!direct_release(magic_phys,magic_bytes));
    assert(!vma_count && !live_bytes && !flexible_bytes);
    puts("PASS: exact Driveclub heap sizes, 39-bit host rejection, non-fixed relocation and fixed failure");
}

static void test_host_collisions_and_alias_release(void) {
    const uintptr_t hint=UINT64_C(0x3000000000), fallback=UINT64_C(0x6000000000), alias=UINT64_C(0x1000000000);
    const size_t alignment=2*1024*1024;
    unsigned char *first=mmap((void *)hint,alignment,PROT_READ|PROT_WRITE,
                               MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
    unsigned char *second=mmap((void *)fallback,alignment,PROT_READ|PROT_WRITE,
                                MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
    assert(first==(void *)hint && second==(void *)fallback);
    first[0]=0x31; second[0]=0x73;
    int64_t physical;
    assert(!direct_allocate(0,POOL_SIZE,4*PAGE,PAGE,0,&physical));
    void *memory=(void *)hint;
    assert(!direct_map(&memory,4*PAGE,3,0,physical,alignment));
    assert((uintptr_t)memory>=fallback+alignment && !((uintptr_t)memory%alignment));
    assert(first[0]==0x31 && second[0]==0x73);
    assert_free(alias,4*PAGE);
    void *duplicate=(void *)alias;
    assert(!direct_map(&duplicate,4*PAGE,3,MAP_FIXED_FLAG,physical,PAGE));
    ((uint64_t *)memory)[0]=UINT64_C(0x1122334455667788);
    *(uint64_t *)((unsigned char *)memory+3*PAGE)=UINT64_C(0x8877665544332211);
    assert(*(uint64_t *)alias==UINT64_C(0x1122334455667788));
    assert(*(uint64_t *)(alias+3*PAGE)==UINT64_C(0x8877665544332211));

    assert(!direct_release((uint64_t)physical+PAGE,2*PAGE));
    assert(!runtime_memory_is_mapped((uintptr_t)memory+PAGE,2*PAGE));
    assert(!runtime_memory_is_mapped(alias+PAGE,2*PAGE));
    VirtualQueryInfo query;
    assert(!virtual_query(memory,0,&query,sizeof(query)) && query.offset==(uint64_t)physical && query.end==query.start+PAGE);
    assert(!virtual_query((void *)((uintptr_t)memory+3*PAGE),0,&query,sizeof(query)) && query.offset==(uint64_t)physical+3*PAGE);
    assert(!virtual_query((void *)(alias+3*PAGE),0,&query,sizeof(query)) && query.offset==(uint64_t)physical+3*PAGE);
    *(uint64_t *)(alias+3*PAGE)=UINT64_C(0x1234abcd);
    assert(*(uint64_t *)((unsigned char *)memory+3*PAGE)==UINT64_C(0x1234abcd));
    assert(!direct_release(physical,PAGE));
    assert(!direct_release((uint64_t)physical+3*PAGE,PAGE));
    assert(!vma_count && !live_bytes);
    assert_free((uintptr_t)memory,4*PAGE);
    assert_free(alias,4*PAGE);
    assert(first[0]==0x31 && second[0]==0x73);
    assert(!munmap(first,alignment) && !munmap(second,alignment));
    puts("PASS: host sentinels, aligned fallback, fixed alias and partial backing release");
}

static void test_reservations_and_metadata_failure(void) {
    void *reservation=NULL;
    assert(!reserve_range(&reservation,PAGE,0,PAGE));
    assert((uintptr_t)reservation>=UINT64_C(0x6000000000));
    int64_t physical;
    assert(!direct_allocate(0,POOL_SIZE,PAGE,PAGE,0,&physical));
    void *memory=reservation;
    assert(!direct_map(&memory,PAGE,3,0,physical,PAGE));
    assert(memory!=reservation);
    void *fixed=reservation;
    assert(!direct_map(&fixed,PAGE,3,MAP_FIXED_FLAG|MAP_NO_OVERWRITE,physical,PAGE));
    *(unsigned char *)memory=0x61;
    assert(*(unsigned char *)fixed==0x61);
    assert(!direct_release(physical,PAGE) && !vma_count && !live_bytes);

    /* Inject metadata allocation failure before any host mapping is made. */
    free(vmas); vmas=NULL; vma_capacity=0;
    assert(!direct_allocate(0,POOL_SIZE,PAGE,PAGE,0,&physical));
    memory=(void *)UINT64_C(0x3000000000);
    size_t before_maps=maps, before_hooks=mapped_hooks;
    unsigned before_calls=explicit_mmap_calls;
    fail_metadata=1;
    assert(direct_map(&memory,PAGE,3,0,physical,PAGE)==NO_MEMORY);
    fail_metadata=0;
    assert(memory==(void *)UINT64_C(0x3000000000) && explicit_mmap_calls==before_calls);
    assert(!vma_count && maps==before_maps && mapped_hooks==before_hooks);
    assert_free((uintptr_t)memory,PAGE);
    assert(!direct_release(physical,PAGE) && !live_bytes);
    puts("PASS: guest reservation replacement and metadata failure without a leaked host mapping");
}

static PrtAperture observed_prt[PRT_APERTURES];
static unsigned prt_callbacks;
static void prt_hook(unsigned id, uintptr_t address, uint64_t size) {
    assert(id<PRT_APERTURES);
    observed_prt[id]=(PrtAperture){address,size};
    ++prt_callbacks;
    /* GPU callbacks may query the runtime: they must run outside its writer lock. */
    void *current=NULL; uint64_t current_size=UINT64_MAX;
    assert(!get_prt_aperture((int32_t)id,&current,&current_size));
    assert((uintptr_t)current==address && current_size==size);
}

static void test_prt_contract_and_driveclub_aliases(void) {
    typedef int32_t (ABI *SetPrt)(int32_t,void *,uint64_t);
    typedef int32_t (ABI *GetPrt)(int32_t,void **,uint64_t *);
    typedef int32_t (ABI *CheckedRelease)(uint64_t,uint64_t);
    SetPrt set_prt=(SetPrt)runtime_memory_resolve("BohYr-F7-is#BD#E");
    GetPrt get_prt=(GetPrt)runtime_memory_resolve("L0v2Go5jOuM#BD#E");
    CheckedRelease checked_release=(CheckedRelease)runtime_memory_resolve("hwVSPCmp5tM#BD#E");
    assert(set_prt && get_prt && checked_release);
    const uintptr_t aperture=UINT64_C(0x1000000000);
    const uint64_t tile_bytes=6*PAGE, tail_offset=2*1024*1024, tail_bytes=2*PAGE;
    const uint64_t aperture_size=tail_offset+tail_bytes, physical_size=tile_bytes+tail_bytes;
    void *query_base=(void *)UINTPTR_MAX;
    uint64_t query_size=UINT64_MAX;
    for (int id=0;id<PRT_APERTURES;++id) {
        assert(!get_prt(id,&query_base,&query_size) && !query_base && !query_size);
    }
    assert(get_prt(-1,&query_base,&query_size)==INVALID);
    assert(get_prt(PRT_APERTURES,&query_base,&query_size)==INVALID);
    assert(get_prt(0,NULL,&query_size)==INVALID && get_prt(0,&query_base,NULL)==INVALID);
    runtime_memory_set_prt_hook(prt_hook);
    assert(prt_callbacks==PRT_APERTURES);

    unsigned before_calls=explicit_mmap_calls, before_callbacks=prt_callbacks, before_maps=mapped_hooks;
    size_t before_vmas=vma_count;
    uint64_t before_live=live_bytes;
    assert(!set_prt(0,(void *)aperture,aperture_size));
    assert(!get_prt(0,&query_base,&query_size) && query_base==(void *)aperture && query_size==aperture_size);
    assert(observed_prt[0].address==aperture && observed_prt[0].size==aperture_size);
    assert(prt_callbacks==before_callbacks+1 && explicit_mmap_calls==before_calls && mapped_hooks==before_maps);
    assert(vma_count==before_vmas && live_bytes==before_live && !flexible_bytes);
    assert(!runtime_memory_is_mapped(aperture,aperture_size));
    assert_free(aperture,aperture_size); /* Configuring PRT did not allocate host pages. */
    assert(!set_prt(0,(void *)aperture,aperture_size) && prt_callbacks==before_callbacks+1);
    assert(set_prt(-1,(void *)aperture,aperture_size)==INVALID);
    assert(set_prt(PRT_APERTURES,(void *)aperture,aperture_size)==INVALID);
    assert(set_prt(0,(void *)(aperture+1),aperture_size)==INVALID);
    assert(set_prt(0,(void *)(USER_MIN-4096),aperture_size)==INVALID);
    assert(set_prt(0,(void *)USER_MAX,1)==INVALID);
    assert(set_prt(0,(void *)aperture,UINT64_MAX)==INVALID);
    assert(!get_prt(0,&query_base,&query_size) && query_base==(void *)aperture && query_size==aperture_size);
    assert(prt_callbacks==before_callbacks+1);
    /* The aperture API uses 4 KiB base alignment, independently of 16 KiB
     * physical pages; it does not round or invent a size alignment. */
    assert(!set_prt(1,(void *)(aperture+4096),17));
    assert(!get_prt(1,&query_base,&query_size) && query_base==(void *)(aperture+4096) && query_size==17);
    assert(!set_prt(2,(void *)USER_MAX,0));

    int64_t physical=-1;
    assert(!direct_allocate(0,POOL_SIZE,physical_size,65536,3,&physical));
    void *master=(void *)UINT64_C(0x7f0000000);
    assert(!direct_map(&master,physical_size,0x33,0,physical,65536));
    memset(master,0x36,tile_bytes);
    memset((unsigned char *)master+tile_bytes,0xa5,tail_bytes);
    /* The original caller at guest0x749267 configures PRT, then maps six
     * tiles and a separate mip tail as physical aliases at guest0x74929c/301. */
    void *tiles=(void *)aperture, *tail=(void *)(aperture+tail_offset);
    assert(!direct_map(&tiles,tile_bytes,0x33,MAP_FIXED_FLAG,physical,2*1024*1024));
    assert(!direct_map(&tail,tail_bytes,0x33,MAP_FIXED_FLAG,physical+tile_bytes,2*1024*1024));
    assert(!set_virtual_range_name(tiles,tile_bytes,"PRT tiles"));
    assert(!set_virtual_range_name(tail,tail_bytes,"PRT mip tail"));
    assert(((unsigned char *)tiles)[0]==0x36 && ((unsigned char *)tail)[0]==0xa5);
    VirtualQueryInfo query;
    assert(!virtual_query(tail,0,&query,sizeof(query)) && query.offset==(uint64_t)physical+tile_bytes);
    assert(!strcmp(query.name,"PRT mip tail") && query.protection==0x33);
    assert(virtual_query((void *)(aperture+tile_bytes),0,&query,sizeof(query))==ACCESS);
    unsigned char sparse[3*PAGE];
    memset(sparse,0xcc,sizeof(sparse));
    runtime_memory_read_sparse(aperture+tail_offset-PAGE,sparse,sizeof(sparse));
    for (size_t i=0;i<PAGE;++i) assert(sparse[i]==0);
    for (size_t i=PAGE;i<sizeof(sparse);++i) assert(sparse[i]==0xa5);
    assert(!runtime_memory_read_backing(aperture+tile_bytes,sparse,PAGE));

    assert(!checked_release(physical+PAGE,2*PAGE));
    assert(!runtime_memory_is_mapped(aperture+PAGE,2*PAGE));
    assert(!runtime_memory_is_mapped((uintptr_t)master+PAGE,2*PAGE));
    assert(!get_prt(0,&query_base,&query_size) && query_base==(void *)aperture && query_size==aperture_size);
    runtime_memory_read_sparse(aperture,sparse,sizeof(sparse));
    for (size_t i=0;i<PAGE;++i) assert(sparse[i]==0x36);
    for (size_t i=PAGE;i<sizeof(sparse);++i) assert(sparse[i]==0);
    size_t retained_vmas=vma_count;
    uint64_t retained_bytes=live_bytes;
    assert(checked_release(physical,physical_size)==NOT_FOUND);
    assert(vma_count==retained_vmas && live_bytes==retained_bytes);
    assert(((unsigned char *)tiles)[0]==0x36 && ((unsigned char *)tail)[0]==0xa5);
    assert(checked_release(POOL_SIZE,PAGE)==NOT_FOUND);
    assert(checked_release(UINT64_MAX-PAGE+1,PAGE)==NOT_FOUND);
    assert(checked_release(physical,UINT64_MAX-PAGE+1)==NOT_FOUND);
    assert(checked_release(physical+1,PAGE)==INVALID);
    assert(checked_release(physical,PAGE+1)==INVALID);
    assert(!checked_release(POOL_SIZE,0));
    /* Refill the released hole as a new allocation. Checked release then
     * covers three adjacent physical blocks and removes every alias. */
    int64_t refill=-1;
    assert(!direct_allocate(physical+PAGE,physical+3*PAGE,2*PAGE,PAGE,3,&refill));
    assert(refill==physical+(int64_t)PAGE);
    void *restored=(void *)(aperture+PAGE);
    assert(!direct_map(&restored,2*PAGE,0x33,MAP_FIXED_FLAG,refill,PAGE));
    for (size_t i=0;i<2*PAGE;++i) assert(((unsigned char *)restored)[i]==0);
    assert(!checked_release(physical,physical_size));
    assert(!vma_count && !live_bytes);
    assert_free(aperture,aperture_size);

    /* Disable/reconfigure and install a fresh GPU hook: the complete state is
     * replayed, including zero-size slots, without changing CPU VMAs. */
    runtime_memory_set_prt_hook(NULL);
    before_callbacks=prt_callbacks;
    assert(!set_prt(0,(void *)aperture,0));
    assert(!set_prt(1,(void *)(aperture+4096),0));
    assert(prt_callbacks==before_callbacks);
    runtime_memory_set_prt_hook(prt_hook);
    assert(prt_callbacks==before_callbacks+PRT_APERTURES && !observed_prt[0].size && !observed_prt[1].size);
    runtime_memory_set_prt_hook(NULL);
    puts("PASS: PRT state, GPU hook replay, two real physical aliases, sparse holes and checked release");
}

static void test_checked_and_unchecked_release(void) {
    int64_t physical;
    assert(!direct_allocate(0,POOL_SIZE,3*PAGE,PAGE,0,&physical));
    void *memory=NULL;
    assert(!direct_map(&memory,3*PAGE,3,0,physical,PAGE));
    memset(memory,0x27,3*PAGE);
    assert(!direct_checked_release(physical+PAGE,PAGE));
    size_t before_vmas=vma_count;
    uint64_t before_live=live_bytes;
    assert(direct_checked_release(physical,3*PAGE)==NOT_FOUND);
    assert(vma_count==before_vmas && live_bytes==before_live && *(unsigned char *)memory==0x27);
    assert(!direct_release(physical,3*PAGE)); /* Unchecked free ignores the middle hole. */
    assert(!direct_release(POOL_SIZE,PAGE) && !direct_release(UINT64_MAX-PAGE+1,PAGE));
    assert(!vma_count && !live_bytes);

    /* The finite allocation table cannot lose a right-hand physical fragment
     * when releasing an interior range with no spare metadata slot. */
    assert(!direct_allocate(0,POOL_SIZE,3*PAGE,PAGE,0,&physical));
    memory=NULL;
    assert(!direct_map(&memory,3*PAGE,3,0,physical,PAGE));
    memset(memory,0x54,3*PAGE);
    Block saved[LIMIT];
    memcpy(saved,blocks,sizeof(saved));
    for (int i=0;i<LIMIT;++i) if (!blocks[i].used) blocks[i]=(Block){POOL_SIZE,0,0,1};
    before_vmas=vma_count; before_live=live_bytes;
    assert(direct_checked_release(physical+PAGE,PAGE)==NO_MEMORY);
    assert(vma_count==before_vmas && live_bytes==before_live);
    assert(((unsigned char *)memory)[PAGE]==0x54 && ((unsigned char *)memory)[2*PAGE]==0x54);
    memcpy(blocks,saved,sizeof(saved));
    assert(!direct_checked_release(physical,3*PAGE));
    assert(!vma_count && !live_bytes);
    puts("PASS: checked free is atomic on missing physical pages and allocation-table exhaustion");
}

int main(void) {
    setbuf(stdout,NULL);
    setenv("BB_DMEM_MB","5056",1);
    hook_map=mapped_hook;
    test_names();
    test_driveclub_boot_replay();
    test_host_collisions_and_alias_release();
    test_reservations_and_metadata_failure();
    test_prt_contract_and_driveclub_aliases();
    test_checked_and_unchecked_release();
    puts("PASS: virtual memory allocation, names, queries, bounds, aliases and backing offsets");
    return 0;
}
