// SPDX-License-Identifier: GPL-2.0-or-later
// Bootstrap direct-memory pool: shared section backing preserves CPU aliases.
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ERR_INVALID ((int32_t)UINT32_C(0x80020016))
#define ERR_AGAIN ((int32_t)UINT32_C(0x80020023))
#define ERR_NOMEM ((int32_t)UINT32_C(0x8002000c))
typedef struct { uint64_t start, length; } Allocation;
static Allocation allocations[1024];
static uint64_t allocation_count;
static SRWLOCK memory_lock = SRWLOCK_INIT;
static HANDLE backing;
static unsigned char *physical_shadow,*image_guest,*image_shadow;
typedef struct {uint64_t address,size;unsigned char *shadow;uint32_t permissions;} Mapping;
static Mapping mappings[4096];
static unsigned mapping_count;
static uint64_t mapping_epoch=1;
// Bumped when a mapping is removed or its table slot reused (memory_unmap): the per-thread
// lookup caches below drop what they remembered. Read without the lock.
static volatile uint64_t mapping_generation=1;
static void (*map_hook)(uint64_t,uint64_t,uint8_t *);
static void (*unmap_hook)(uint64_t,uint64_t);

static uint64_t round_up(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}
// Direct memory as a base PS4 reports it: 5248 MiB for the game, less its flexible memory (the
// process parameter's flexible_memory_size plus 64 MiB of system flexible memory; 512 MiB in
// all when the parameter is absent), as in shadPS4. Games size fixed budgets against this, so a
// smaller value can make "what remains" negative. Read on first use, after relocation.
static uint64_t capacity_value;
static uint64_t capacity(void) {
    if (capacity_value) return capacity_value;
    uint64_t flexible = UINT64_C(512) << 20;
    const unsigned char *process = runtime_process_param(), *parameters = NULL;
    uint64_t size = 0, parameters_size = 0;
    if (process) memcpy(&size, process, 8);
    if (size >= 0x48) memcpy(&parameters, process + 0x40, 8);
    if (parameters) memcpy(&parameters_size, parameters, 8);
    if (parameters_size >= 0x18) {
        const uint64_t *flexible_size = NULL; memcpy(&flexible_size, parameters + 0x10, 8);
        if (flexible_size) flexible = *flexible_size + (UINT64_C(64) << 20);
    }
    uint64_t total = UINT64_C(5248) << 20;
    capacity_value = flexible < total ? (total - flexible) & ~UINT64_C(0x1fffff) : UINT64_C(0x120000000);
    printf("Direct memory: %llu MiB (flexible %llu MiB)\n", (unsigned long long)(capacity_value >> 20), (unsigned long long)(flexible >> 20));
    return capacity_value;
}
#define CAPACITY capacity()
static int ensure_backing(void) {
    if (!backing) backing = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE | SEC_RESERVE,
                                              (DWORD)(CAPACITY >> 32), (DWORD)CAPACITY, NULL);
    if(backing&&!physical_shadow)physical_shadow=MapViewOfFile(backing,FILE_MAP_READ|FILE_MAP_WRITE,0,0,(SIZE_T)CAPACITY);
    return backing != NULL && physical_shadow != NULL;
}
static void register_mapping_locked(uint64_t address,uint64_t size,unsigned char *shadow,uint32_t permissions){
    // A slot freed by memory_unmap (size 0) is reused first: the game streams textures through
    // 64 KiB mappings it maps and unmaps all the time.
    unsigned slot=mapping_count;
    for(unsigned i=0;i<mapping_count;++i)if(!mappings[i].size){slot=i;break;}
    if(slot==4096){fputs("Mapping table exhausted\n",stderr);ExitProcess(23);}
    mappings[slot]=(Mapping){address,size,shadow,permissions};
    if(slot==mapping_count)++mapping_count;else ++mapping_generation;
    ++mapping_epoch;
}
static void register_mapping(uint64_t address,uint64_t size,unsigned char *shadow,uint32_t permissions){
    AcquireSRWLockExclusive(&memory_lock);
    register_mapping_locked(address,size,shadow,permissions);
    void (*hook)(uint64_t,uint64_t,uint8_t *)=map_hook;
    ReleaseSRWLockExclusive(&memory_lock);if(hook)hook(address,size,shadow);
}
uint64_t memory_mapping_lock_shared(void){AcquireSRWLockShared(&memory_lock);return mapping_epoch;}
void memory_mapping_unlock_shared(void){ReleaseSRWLockShared(&memory_lock);}
uint64_t memory_mapping_epoch(void){uint64_t epoch=memory_mapping_lock_shared();memory_mapping_unlock_shared();return epoch;}
static int arena_available(void);
void *memory_allocate_image(uint64_t size){
    // Reserve the guest arena before the GPU driver or anything else can take addresses in it.
    AcquireSRWLockExclusive(&memory_lock);arena_available();ReleaseSRWLockExclusive(&memory_lock);
    size=round_up(size,65536);
    HANDLE section=CreateFileMappingW(INVALID_HANDLE_VALUE,NULL,PAGE_EXECUTE_READWRITE,0,(DWORD)size,NULL);
    if(!section)return NULL;
    image_shadow=MapViewOfFile(section,FILE_MAP_READ|FILE_MAP_WRITE,0,0,(SIZE_T)size);
    for(uintptr_t address=0x10000000;image_shadow&&!image_guest&&address+size<0xf0000000;address+=0x1000000)
        image_guest=MapViewOfFileEx(section,FILE_MAP_ALL_ACCESS|FILE_MAP_EXECUTE,0,0,(SIZE_T)size,(void *)address);
    CloseHandle(section);return image_guest;
}
void memory_register_image(uint64_t offset,uint64_t size,uint32_t flags){
    uint32_t permissions=((flags&4)?1u:0)|((flags&2)?2u:0)|((flags&1)?4u:0);
    register_mapping((uintptr_t)image_guest+offset,size,image_shadow+offset,permissions);
}
uint8_t *memory_backing(uint64_t address,uint64_t size){
    // A positive first-match range remains valid after later registrations
    // (removals bump mapping_generation), so sequential sparse copies can avoid a
    // lock and a linear scan for every 16 KiB page. Bound the cached range
    // before any earlier registration that starts inside it, preserving the
    // original first-match rule even when mappings overlap.
    typedef struct {uint64_t start,end,generation;uint8_t *shadow;} BackingCache;
    static __declspec(thread) BackingCache cache;
    if(size&&cache.generation==mapping_generation&&address>=cache.start&&address<cache.end&&size<=cache.end-address)
        return cache.shadow+(address-cache.start);
    const uint64_t generation=mapping_generation;
    uint8_t *p=NULL;AcquireSRWLockShared(&memory_lock);
    for(unsigned i=0;i<mapping_count;++i){Mapping *m=&mappings[i];
        if(m->size&&address>=m->address&&address-m->address<=m->size&&size<=m->size-(address-m->address)){
            p=m->shadow+(address-m->address);
            uint64_t end=m->address+m->size;
            for(unsigned prior=0;prior<i;++prior)
                if(mappings[prior].address>address&&mappings[prior].address<end)
                    end=mappings[prior].address;
            cache=(BackingCache){address,end,generation,p};
            break;
        }}
    ReleaseSRWLockShared(&memory_lock);return p;
}
// Mapping index (ISS_MAPPING_INDEX=1): per thread, the mappings sorted by address, rebuilt when the
// append-only table grew; looked up without the lock by binary search. Only used while no two
// mappings overlap, so the containing mapping is the one the linear scan finds first.
typedef struct {unsigned count,usable;uint64_t generation,start[4096],end[4096];} MappingIndex;
static int index_order(const void *a,const void *b){
    const uint64_t x=((const uint64_t *)a)[0],y=((const uint64_t *)b)[0];return x<y?-1:x>y;
}
static int mapping_index_find(uint64_t address,uint64_t *available){
    static __declspec(thread) MappingIndex *index;
    static int enabled;
    if(!enabled){char value[8]={0};GetEnvironmentVariableA("ISS_MAPPING_INDEX",value,sizeof(value));enabled=value[0]=='1'?1:-1;}
    if(enabled<0)return 0;
    const unsigned count=*(volatile unsigned *)&mapping_count;
    if(!index){index=calloc(1,sizeof(*index));if(!index){enabled=-1;return 0;}}
    if(index->count!=count||index->generation!=mapping_generation){
        static __declspec(thread) uint64_t pairs[4096][2];
        AcquireSRWLockShared(&memory_lock);
        const unsigned n=mapping_count;index->generation=mapping_generation;
        for(unsigned i=0;i<n;++i){pairs[i][0]=mappings[i].address;pairs[i][1]=mappings[i].address+mappings[i].size;}
        ReleaseSRWLockShared(&memory_lock);
        qsort(pairs,n,sizeof(pairs[0]),index_order);
        index->usable=1;
        for(unsigned i=0;i<n;++i){
            index->start[i]=pairs[i][0];index->end[i]=pairs[i][1];
            if(i&&pairs[i][0]<pairs[i-1][1])index->usable=0;
        }
        index->count=n;
    }
    if(!index->usable)return 0;
    unsigned lo=0,hi=index->count;
    while(lo<hi){const unsigned mid=(lo+hi)/2;if(index->start[mid]<=address)lo=mid+1;else hi=mid;}
    *available=lo&&address<index->end[lo-1]?index->end[lo-1]-address:0;
    return 1;
}
uint64_t memory_clamp(uint64_t address,uint64_t size){
    // An exact-address hit for the first containing mapping cannot change when
    // entries are appended; unmap and slot reuse bump mapping_generation, which
    // empties this cache. Never cache misses.
    typedef struct {uint64_t address,available;} ClampEntry;
    static __declspec(thread) ClampEntry cache[1024];
    static __declspec(thread) uint64_t cache_generation;
    if(cache_generation!=mapping_generation){memset(cache,0,sizeof(cache));cache_generation=mapping_generation;}
    static __declspec(thread) int enabled;
    static __declspec(thread) int cache_first;
    if(!cache_first){char value[8]={0};GetEnvironmentVariableA("ISS_CLAMP_CACHE_FIRST",value,sizeof(value));cache_first=value[0]=='1'?1:-1;}
    // Keep the original index-first route for A/B. With both approved mapping
    // options on it otherwise returns before consulting or populating this cache.
    if(cache_first<0){uint64_t available;if(mapping_index_find(address,&available))return available<size?available:size;}
    if(!enabled){char value[8]={0};GetEnvironmentVariableA("ISS_MAPPING_CACHE",value,sizeof(value));enabled=value[0]=='1'?1:-1;}
    ClampEntry *entry=&cache[((address>>4)*UINT64_C(0x9e3779b97f4a7c15))>>54];
    if(enabled>0&&entry->available&&entry->address==address)return size<entry->available?size:entry->available;
    if(cache_first>0){
        uint64_t available;
        if(mapping_index_find(address,&available)){
            if(enabled>0&&available)*entry=(ClampEntry){address,available};
            return available<size?available:size;
        }
    }
    uint64_t n=0;AcquireSRWLockShared(&memory_lock);
    for(unsigned i=0;i<mapping_count;++i){Mapping *m=&mappings[i];if(m->size&&address>=m->address&&address-m->address<m->size){n=m->size-(address-m->address);break;}}
    ReleaseSRWLockShared(&memory_lock);
    if(enabled>0&&n)*entry=(ClampEntry){address,n};
    return n<size?n:size;
}
void memory_visit(void (*visit)(uint64_t,uint64_t,uint8_t *)){
    Mapping snapshot[4096];unsigned count;AcquireSRWLockShared(&memory_lock);count=mapping_count;memcpy(snapshot,mappings,count*sizeof(Mapping));ReleaseSRWLockShared(&memory_lock);
    for(unsigned i=0;i<count;++i)if(snapshot[i].size)visit(snapshot[i].address,snapshot[i].size,snapshot[i].shadow);
}
void memory_set_map_hook(void (*hook)(uint64_t,uint64_t,uint8_t *)){AcquireSRWLockExclusive(&memory_lock);map_hook=hook;ReleaseSRWLockExclusive(&memory_lock);}
void memory_set_unmap_hook(void (*hook)(uint64_t,uint64_t)){AcquireSRWLockExclusive(&memory_lock);unmap_hook=hook;ReleaseSRWLockExclusive(&memory_lock);}
// The GPU tracks ranges that can span neighbouring mappings. In the arena each mapping is its own
// view and VirtualProtect cannot cross views, so each committed region is protected on its own;
// placeholders and free gaps between mappings have nothing to protect.
int memory_gpu_protect(uint64_t address,uint64_t size,uint32_t permissions){
    if(!size)return 1;
    const uint64_t end=address+size<address?UINT64_MAX:address+size;
    for(uint64_t at=address;at<end;){
        MEMORY_BASIC_INFORMATION info;
        if(!VirtualQuery((void *)(uintptr_t)at,&info,sizeof(info)))return 0;
        uint64_t stop=(uintptr_t)info.BaseAddress+info.RegionSize;if(stop>end)stop=end;
        if(info.State==MEM_COMMIT){
            uint32_t executable=0;DWORD old;
            AcquireSRWLockShared(&memory_lock);
            for(unsigned i=0;i<mapping_count;++i)if(mappings[i].size&&at>=mappings[i].address&&at-mappings[i].address<mappings[i].size){executable=mappings[i].permissions&4;break;}
            ReleaseSRWLockShared(&memory_lock);
            DWORD mode=(permissions&2)?PAGE_READWRITE:(permissions&1)?PAGE_READONLY:PAGE_NOACCESS;
            if(executable)mode=(permissions&2)?PAGE_EXECUTE_READWRITE:PAGE_EXECUTE_READ;
            if(!VirtualProtect((void *)(uintptr_t)at,(SIZE_T)(stop-at),mode,&old)){
                printf("GPU protect failed: address=0x%llx bytes=%llu (of 0x%llx+%llu) permissions=%u winerror=%lu\n",
                       (unsigned long long)at,(unsigned long long)(stop-at),(unsigned long long)address,
                       (unsigned long long)size,permissions,GetLastError());
                return 0;
            }
        }
        at=stop;
    }
    return 1;
}
uint64_t GUEST_ABI memory_direct_size(void) { return CAPACITY; }

int32_t GUEST_ABI memory_allocate_direct(int64_t start, int64_t end, uint64_t length,
                                        uint64_t alignment, int32_t type, int64_t *result) {
    if (start < 0 || end < 0 || !result || !length || length % 16384 || type < 0 || type > 10 ||
        (alignment && (alignment < 16384 || alignment & (alignment - 1)))) return ERR_INVALID;
    if ((uint64_t)end > CAPACITY) end = (int64_t)CAPACITY;
    if (end <= start || length > (uint64_t)(end - start)) return ERR_AGAIN;
    if (!alignment) alignment = 16384;
    if (alignment > CAPACITY) return ERR_AGAIN;
    AcquireSRWLockExclusive(&memory_lock);
    uint64_t candidate = round_up((uint64_t)start, alignment);
    int found = 0;
    while (candidate <= (uint64_t)end && length <= (uint64_t)end - candidate) {
        int overlap = 0;
        for (uint64_t i = 0; i < allocation_count; ++i) {
            Allocation *a = &allocations[i];
            if (candidate < a->start + a->length && a->start < candidate + length) {
                candidate = round_up(a->start + a->length, alignment);
                overlap = 1;
                break;
            }
        }
        if (!overlap) { found = 1; break; }
    }
    if (!found || allocation_count == 1024 || !ensure_backing()) {
        ReleaseSRWLockExclusive(&memory_lock);
        return ERR_AGAIN;
    }
    allocations[allocation_count++] = (Allocation){candidate, length};
    *result = (int64_t)candidate;
    ReleaseSRWLockExclusive(&memory_lock);
    printf("Direct allocation: physical=0x%llx bytes=%llu alignment=%llu\n",
           (unsigned long long)candidate, (unsigned long long)length, (unsigned long long)alignment);
    return 0;
}

// ---- guest address arena -------------------------------------------------------------------
// The PS4 maps memory in 16 KiB pages; a plain Windows view starts on 64 KiB, so consecutive
// 16 KiB mappings collided (the second one failed). Like shadPS4, the guest range is reserved as
// placeholders and every mapping is split out of one at page granularity and replaced by a view
// with MapViewOfFile3 (Windows 10 1803+). Windows already has thread stacks and heaps at random
// places in the range, so each free stretch of it becomes its own placeholder region; a mapping
// stays inside one region. Called with memory_lock held. memory_unmap gives a mapping's range
// back: its view becomes a placeholder again, merged with the free placeholders around it, so
// every free gap between used ranges stays one placeholder (arena_map splits from it).
#define ARENA_START UINT64_C(0x400000000)   // 16 GiB
#define ARENA_END UINT64_C(0x10000000000)   // 1 TiB, inside the GPU's 40-bit range
#ifndef MEM_RESERVE_PLACEHOLDER
#define MEM_RESERVE_PLACEHOLDER 0x00040000
#endif
#ifndef MEM_REPLACE_PLACEHOLDER
#define MEM_REPLACE_PLACEHOLDER 0x00004000
#endif
#ifndef MEM_PRESERVE_PLACEHOLDER
#define MEM_PRESERVE_PLACEHOLDER 0x00000002
#endif
typedef void *(WINAPI *VirtualAlloc2Fn)(HANDLE, void *, SIZE_T, ULONG, ULONG, void *, ULONG);
typedef void *(WINAPI *MapViewOfFile3Fn)(HANDLE, HANDLE, void *, ULONG64, SIZE_T, ULONG, ULONG, void *, ULONG);
typedef BOOL (WINAPI *UnmapViewOfFile2Fn)(HANDLE, void *, ULONG);
static MapViewOfFile3Fn map_view3;
static UnmapViewOfFile2Fn unmap_view2;
static int arena_state; // 0 untried, 1 reserved, -1 unavailable
typedef struct { uint64_t start, end; } Range;
static Range arena_regions[256], arena_used[16384];
static unsigned region_count, arena_count;
static int arena_available(void) {
    if (arena_state) return arena_state > 0;
    HMODULE base = GetModuleHandleW(L"kernelbase.dll");
    VirtualAlloc2Fn alloc2 = base ? (VirtualAlloc2Fn)(void *)GetProcAddress(base, "VirtualAlloc2") : NULL;
    map_view3 = base ? (MapViewOfFile3Fn)(void *)GetProcAddress(base, "MapViewOfFile3") : NULL;
    unmap_view2 = base ? (UnmapViewOfFile2Fn)(void *)GetProcAddress(base, "UnmapViewOfFile2") : NULL;
    uint64_t reserved = 0; DWORD failure = 0;
    for (uint64_t at = ARENA_START; alloc2 && map_view3 && at < ARENA_END && region_count < 256;) {
        MEMORY_BASIC_INFORMATION info;
        if (!VirtualQuery((void *)(uintptr_t)at, &info, sizeof(info))) break;
        uint64_t end = (uintptr_t)info.BaseAddress + info.RegionSize;
        if (end > ARENA_END) end = ARENA_END;
        if (info.State == MEM_FREE) {
            uint64_t start = round_up(at, 65536), stop = end & ~UINT64_C(65535);
            if (stop > start && alloc2(NULL, (void *)(uintptr_t)start, (SIZE_T)(stop - start),
                                       MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0)) {
                arena_regions[region_count++] = (Range){start, stop}; reserved += stop - start;
            } else if (stop > start) failure = GetLastError();
        }
        at = end;
    }
    arena_state = region_count ? 1 : -1;
    if (region_count) printf("Guest address arena: %u regions, %llu GiB in 0x400000000-0x10000000000 (16 KiB mappings)\n",
                             region_count, (unsigned long long)(reserved >> 30));
    else printf("Guest address arena: unavailable (%s, winerror %lu), 64 KiB views\n",
                alloc2 && map_view3 ? "no reservation" : "no placeholder API", failure);
    return arena_state > 0;
}
static const Range *arena_region(uint64_t start, uint64_t length) {
    for (unsigned i = 0; i < region_count; ++i)
        if (start >= arena_regions[i].start && start < arena_regions[i].end && length <= arena_regions[i].end - start) return &arena_regions[i];
    return NULL;
}
static int arena_contains(uint64_t start, uint64_t length) { return arena_region(start, length) != NULL; }
static unsigned arena_index(uint64_t start) { // first used range ending after start
    unsigned lo = 0, hi = arena_count;
    while (lo < hi) { unsigned mid = (lo + hi) / 2; if (arena_used[mid].end <= start) lo = mid + 1; else hi = mid; }
    return lo;
}
static int arena_free(uint64_t start, uint64_t length) {
    unsigned i = arena_index(start);
    return i == arena_count || arena_used[i].start >= start + length;
}
static uint64_t arena_find(uint64_t length, uint64_t alignment) {
    for (unsigned r = 0; r < region_count; ++r) {
        const Range *region = &arena_regions[r];
        uint64_t candidate = round_up(region->start, alignment);
        for (unsigned i = arena_index(region->start); ; ++i) {
            uint64_t gap_end = i < arena_count && arena_used[i].start < region->end ? arena_used[i].start : region->end;
            if (candidate <= gap_end && length <= gap_end - candidate) return candidate;
            if (gap_end == region->end) break;
            candidate = round_up(arena_used[i].end, alignment);
        }
    }
    return 0;
}
// Splits [start, start+length) out of its free placeholder and maps section+offset there.
static void *arena_map(HANDLE section, uint64_t offset, uint64_t start, uint64_t length) {
    const Range *region = arena_region(start, length);
    if (!region || arena_count == sizeof(arena_used) / sizeof(*arena_used) || !arena_free(start, length)) return NULL;
    unsigned i = arena_index(start);
    uint64_t gap_start = i && arena_used[i - 1].end > region->start ? arena_used[i - 1].end : region->start;
    uint64_t gap_end = i < arena_count && arena_used[i].start < region->end ? arena_used[i].start : region->end;
    if ((gap_start != start || gap_end != start + length) &&
        !VirtualFree((void *)(uintptr_t)start, (SIZE_T)length, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) return NULL;
    void *view = map_view3(section, GetCurrentProcess(), (void *)(uintptr_t)start, offset, (SIZE_T)length,
                           MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0);
    if (!view) return NULL; // the split placeholder stays a free gap of the arena
    memmove(&arena_used[i + 1], &arena_used[i], (arena_count - i) * sizeof(*arena_used));
    arena_used[i] = (Range){start, start + length};
    ++arena_count;
    return view;
}
#ifndef MEM_COALESCE_PLACEHOLDERS
#define MEM_COALESCE_PLACEHOLDERS 0x00000001
#endif
// Gives [start, start+length) back to the arena: the view becomes a placeholder again and is
// merged with the free placeholders next to it. Only a range arena_map made, exactly.
static int arena_unmap(uint64_t start, uint64_t length) {
    unsigned i = arena_index(start);
    if (i == arena_count || arena_used[i].start != start || arena_used[i].end != start + length || !unmap_view2) return 0;
    const Range *region = arena_region(start, length);
    if (!region || !unmap_view2(GetCurrentProcess(), (void *)(uintptr_t)start, MEM_PRESERVE_PLACEHOLDER)) return 0;
    memmove(&arena_used[i], &arena_used[i + 1], (arena_count - i - 1) * sizeof(*arena_used));
    --arena_count;
    uint64_t gap_start = i && arena_used[i - 1].end > region->start ? arena_used[i - 1].end : region->start;
    uint64_t gap_end = i < arena_count && arena_used[i].start < region->end ? arena_used[i].start : region->end;
    // Fails harmlessly when the range is the whole gap (nothing to merge).
    if (gap_start != start || gap_end != start + length)
        VirtualFree((void *)(uintptr_t)gap_start, (SIZE_T)(gap_end - gap_start), MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS);
    return 1;
}
static int32_t map_failed(const char *kind, uint64_t requested, uint64_t length, int32_t flags, int32_t result) {
    printf("%s mapping failed: hint=0x%llx bytes=%llu flags=0x%x winerror=%lu\n", kind, (unsigned long long)requested,
           (unsigned long long)length, flags, GetLastError());
    return result;
}

int32_t GUEST_ABI memory_map_direct(void **address, uint64_t length, int32_t prot,
                                   int32_t flags, int64_t physical, uint64_t alignment) {
    // Unknown modes remain explicit errors. Fixed address bit 0x10 is supported;
    // executable, mixed/flexible mappings and release/protect APIs remain separate work.
    if (!address || !length || length % 16384 || physical < 0 || (uint64_t)physical % 16384 ||
        (prot & ~0x33) || (flags & ~0x10) ||
        (alignment && (alignment < 16384 || alignment & (alignment - 1)))) return ERR_INVALID;
    if (!alignment) alignment = 16384;
    if (alignment > UINT64_C(0x100000000)) return ERR_INVALID;
    AcquireSRWLockExclusive(&memory_lock);
    int allocated = 0;
    for (uint64_t i = 0; i < allocation_count; ++i)
        if ((uint64_t)physical >= allocations[i].start &&
            (uint64_t)physical - allocations[i].start <= allocations[i].length &&
            length <= allocations[i].length - ((uint64_t)physical - allocations[i].start)) allocated = 1;
    if (!allocated) { ReleaseSRWLockExclusive(&memory_lock); return map_failed("Direct", (uintptr_t)*address, length, flags, ERR_INVALID); }
    uintptr_t hint = (uintptr_t)*address;
    if (arena_available() && (!hint || arena_contains(hint, length))) {
        uint64_t at = hint && !(hint % alignment) && arena_free(hint, length) ? hint : (flags & 0x10) ? 0 : arena_find(length, alignment);
        void *view = at ? arena_map(backing, (uint64_t)physical, at, length) : NULL;
        DWORD old, mode = (prot & 2) ? PAGE_READWRITE : (prot & 1) ? PAGE_READONLY : PAGE_NOACCESS;
        if (view && (!VirtualAlloc(view, (SIZE_T)length, MEM_COMMIT, PAGE_READWRITE) || !VirtualProtect(view, (SIZE_T)length, mode, &old)))
            view = NULL; // stays mapped and recorded; reported as a failure
        if (!view) { ReleaseSRWLockExclusive(&memory_lock); return map_failed("Direct", hint, length, flags, ERR_NOMEM); }
        *address = view;
        unsigned char *shadow = physical_shadow + (uint64_t)physical;
        register_mapping_locked((uintptr_t)view, length, shadow, (uint32_t)prot & 3);
        void (*hook)(uint64_t, uint64_t, uint8_t *) = map_hook;
        ReleaseSRWLockExclusive(&memory_lock);
        if (hook) hook((uintptr_t)view, length, shadow);
        printf("Direct mapping: physical=0x%llx virtual=0x%llx bytes=%llu\n", (unsigned long long)(uint64_t)physical,
               (unsigned long long)(uintptr_t)view, (unsigned long long)length);
        return 0;
    }
    uint64_t offset = (uint64_t)physical & ~UINT64_C(65535);
    uint64_t delta = (uint64_t)physical - offset;
    uint64_t span = delta + length;
    void *view = NULL;
    uintptr_t requested = (uintptr_t)*address;
    if (requested && requested >= delta && (requested - delta) % 65536 == 0 && requested % alignment == 0)
        view = MapViewOfFileEx(backing, FILE_MAP_ALL_ACCESS, (DWORD)(offset >> 32), (DWORD)offset,
                               (SIZE_T)span, (void *)(requested - delta));
    if (!view && !(flags & 0x10)) {
        for (uintptr_t base = 0x20000000; base + span <= UINT64_C(0xf0000000); base += 0x1000000) {
            if ((base + delta) % alignment) continue;
            view = MapViewOfFileEx(backing, FILE_MAP_ALL_ACCESS, (DWORD)(offset >> 32), (DWORD)offset,
                                  (SIZE_T)span, (void *)base);
            if (view) break;
        }
        // Large GPU arenas cannot fit below 4 GiB; keep them within the PS4's 40-bit GPU address range.
        for(uintptr_t base=UINT64_C(0x200000000);!view&&base+span<UINT64_C(0x10000000000);base+=UINT64_C(0x10000000)){
            if((base+delta)%alignment)continue;
            view=MapViewOfFileEx(backing,FILE_MAP_ALL_ACCESS,(DWORD)(offset>>32),(DWORD)offset,(SIZE_T)span,(void *)base);
        }
    }
    if (!view) { ReleaseSRWLockExclusive(&memory_lock); return map_failed("Direct", requested, length, flags, ERR_NOMEM); }
    // SEC_RESERVE makes backing lazy. Committing a section range commits shared
    // storage, so all views of that physical range observe writes without copying.
    if (!VirtualAlloc(view, (SIZE_T)span, MEM_COMMIT, PAGE_READWRITE)) {
        UnmapViewOfFile(view); ReleaseSRWLockExclusive(&memory_lock); return ERR_NOMEM;
    }
    DWORD old;
    DWORD mode = (prot & 2) ? PAGE_READWRITE : (prot & 1) ? PAGE_READONLY : PAGE_NOACCESS;
    if (!VirtualProtect(view, (SIZE_T)span, PAGE_NOACCESS, &old) ||
        !VirtualProtect((unsigned char *)view + delta, (SIZE_T)length, mode, &old)) {
        UnmapViewOfFile(view); ReleaseSRWLockExclusive(&memory_lock); return ERR_NOMEM;
    }
    *address = (unsigned char *)view + delta;
    unsigned char *shadow=physical_shadow+(uint64_t)physical;
    register_mapping_locked((uintptr_t)*address,length,shadow,(uint32_t)prot&3);
    void (*hook)(uint64_t,uint64_t,uint8_t *)=map_hook;
    ReleaseSRWLockExclusive(&memory_lock);
    if(hook)hook((uintptr_t)*address,length,shadow);
    printf("Direct mapping: physical=0x%llx virtual=0x%llx bytes=%llu\n",
           (unsigned long long)(uint64_t)physical, (unsigned long long)(uintptr_t)*address, (unsigned long long)length);
    return 0;
}

// ---- flexible memory, protection, batch maps ---------------------------------------------------
// Flexible memory is a section of its own, mapped twice like direct memory: the guest view and a
// shadow view the renderer reads. Views need 64 KiB granularity, so lengths round up to it.
static DWORD page_mode(int32_t prot){
    int read=prot&1,write=prot&2,execute=prot&4;
    if(execute)return write?PAGE_EXECUTE_READWRITE:read?PAGE_EXECUTE_READ:PAGE_EXECUTE;
    return write?PAGE_READWRITE:read?PAGE_READONLY:PAGE_NOACCESS;
}
static void *map_view(HANDLE section,uint64_t span,uintptr_t requested,int fixed){
    void *view=NULL;
    if(requested&&!(requested&65535))
        view=MapViewOfFileEx(section,FILE_MAP_ALL_ACCESS|FILE_MAP_EXECUTE,0,0,(SIZE_T)span,(void *)requested);
    if(view||fixed)return view;
    for(uintptr_t base=0x20000000;!view&&base+span<=UINT64_C(0xf0000000);base+=0x1000000)
        view=MapViewOfFileEx(section,FILE_MAP_ALL_ACCESS|FILE_MAP_EXECUTE,0,0,(SIZE_T)span,(void *)base);
    for(uintptr_t base=UINT64_C(0x200000000);!view&&base+span<UINT64_C(0x10000000000);base+=UINT64_C(0x10000000))
        view=MapViewOfFileEx(section,FILE_MAP_ALL_ACCESS|FILE_MAP_EXECUTE,0,0,(SIZE_T)span,(void *)base);
    return view;
}
static int32_t flexible_failed(const char *why,void **address,uint64_t length,int32_t prot,int32_t flags,int32_t result){
    printf("Flexible mapping failed (%s): hint=0x%llx bytes=%llu prot=0x%x flags=0x%x winerror=%lu\n",why,
           address?(unsigned long long)(uintptr_t)*address:0ull,(unsigned long long)length,prot,flags,GetLastError());
    return result;
}
int32_t GUEST_ABI memory_map_flexible(void **address,uint64_t length,int32_t prot,int32_t flags,const char *name){
    if(!address||!length||length%16384||(prot&~0x37))return flexible_failed("arguments",address,length,prot,flags,ERR_INVALID);
    uintptr_t hint=(uintptr_t)*address;
    int arena=arena_available()&&(!hint||arena_contains(hint,length));
    uint64_t span=arena?length:round_up(length,65536);
    HANDLE section=CreateFileMappingW(INVALID_HANDLE_VALUE,NULL,PAGE_EXECUTE_READWRITE,(DWORD)(span>>32),(DWORD)span,NULL);
    if(!section)return flexible_failed("section",address,length,prot,flags,ERR_NOMEM);
    unsigned char *shadow=MapViewOfFile(section,FILE_MAP_READ|FILE_MAP_WRITE,0,0,(SIZE_T)span);
    void *view=NULL;
    if(shadow&&arena){
        AcquireSRWLockExclusive(&memory_lock);
        uint64_t at=hint&&!(hint%16384)&&arena_free(hint,length)?hint:(flags&0x10)?0:arena_find(length,16384);
        view=at?arena_map(section,0,at,length):NULL;
        ReleaseSRWLockExclusive(&memory_lock);
    }else if(shadow)view=map_view(section,span,hint,flags&0x10);
    CloseHandle(section);
    DWORD old;
    if(!view||!VirtualProtect(view,(SIZE_T)span,page_mode(prot),&old)){
        int32_t r=flexible_failed(!shadow?"shadow view":!view?"guest view":"protection",address,length,prot,flags,ERR_NOMEM);
        if(view)UnmapViewOfFile(view);if(shadow)UnmapViewOfFile(shadow);return r;
    }
    *address=view;
    register_mapping((uintptr_t)view,length,shadow,(uint32_t)(prot&3)|((prot&4)?4u:0));
    printf("Flexible mapping: virtual=0x%llx bytes=%llu name=%s\n",(unsigned long long)(uintptr_t)view,
           (unsigned long long)length,name?name:"");
    return 0;
}
int32_t GUEST_ABI memory_protect(const void *address,uint64_t length,int32_t prot){
    if(!length)return ERR_INVALID;
    uintptr_t start=(uintptr_t)address&~(uintptr_t)4095,end=((uintptr_t)address+length+4095)&~(uintptr_t)4095;
    DWORD old;return VirtualProtect((void *)start,(SIZE_T)(end-start),page_mode(prot),&old)?0:ERR_INVALID;
}
int32_t GUEST_ABI memory_query_protection(void *address,void **start,void **end,uint32_t *prot){
    MEMORY_BASIC_INFORMATION info;
    if(!VirtualQuery(address,&info,sizeof(info))||info.State!=MEM_COMMIT)return ERR_INVALID;
    if(start)*start=info.BaseAddress;
    if(end)*end=(unsigned char *)info.BaseAddress+info.RegionSize;
    if(prot){DWORD p=info.Protect&0xff;
        *prot=(p==PAGE_READONLY?1u:p==PAGE_READWRITE?3u:p==PAGE_EXECUTE_READ?5u:p==PAGE_EXECUTE_READWRITE?7u:p==PAGE_EXECUTE?4u:0u);}
    return 0;
}
// Physical ranges are bookkeeping only (the section stays committed); mappings stay valid.
int32_t GUEST_ABI memory_release_direct(int64_t start,uint64_t length){
    if(start<0||!length||(uint64_t)start%16384||length%16384)return ERR_INVALID;
    uint64_t end=(uint64_t)start+length;int32_t result=ERR_INVALID;
    AcquireSRWLockExclusive(&memory_lock);
    for(uint64_t i=0;i<allocation_count;){
        Allocation *a=&allocations[i];uint64_t a_end=a->start+a->length;
        if(a_end<=(uint64_t)start||a->start>=end){++i;continue;}
        result=0;
        if(a->start<(uint64_t)start&&a_end>end){ // split around the released range
            if(allocation_count==1024){result=ERR_NOMEM;break;}
            allocations[allocation_count++]=(Allocation){end,a_end-end};a->length=(uint64_t)start-a->start;++i;
        }else if(a->start<(uint64_t)start){a->length=(uint64_t)start-a->start;++i;}
        else if(a_end>end){a->length=a_end-end;a->start=end;++i;}
        else allocations[i]=allocations[--allocation_count];
    }
    ReleaseSRWLockExclusive(&memory_lock);return result;
}
// Unmap removes every mapping wholly inside the range from the table and gives its arena range
// back, so a later fixed mapping there succeeds. Ghost of Tsushima streams textures through
// 64 KiB mappings it unmaps and maps again at the same addresses; with unmap a no-op the fixed
// remap failed ("Direct mapping failed: hint=0x1428990000") and the file reader then wrote into
// the unmapped page (exit code 21 at 0x1420d70000, 2026-10-28). The GPU is told first, while
// the pages are still mapped. Mappings outside the arena (no placeholder API) stay as before.
int32_t GUEST_ABI memory_unmap(void *address,uint64_t length){
    static volatile LONG reported;
    if(!length)return ERR_INVALID;
    const uint64_t start=(uintptr_t)address,end=start+length<start?UINT64_MAX:start+length;
    for(;;){
        uint64_t at=0,size=0;
        AcquireSRWLockShared(&memory_lock);
        for(unsigned i=0;i<mapping_count;++i){Mapping *m=&mappings[i];
            if(m->size&&m->address>=start&&m->address+m->size<=end&&arena_count&&
               arena_index(m->address)<arena_count&&arena_used[arena_index(m->address)].start==m->address){at=m->address;size=m->size;break;}}
        void (*hook)(uint64_t,uint64_t)=unmap_hook;
        ReleaseSRWLockShared(&memory_lock);
        if(!size)break;
        if(hook)hook(at,size);
        AcquireSRWLockExclusive(&memory_lock);
        int removed=0;
        for(unsigned i=0;i<mapping_count;++i)if(mappings[i].size&&mappings[i].address==at&&mappings[i].size==size){
            if(arena_unmap(at,size)){mappings[i]=(Mapping){0,0,NULL,0};removed=1;++mapping_epoch;++mapping_generation;}
            break;}
        ReleaseSRWLockExclusive(&memory_lock);
        if(!removed){
            if(InterlockedIncrement(&reported)<=16)
                printf("Unmap failed, range kept: virtual=0x%llx bytes=%llu winerror=%lu\n",(unsigned long long)at,(unsigned long long)size,GetLastError());
            break;
        }
    }
    return 0;
}
// SceKernelBatchMapEntry.
typedef struct {void *start;uint64_t offset,length;char prot,type;int16_t reserved;int32_t operation;} BatchEntry;
_Static_assert(sizeof(BatchEntry)==32,"SceKernelBatchMapEntry layout");
int32_t GUEST_ABI memory_batch_map2(void *list,int32_t count,int32_t *done,int32_t flags){
    BatchEntry *entries=list;int32_t result=0,i=0;
    for(;i<count;++i){
        BatchEntry *e=entries?&entries[i]:NULL;
        if(!e||!e->length||e->operation<0||e->operation>4){result=ERR_INVALID;break;}
        switch(e->operation){
        case 0:result=memory_map_direct(&e->start,e->length,(uint8_t)e->prot,flags,(int64_t)e->offset,0);break;
        case 1:result=memory_unmap(e->start,e->length);break;
        case 2:result=memory_protect(e->start,e->length,(uint8_t)e->prot);break;
        case 3:result=memory_map_flexible(&e->start,e->length,(uint8_t)e->prot,flags,"batch");break;
        default:result=memory_protect(e->start,e->length,(uint8_t)e->prot);break; // type is not modelled
        }
        if(result)break;
    }
    if(done)*done=i;
    return result;
}
int32_t GUEST_ABI memory_batch_map(void *entries,int32_t count,int32_t *done){return memory_batch_map2(entries,count,done,0x10);}
