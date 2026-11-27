/* Contract checks for range names without game files or a renderer.
 * cc -std=c11 -O2 -ffunction-sections -fdata-sections -pthread
 *    tests/test_runtime_memory.c -Wl,--gc-sections -o out/test-runtime-memory */
#include "../src/runtime_memory.c"
#include <assert.h>

/* The two range-name NIDs must resolve even before the runtime's name table is
 * installed. All unrelated HLE exports are intentionally absent from this test. */
uintptr_t runtime_lookup(const RuntimeExport *entries, size_t count, const char *name) {
    (void)entries; (void)count; (void)name;
    assert(!"unexpected symbol lookup");
    return 0;
}

int main(void) {
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
    puts("PASS: virtual range names, bounds, NIDs and backing offsets");
    return 0;
}
