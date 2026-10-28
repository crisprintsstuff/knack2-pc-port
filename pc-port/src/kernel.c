// SPDX-License-Identifier: GPL-2.0-or-later
// libkernel services the game's libc.prx and Fios2 import, and a few the eboot does, that have no
// richer home: process exit, signals, resource usage, debug exceptions, module queries, sanitizer
// hooks, module loading and the memory calls of memory.c.
#include "runtime.h"
#include "game.h"
#include <stdio.h>
#include <string.h>

#define ERR_SRCH ((int32_t)UINT32_C(0x80020003))
#define ERR_NOENT ((int32_t)UINT32_C(0x80020002))
#define ERR_INVALID ((int32_t)UINT32_C(0x80020016))
static void note(volatile LONG *seen, const char *what) {
    if (!InterlockedExchange(seen, 1)) fprintf(stderr, "KERNEL %s\n", what);
}

// ---- process -----------------------------------------------------------------------------------
static void GUEST_ABI kernel_exit(int status) { printf("Guest _exit(%d)\n", status); fflush(NULL); ExitProcess((UINT)status); }
// Raised by the game for fatal errors; the code is the useful part of the report.
static void GUEST_ABI raise_exception(uint32_t code, uint64_t a, uint64_t b) {
    fprintf(stderr, "RUNTIME_ERROR: guest raised debug exception 0x%08x (0x%llx, 0x%llx)\n", code,
            (unsigned long long)a, (unsigned long long)b);
    fflush(NULL);
    ExitProcess(24);
}
static int32_t GUEST_ABI print_backtrace(void) { static volatile LONG seen; note(&seen, "sceKernelPrintBacktraceWithModuleInfo"); return 0; }
// FreeBSD struct rusage: two timevals, then fourteen longs.
static int GUEST_ABI get_rusage(int who, uint8_t *out) {
    (void)who;
    if (!out) { *runtime_errno() = 14; return -1; }
    memset(out, 0, 144);
    FILETIME created, exited, kernel, user;
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        uint64_t u = (((uint64_t)user.dwHighDateTime << 32) | user.dwLowDateTime) / 10;
        uint64_t k = (((uint64_t)kernel.dwHighDateTime << 32) | kernel.dwLowDateTime) / 10;
        int64_t times[4] = {(int64_t)(u / 1000000), (int64_t)(u % 1000000), (int64_t)(k / 1000000), (int64_t)(k % 1000000)};
        memcpy(out, times, sizeof(times));
    }
    return 0;
}
// Signals are never delivered; masks and handlers are accepted and reported as defaults.
static int GUEST_ABI sigprocmask(int how, const void *set, uint8_t *old) { (void)how; (void)set; if (old) memset(old, 0, 16); return 0; }
static uint64_t GUEST_ABI signal_handler(int sig, uint64_t handler) { (void)sig; (void)handler; return 0; }
static int GUEST_ABI is_signal_return(const void *context) { (void)context; return 0; }

// ---- thread exit hooks of libc ------------------------------------------------------------------
// libc registers these to count and report thread-exit handlers; the destructor hook itself is
// kept by threads.c (_sceKernelSetThreadDtors).
static void *atexit_count, *atexit_report;
static void GUEST_ABI set_atexit_count(void *fn) { atexit_count = fn; }
static void GUEST_ABI set_atexit_report(void *fn) { atexit_report = fn; }
static int32_t GUEST_ABI rtld_atexit_change(void) { return 0; }
static void GUEST_ABI cxa_finalize_module(const void *info) { (void)info; } // modules never unload

// ---- modules -----------------------------------------------------------------------------------
// Elf64_Phdr inside a dl_phdr_info { dlpi_addr, dlpi_name, dlpi_phdr, dlpi_phnum }.
typedef struct { uint32_t type, flags; uint64_t offset, vaddr, paddr, filesz, memsz, align; } Phdr;
typedef struct { uint64_t addr; const char *name; const Phdr *phdr; uint16_t phnum; } PhdrInfo;
static int GUEST_ABI phdr_match_addr(const PhdrInfo *info, uint64_t address) {
    if (!info || !info->phdr) return 0;
    for (uint16_t i = 0; i < info->phnum; ++i) {
        const Phdr *p = &info->phdr[i];
        uint64_t start = info->addr + p->vaddr;
        if (p->type == 1 && address >= start && address - start < p->memsz) return 1;
    }
    return 0;
}
// The eboot imports no C++ throw or _Unwind entry, so unwind tables are not published (they
// would need the PT_GNU_EH_FRAME of each module in the boot image).
static int32_t GUEST_ABI module_info_for_unwind(uint64_t address, int flags, void *info) {
    static volatile LONG seen;
    (void)flags; (void)info;
    if (!InterlockedExchange(&seen, 1)) fprintf(stderr, "KERNEL unwind info requested for 0x%llx (not published)\n", (unsigned long long)address);
    return ERR_SRCH;
}
static int32_t GUEST_ABI module_info_from_address(uint64_t address, int flags, void *info) { return module_info_for_unwind(address, flags, info); }
// The stub's load of /app0/game.elf. Other paths stay unavailable: this port has not implemented
// a general module loader, only the one request the CUSA08014 stub exists to make. See
// runtime_start_deferred_module for why the image is staged at link time instead of parsed here.
static int is_game_elf(const char *path) {
    if (!path) return 0;
    if (!strcmp(path, GAME_MAIN_PATH)) return 1;
    char alt[sizeof(GAME_MAIN_PATH)];
    for (size_t i = 0; i < sizeof(GAME_MAIN_PATH); ++i)
        alt[i] = GAME_MAIN_PATH[i] == '/' ? '\\' : GAME_MAIN_PATH[i];
    return !strcmp(path, alt);
}
static int32_t GUEST_ABI load_start_module(const char *path, uint64_t argc, const void *argv, uint32_t flags, const void *option, int32_t *result) {
    (void)flags; (void)option;
    if (is_game_elf(path)) {
        fprintf(stderr, "KERNEL sceKernelLoadStartModule(%s)\n", path);
        return runtime_start_deferred_module(argc, argv, result);
    }
    fprintf(stderr, "KERNEL sceKernelLoadStartModule(%s): not available\n", path ? path : "(null)");
    if (result) *result = 0;
    return ERR_NOENT;
}
static int32_t GUEST_ABI dlsym(int32_t handle, const char *symbol, void **address) {
    (void)handle; if (address) *address = NULL;
    fprintf(stderr, "KERNEL sceKernelDlsym(%d, %s): no such module\n", handle, symbol ? symbol : "(null)");
    return ERR_SRCH;
}
static int32_t GUEST_ABI sysmodule_load_internal(uint32_t id, uint64_t argc, const void *argv, uint32_t flags, int32_t *result) {
    (void)argc; (void)argv; (void)flags;
    printf("Sysmodule 0x%x treated as loaded\n", id);
    if (result) *result = 0;
    return 0;
}
static int32_t GUEST_ABI segment_info(void *info) { static volatile LONG seen; (void)info; note(&seen, "sceKernelInternalMemoryGetModuleSegmentInfo unavailable"); return ERR_INVALID; }

// ---- sanitizers and memory ------------------------------------------------------------------------
static uint64_t GUEST_ABI no_replacement(void) { return 0; }
static int32_t GUEST_ABI prt_aperture(int32_t id, uint64_t *address, uint64_t *size) {
    if (id < 0 || id >= 3) return ERR_INVALID;
    if (address) *address = 0;
    if (size) *size = 0;
    return 0;
}
static int32_t GUEST_ABI mlock(const void *address, uint64_t length) { (void)address; (void)length; return 0; }

static const char progname[] = "eboot.bin";
static const char *progname_pointer = progname;

static const struct { const char *api; const void *fn; } table[] = {
#define E(name, fn) {name, (const void *)fn}
    E("_exit", kernel_exit), E("sceKernelDebugRaiseException", raise_exception),
    E("sceKernelDebugRaiseExceptionOnReleaseMode", raise_exception),
    E("sceKernelPrintBacktraceWithModuleInfo", print_backtrace), E("getrusage", get_rusage),
    E("_sigprocmask", sigprocmask), E("signal", signal_handler), E("_is_signal_return", is_signal_return),
    E("_sceKernelSetThreadAtexitCount", set_atexit_count), E("_sceKernelSetThreadAtexitReport", set_atexit_report),
    E("_sceKernelRtldThreadAtexitIncrement", rtld_atexit_change), E("_sceKernelRtldThreadAtexitDecrement", rtld_atexit_change),
    E("__pthread_cxa_finalize", cxa_finalize_module), E("__elf_phdr_match_addr", phdr_match_addr),
    E("sceKernelGetModuleInfoForUnwind", module_info_for_unwind), E("sceKernelGetModuleInfoFromAddr", module_info_from_address),
    E("sceSysmoduleGetModuleInfoForUnwind", module_info_for_unwind),
    E("sceSysmoduleLoadModuleInternalWithArg", sysmodule_load_internal),
    E("sceKernelLoadStartModule", load_start_module), E("sceKernelDlsym", dlsym),
    E("sceKernelInternalMemoryGetModuleSegmentInfo", segment_info),
    E("sceKernelGetSanitizerMallocReplaceExternal", no_replacement), E("sceKernelGetSanitizerNewReplaceExternal", no_replacement),
    E("sceKernelIsAddressSanitizerEnabled", no_replacement),
    E("sceKernelGetPrtAperture", prt_aperture), E("sceKernelMlock", mlock),
    E("sceKernelMapNamedFlexibleMemory", memory_map_flexible), E("sceKernelMprotect", memory_protect),
    E("sceKernelQueryMemoryProtection", memory_query_protection), E("sceKernelReleaseDirectMemory", memory_release_direct),
    E("sceKernelMunmap", memory_unmap), E("sceKernelBatchMap", memory_batch_map), E("sceKernelBatchMap2", memory_batch_map2),
#undef E
};
static int is_kernel(const char *identity) {
    return strstr(identity, "#libkernel#1#libkernel#257") || strstr(identity, "#libScePosix#1#libkernel#257") ||
           strstr(identity, "#libSceSysmodule#1#libSceSysmodule#257");
}
uintptr_t kernel_resolve(const char *identity) {
    if (!is_kernel(identity)) return 0;
    const char *api = runtime_api_name(identity);
    for (size_t i = 0; i < sizeof(table) / sizeof(*table); ++i)
        if (!strcmp(api, table[i].api)) return (uintptr_t)table[i].fn;
    return 0;
}
uintptr_t kernel_resolve_data(const char *identity) {
    return is_kernel(identity) && !strcmp(runtime_api_name(identity), "__progname") ? (uintptr_t)&progname_pointer : 0;
}
