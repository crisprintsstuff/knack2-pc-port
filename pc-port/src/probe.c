// SPDX-License-Identifier: GPL-2.0-or-later
// Windows x86-64 entry probe for Knack II. The image is the stub plus, when linked, game.elf.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "runtime.h"
#include "game.h"

#if !defined(__clang__) || !defined(_M_X64)
#error Build this Windows x64 probe with clang-cl (sysv_abi required).
#endif

typedef struct { uint64_t address, size, flags; } Segment;
typedef struct { uint64_t target, kind, value, addend; } Reloc;
typedef void (__attribute__((sysv_abi)) *GuestEntry)(void *, void *);
static unsigned char *image, *traps, *objects;
static uint64_t image_size, import_count;
static char (*names)[128];
static GuestModule modules[8];
static uint64_t module_count;
static volatile LONG stage_state; // 0 idle, 1 inside game.elf, 2 game.elf's entry returned
static int32_t stage_result, stage_handle;
static const char *phase = "mapping";
static uint64_t active_module = UINT64_MAX;

static void fail(const char *message) {
    fprintf(stderr, "PROBE_ERROR: %s (Windows error %lu)\n", message, GetLastError());
    exit(1);
}
static void *checked_calloc(uint64_t n, size_t size) {
    void *p = calloc(n ? (size_t)n : 1, size);
    if (!p) fail("allocation failed");
    return p;
}
static uint64_t read64(FILE *f) {
    unsigned char b[8];
    if (fread(b, 1, 8, f) != 8) fail("truncated metadata");
    uint64_t n = 0;
    for (int i = 7; i >= 0; --i) n = (n << 8) | b[i];
    return n;
}
static size_t page_round(uint64_t n) { return ((size_t)n + 4095) & ~(size_t)4095; }
static int mapped(const Segment *s, uint64_t ns, uint64_t addr, uint64_t bytes, int exec) {
    for (uint64_t i = 0; i < ns; ++i)
        if ((!exec || (s[i].flags & 1)) && addr >= s[i].address &&
            addr - s[i].address <= s[i].size && bytes <= s[i].size - (addr - s[i].address)) return 1;
    return 0;
}
static void protect(void *p, size_t size, DWORD mode) {
    DWORD old;
    if (!VirtualProtect(p, size, mode, &old)) fail("VirtualProtect failed");
}
static DWORD permissions(uint64_t flags) {
    if (flags & 1) return (flags & 2) ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
    if (flags & 2) return PAGE_READWRITE;
    return (flags & 4) ? PAGE_READONLY : PAGE_NOACCESS;
}
static void *low_allocate(size_t size, DWORD mode) {
    // Game code may store addresses in 32-bit fields. Keep the probe below 4 GiB.
    for (uintptr_t address = 0x10000000; address + size <= UINT64_C(0xf0000000); address += 0x1000000) {
        void *p = VirtualAlloc((void *)address, size, MEM_RESERVE | MEM_COMMIT, mode);
        if (p) return p;
    }
    fail("cannot allocate low guest address space");
    return NULL;
}

// ISS_WATCH=<address>[,<address>...] (up to 4, 8-byte aligned): hardware write watchpoints (debug
// registers) on 8 bytes at each address, on every thread of the process; a thread started later
// gets them within 50 ms. Each write logs one WATCH line: the address, the value now there, the
// thread, the instruction after the write (x86 reports data breakpoints after the write) and,
// for game code, its rbp return chain. Writes from host code (a memcpy in iss-gpu.dll) show too;
// a value that changes with no WATCH line was written by the GPU or through another mapping.
// The addresses come from ISS_WATCH; this game has no recorded watch sites yet.
static uint64_t watch_address[4];
static int watch_count;
static volatile LONG watch_logged;
static void watch_location(char *out, size_t size, uint64_t address) {
    if (address >= (uintptr_t)image && address - (uintptr_t)image < image_size) {
        snprintf(out, size, "+0x%llx", (unsigned long long)(address - (uintptr_t)image)); return;
    }
    HMODULE host = NULL; char path[MAX_PATH] = "";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)(uintptr_t)address, &host) && host && GetModuleFileNameA(host, path, sizeof(path))) {
        const char *name = strrchr(path, '\\');
        snprintf(out, size, "%s+0x%llx", name ? name + 1 : path, (unsigned long long)(address - (uintptr_t)host)); return;
    }
    snprintf(out, size, "0x%llx", (unsigned long long)address);
}
static int watch_hit(EXCEPTION_POINTERS *exception) {
    CONTEXT *c = exception->ContextRecord;
    if (!watch_count || !(c->Dr6 & 0xf)) return 0;
    for (int i = 0; i < watch_count; ++i) {
        if (!(c->Dr6 & (1u << i))) continue;
        uint64_t value = 0; SIZE_T got = 0;
        ReadProcessMemory(GetCurrentProcess(), (void *)(uintptr_t)watch_address[i], &value, sizeof(value), &got);
        // Every write while there are few; past 4000 only the writes that store 0.
        if (InterlockedIncrement(&watch_logged) > 4000 && value) continue;
        char where[160]; watch_location(where, sizeof(where), c->Rip);
        char chain[256] = ""; size_t used = 0;
        uint64_t frame = c->Rbp, link[2];
        for (int depth = 0; depth < 6 && frame && !(frame & 7) && used + 24 < sizeof(chain); ++depth) {
            if (!ReadProcessMemory(GetCurrentProcess(), (void *)(uintptr_t)frame, link, sizeof(link), &got) || got != sizeof(link)) break;
            if (link[1] < (uintptr_t)image || link[1] - (uintptr_t)image >= image_size) break;
            used += (size_t)snprintf(chain + used, sizeof(chain) - used, " +0x%llx", (unsigned long long)(link[1] - (uintptr_t)image));
            if (link[0] <= frame) break;
            frame = link[0];
        }
        printf("WATCH 0x%llx = 0x%llx tid=%lu after=%s chain:%s\n", (unsigned long long)watch_address[i],
               (unsigned long long)value, GetCurrentThreadId(), where, chain);
    }
    c->Dr6 = 0;
    return 1;
}
static void watch_arm(DWORD tid) {
    HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, tid);
    if (!thread) return;
    if (SuspendThread(thread) != (DWORD)-1) {
        CONTEXT c; memset(&c, 0, sizeof(c)); c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(thread, &c)) {
            DWORD64 *slots[4] = {&c.Dr0, &c.Dr1, &c.Dr2, &c.Dr3};
            for (int i = 0; i < watch_count; ++i) {
                *slots[i] = watch_address[i];
                // Local enable, break on writes (RW=01), 8 bytes (LEN=10).
                c.Dr7 |= (DWORD64)1 << (2 * i) | (DWORD64)1 << (16 + 4 * i) | (DWORD64)2 << (18 + 4 * i);
            }
            SetThreadContext(thread, &c);
        }
        ResumeThread(thread);
    }
    CloseHandle(thread);
}
static DWORD WINAPI watch_arm_threads(void *unused) {
    (void)unused;
    static DWORD armed[8192]; static int armed_count;
    for (;;) {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 entry; entry.dwSize = sizeof(entry);
        for (BOOL more = snapshot != INVALID_HANDLE_VALUE && Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
            if (entry.th32OwnerProcessID != GetCurrentProcessId() || entry.th32ThreadID == GetCurrentThreadId()) continue;
            int known = 0;
            for (int i = 0; i < armed_count && !known; ++i) known = armed[i] == entry.th32ThreadID;
            if (known) continue;
            watch_arm(entry.th32ThreadID);
            if (armed_count < 8192) armed[armed_count++] = entry.th32ThreadID;
        }
        if (snapshot != INVALID_HANDLE_VALUE) CloseHandle(snapshot);
        Sleep(50);
    }
}
static void watch_start(void) {
    const char *spec = getenv("ISS_WATCH");
    for (const char *s = spec; s && *s && watch_count < 4;) {
        char *end; unsigned long long address = strtoull(s, &end, 16);
        if (end == s) break;
        if (address && !(address & 7)) watch_address[watch_count++] = address;
        else printf("WATCH ignored 0x%llx: not 8-byte aligned\n", address);
        s = *end == ',' ? end + 1 : end;
        if (end[0] != ',') break;
    }
    if (!watch_count) return;
    HANDLE thread = CreateThread(NULL, 0, watch_arm_threads, NULL, 0, NULL);
    printf("WATCH armed: %d address(es)%s\n", watch_count, thread ? "" : " (arming thread failed)");
    if (thread) CloseHandle(thread);
}

static LONG CALLBACK fault(EXCEPTION_POINTERS *exception) {
    DWORD exception_code=exception->ExceptionRecord->ExceptionCode;
    if (exception_code == EXCEPTION_SINGLE_STEP && watch_hit(exception)) return EXCEPTION_CONTINUE_EXECUTION;
    // Let Windows debug-print/thread-name notifications and C++ exceptions reach
    // their normal handlers. They are not guest crashes.
    if(exception_code!=EXCEPTION_ACCESS_VIOLATION && exception_code!=EXCEPTION_BREAKPOINT &&
       exception_code!=EXCEPTION_ILLEGAL_INSTRUCTION && exception_code!=EXCEPTION_IN_PAGE_ERROR &&
       exception_code!=EXCEPTION_STACK_OVERFLOW && exception_code!=EXCEPTION_INT_DIVIDE_BY_ZERO &&
       exception_code!=EXCEPTION_PRIV_INSTRUCTION)return EXCEPTION_CONTINUE_SEARCH;
    if (runtime_emulate_tls(exception)) return EXCEPTION_CONTINUE_EXECUTION;
    if (gpu_handle_fault(exception)) return EXCEPTION_CONTINUE_EXECUTION;
    // Only the first fatal fault reports: three job workers faulting on the same instruction at
    // once interleaved their PROBE_EVENT lines into one unreadable line (2026-10-28). The others
    // wait here until the first one terminates the process.
    static volatile LONG reporting;
    if (InterlockedExchange(&reporting, 1)) for (;;) Sleep(INFINITE);
    uintptr_t address = (uintptr_t)exception->ExceptionRecord->ExceptionAddress;
    uintptr_t accessed = 0;
    DWORD code = exception->ExceptionRecord->ExceptionCode;
    uint64_t id = UINT64_MAX;
    const char *status = "guest_fault";
    int exit_code = 21;
    if (code == EXCEPTION_BREAKPOINT && address >= (uintptr_t)traps &&
        address - (uintptr_t)traps < import_count * 16 && (address - (uintptr_t)traps) % 16 == 0) {
        id = (address - (uintptr_t)traps) / 16;
        status = "unsupported_import";
        exit_code = 20;
    } else if (code == EXCEPTION_ACCESS_VIOLATION && exception->ExceptionRecord->NumberParameters >= 2) {
        accessed = exception->ExceptionRecord->ExceptionInformation[1];
        if (accessed >= (uintptr_t)objects && accessed - (uintptr_t)objects < import_count * 4096) {
            id = (accessed - (uintptr_t)objects) / 4096;
            status = "unsupported_data_import";
            exit_code = 20;
        }
    }
    uint64_t caller = 0;
    SIZE_T copied;
    ReadProcessMemory(GetCurrentProcess(), (void *)exception->ContextRecord->Rsp, &caller, sizeof(caller), &copied);
    printf("PROBE_EVENT {\"status\":\"%s\",\"exception\":\"0x%08lx\",", status, code);
    printf("\"phase\":\"%s\",", phase);
    // The address an access violation touched, and whether it read (0), wrote (1) or executed (8).
    if (code == EXCEPTION_ACCESS_VIOLATION && exception->ExceptionRecord->NumberParameters >= 2)
        printf("\"access\":%llu,\"accessed\":\"0x%llx\",", (unsigned long long)exception->ExceptionRecord->ExceptionInformation[0],
               (unsigned long long)accessed);
    if (active_module != UINT64_MAX) printf("\"active_module\":%llu,", (unsigned long long)active_module);
    printf("\"rip\":\"0x%llx\",\"image_base\":\"0x%llx\",", (unsigned long long)address, (unsigned long long)(uintptr_t)image);
    if (address >= (uintptr_t)image && address - (uintptr_t)image < image_size)
        printf("\"guest_offset\":\"0x%llx\",", (unsigned long long)(address - (uintptr_t)image));
    if (caller >= (uintptr_t)image && caller - (uintptr_t)image < image_size)
        printf("\"caller_offset\":\"0x%llx\",", (unsigned long long)(caller - (uintptr_t)image));
    for (uint64_t m = 0; m < module_count; ++m)
        if (address >= (uintptr_t)image + modules[m].base &&
            address - ((uintptr_t)image + modules[m].base) < modules[m].size)
            printf("\"module_index\":%llu,\"module_offset\":\"0x%llx\",",
                   (unsigned long long)m, (unsigned long long)(address - ((uintptr_t)image + modules[m].base)));
    printf("\"tls_reads_emulated\":%llu,", (unsigned long long)runtime_tls_fault_count());
    if (id < import_count)
        printf("\"import_index\":%llu,\"symbol\":\"%s\",", (unsigned long long)id, names[id]);
    printf("\"rax\":\"0x%llx\",\"tid\":%lu,",(unsigned long long)exception->ContextRecord->Rax,GetCurrentThreadId());
    {   // More registers, and the return addresses of the frame-pointer chain (the eboot keeps
        // rbp frames) as image offsets, so a fault inside a shared helper names its callers.
        CONTEXT *c = exception->ContextRecord;
        // r8-r11 too: the fault at +0xb64121 wrote through r9, which the event did not show.
        printf("\"r8\":\"0x%llx\",\"r9\":\"0x%llx\",\"r10\":\"0x%llx\",\"r11\":\"0x%llx\",",
               (unsigned long long)c->R8, (unsigned long long)c->R9, (unsigned long long)c->R10, (unsigned long long)c->R11);
        printf("\"rbx\":\"0x%llx\",\"rcx\":\"0x%llx\",\"rdx\":\"0x%llx\",\"r12\":\"0x%llx\",\"r13\":\"0x%llx\",\"r14\":\"0x%llx\",\"r15\":\"0x%llx\",\"rsp\":\"0x%llx\",\"rbp\":\"0x%llx\",\"stack\":[",
               (unsigned long long)c->Rbx, (unsigned long long)c->Rcx, (unsigned long long)c->Rdx, (unsigned long long)c->R12,
               (unsigned long long)c->R13, (unsigned long long)c->R14, (unsigned long long)c->R15, (unsigned long long)c->Rsp,
               (unsigned long long)c->Rbp);
        uint64_t frame = c->Rbp, link[2];
        for (int depth = 0; depth < 24 && frame && !(frame & 7); ++depth) {
            if (!ReadProcessMemory(GetCurrentProcess(), (void *)frame, link, sizeof(link), &copied) || copied != sizeof(link)) break;
            if (link[1] < (uintptr_t)image || link[1] - (uintptr_t)image >= image_size) break;
            printf("%s\"0x%llx\"", depth ? "," : "", (unsigned long long)(link[1] - (uintptr_t)image));
            if (link[0] <= frame) break;
            frame = link[0];
        }
        printf("],");
        // Outside the image: name the host module, and scan the stack for return addresses into
        // the image (host code keeps no rbp chain), newest first.
        if (address < (uintptr_t)image || address - (uintptr_t)image >= image_size) {
            HMODULE host = NULL; char path[MAX_PATH] = "";
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCSTR)(uintptr_t)address, &host) && host) {
                GetModuleFileNameA(host, path, sizeof(path));
                const char *name = strrchr(path, '\\');
                printf("\"host_module\":\"%s\",\"host_offset\":\"0x%llx\",", name ? name + 1 : path,
                       (unsigned long long)(address - (uintptr_t)host));
            }
            uint64_t words[512]; SIZE_T got = 0;
            ReadProcessMemory(GetCurrentProcess(), (void *)c->Rsp, words, sizeof(words), &got);
            printf("\"stack_scan\":[");
            int found = 0;
            for (size_t i = 0; i < got / 8 && found < 12; ++i)
                if (words[i] >= (uintptr_t)image && words[i] - (uintptr_t)image < image_size)
                    printf("%s\"0x%llx\"", found++ ? "," : "", (unsigned long long)(words[i] - (uintptr_t)image));
            printf("],");
        }
    }
    printf("\"rdi\":\"0x%llx\",\"rsi\":\"0x%llx\"}\n",
           (unsigned long long)exception->ContextRecord->Rdi, (unsigned long long)exception->ContextRecord->Rsi);
    fflush(stdout);
    // A fatal diagnostic trap cannot unwind concurrent guest/GPU workers safely.
    // Terminate only this isolated probe after flushing its evidence; no DLL destructors.
    TerminateProcess(GetCurrentProcess(),(UINT)exit_code);
    return EXCEPTION_CONTINUE_SEARCH;
}

// The hang report goes to hang-report.txt next to the executable (appended), not the console:
// PowerShell drops a redirected program's output when Ctrl+C ends the pipeline.
static void hang_report(const char *why, int resume) {
    char path[MAX_PATH]; GetModuleFileNameA(NULL, path, sizeof(path));
    char *slash = strrchr(path, '\\'); if (slash) strcpy(slash + 1, "hang-report.txt");
    // Each report replaces the previous one: the newest picture of the threads is the useful one.
    HANDLE out = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (out == INVALID_HANDLE_VALUE) out = GetStdHandle(STD_ERROR_HANDLE);
    runtime_report_write(out, "HANG_REPORT begin (%s) tick_ms=%llu\r\n", why, (unsigned long long)GetTickCount64());
    // Memory dumps first: the thread walk can get stuck partway (it stopped after a few threads in
    // several reports, 2026-10-28), and then the dumps asked for were never written.
    runtime_report_dumps(out);
    runtime_report_threads(out, resume);
    runtime_report_write(out, "HANG_REPORT end\r\n");
    if (out != GetStdHandle(STD_ERROR_HANDLE)) CloseHandle(out);
}
// Ctrl+C or Ctrl+Break: report every guest thread, then exit, so a hang shows who waits where.
static BOOL WINAPI console_break(DWORD event) {
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT) return FALSE;
    hang_report("ctrl+c", 0);
    fflush(NULL);
    ExitProcess(130);
}
// ISS_HANG_REPORT=<seconds>: a report every <seconds> while the game runs (the file always holds
// the latest), so closing the window at any time after the first one leaves a recent report.
static DWORD WINAPI hang_timer(void *seconds) {
    for (;;) {
        Sleep((DWORD)(uintptr_t)seconds * 1000);
        hang_report("timer", 1);
    }
}
// Second stage. The stub is 0x61a4 bytes and the measured inventory gives it two libkernel
// imports and one libSceSystemService import; the strings in it name /app0/game.elf. The stub
// was not disassembled in this tree (the dump is not here), so this does not replay its
// instructions. link_modules has already parsed game.elf, relocated it and bound its imports,
// the same work libkernel would do inside sceKernelLoadStartModule, and marked it deferred.
// When the stub makes that call, this runs DT_INIT if the image has one and then the entry at
// module base + GAME_MAIN_ENTRY. The call uses the libkernel module-start contract
// (args, argp, NULL), which is what LoadStartModule passes to a loaded image. It is not a
// trace of the bytes at +0x4fa0. A game entry that never returns never comes back to the stub.
int32_t runtime_start_deferred_module(uint64_t argc, const void *argv, int32_t *result) {
    const int32_t err_noent = (int32_t)UINT32_C(0x80020002);
    const int32_t err_invalid = (int32_t)UINT32_C(0x80020016);
    if (!image) return err_noent;
    uint64_t found = UINT64_MAX;
    for (uint64_t i = 0; i < module_count; ++i)
        if (modules[i].flags & 1) {
            if (found != UINT64_MAX) return err_invalid;
            found = i;
        }
    if (found == UINT64_MAX) return err_noent;
    if (stage_state == 2) {
        if (result) *result = stage_result;
        return stage_handle;
    }
    if (InterlockedCompareExchange(&stage_state, 1, 0) != 0) return err_invalid;
    GuestModule *m = &modules[found];
    fprintf(stderr, "STAGE2 game.elf base=+0x%llx entry=+0x%x args=%llu\n",
            (unsigned long long)m->base, GAME_MAIN_ENTRY, (unsigned long long)argc);
    if (m->init != UINT64_MAX) {
        active_module = found;
        phase = "game_init";
        // DT_INIT is a constructor, not a function with a status: _init walks DT_INIT_ARRAY and
        // falls through to pop/pop/ret without ever setting eax, so its return value is whatever the
        // last constructor or the caller left there. game.elf's DT_INIT_ARRAY is empty (start == end
        // == 0x127b870) and the call returned -1 from a function that returns nothing, which used to
        // be treated as a failed start and made the stub raise 0xa0020002. Call it as void.
        typedef void (GUEST_ABI *ModuleInit)(uint64_t, const void *, void *);
        ((ModuleInit)(image + m->init))(argc, argv, NULL);
        fprintf(stderr, "STAGE2 game.elf initializer returned\n");
    }
    active_module = found;
    phase = "game_entry";
    // The guest entry takes rdi = &{count, argv[]}, not the count itself: game.elf +0x80 starts
    // "mov (%rdi),%r14d; lea 0x8(%rdi),%r15", so passing argc loaded 4 as a pointer and faulted on
    // address 0x4. The stub is entered the same way in main below. The block stays on this frame:
    // the call does not return while the game runs, and the strings it points at are the caller's.
    struct { uint64_t count; const char *args[65]; } entry_args;
    entry_args.count = argc > 64 ? 64 : argc;
    for (uint64_t i = 0; i < entry_args.count; ++i)
        entry_args.args[i] = argv ? ((const char *const *)argv)[i] : NULL;
    typedef int (GUEST_ABI *ModuleStart)(void *, void *);
    int started = ((ModuleStart)(image + m->base + GAME_MAIN_ENTRY))(&entry_args, NULL);
    stage_result = started;
    stage_handle = (int32_t)m->tls_id;
    if (result) *result = started;
    stage_state = 2;
    fprintf(stderr, "STAGE2 game.elf entry returned %d handle=%d\n", started, stage_handle);
    return stage_handle;
}

int main(int argc, char **argv) {
    SetConsoleCtrlHandler(console_break, TRUE);
    {   const char *after = getenv("ISS_HANG_REPORT"); int seconds = after ? atoi(after) : 0;
        if (seconds > 0) { HANDLE timer = CreateThread(NULL, 0, hang_timer, (void *)(uintptr_t)seconds, 0, NULL);
            printf("HANG_REPORT armed: %d s (%s)\n", seconds, timer ? "timer started" : "timer thread failed"); fflush(stdout);
            if (timer) CloseHandle(timer); } }
    setvbuf(stdout,NULL,_IONBF,0);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    if (argc != 3 || (strcmp(argv[2], "--map-only") && strcmp(argv[2], "--entry"))) {
        fprintf(stderr, "Usage: iss-probe.exe boot.bin --map-only|--entry\n");
        return 2;
    }
    SYSTEM_INFO sys;
    GetSystemInfo(&sys);
    if (sys.dwPageSize != 4096) fail("unsupported host page size");
    FILE *f = fopen(argv[1], "rb");
    if (!f) fail("cannot open prepared image");
    char magic[8];
    if (fread(magic, 1, 8, f) != 8 || (memcmp(magic, "ISSPRB01", 8) && memcmp(magic, "ISSPRB02", 8))) fail("bad boot magic");
    int linked = !memcmp(magic, "ISSPRB02", 8);
    image_size = read64(f);
    uint64_t entry = read64(f), ns = read64(f), nr = read64(f);
    import_count = read64(f);
    if (!image_size || image_size > 512 * 1024 * 1024 || !ns || ns > 64 ||
        nr > 1000000 || import_count > 10000) fail("metadata limits exceeded");
    uint64_t binding_count = 0, procparam = 0;
    uint64_t *bindings = checked_calloc(import_count, sizeof(*bindings));
    uint64_t *binding_kinds = checked_calloc(import_count, sizeof(*binding_kinds));
    if (linked) {
        module_count = read64(f); binding_count = read64(f); procparam = read64(f);
        if (!module_count || module_count > 8 || binding_count > import_count) fail("invalid module/binding count");
        _Static_assert(sizeof(GuestModule) == 9 * sizeof(uint64_t), "module record is nine qwords");
        uint64_t deferred = 0;
        for (uint64_t i = 0; i < module_count; ++i) {
            uint64_t *fields = (uint64_t *)&modules[i];
            for (int j = 0; j < 9; ++j) fields[j] = read64(f);
            GuestModule *m = &modules[i];
            if (m->base >= image_size || !m->size || m->size > image_size - m->base ||
                m->tls_id != i + 1 || m->tls_filesz > m->tls_memsz || m->tls_memsz > 1024 * 1024 ||
                !m->tls_align || m->tls_align > 4096 || (m->tls_align & (m->tls_align - 1)) ||
                (m->flags & ~UINT64_C(1))) fail("invalid module metadata");
            if (m->flags & 1) ++deferred;
        }
        // A linked CUSA08014 image always carries game.elf, and only that module is deferred.
        if (deferred != 1) fail("linked image must contain one deferred game.elf");
        for (uint64_t i = 0; i < binding_count; ++i) {
            uint64_t index = read64(f), address = read64(f), kind = read64(f);
            if (index >= import_count || address >= image_size || (kind != 1 && kind != 2) || binding_kinds[index])
                fail("invalid native binding");
            bindings[index] = address; binding_kinds[index] = kind;
        }
    }
    Segment *segments = checked_calloc(ns, sizeof(*segments));
    Reloc *relocs = checked_calloc(nr, sizeof(*relocs));
    names = checked_calloc(import_count, sizeof(*names));
    for (uint64_t i = 0; i < ns; ++i) {
        Segment *s = &segments[i];
        s->address = read64(f); s->size = read64(f); s->flags = read64(f);
        if (s->address > image_size || !s->size || s->size > image_size - s->address ||
            s->address % 4096 || s->flags > 7) fail("invalid segment");
        for (uint64_t j = 0; j < i; ++j)
            if (s->address < segments[j].address + page_round(segments[j].size) &&
                segments[j].address < s->address + page_round(s->size)) fail("overlapping segment pages");
    }
    if (!mapped(segments, ns, entry, 1, 1)) fail("entry is not executable");
    if (linked) {
        if (entry != modules[0].base + GAME_STUB_ENTRY) fail("stub entry is not GAME_STUB_ENTRY");
        for (uint64_t i = 0; i < module_count; ++i)
            if ((modules[i].flags & 1) &&
                !mapped(segments, ns, modules[i].base + GAME_MAIN_ENTRY, 1, 1))
                fail("game.elf entry is not executable");
        if (!mapped(segments, ns, procparam, 64, 0)) fail("invalid procparam");
        for (uint64_t i = 0; i < module_count; ++i) {
            GuestModule *m = &modules[i];
            if ((m->init != UINT64_MAX && !mapped(segments, ns, m->init, 1, 1)) ||
                !mapped(segments, ns, m->tls_address, m->tls_memsz, 0)) fail("invalid module init/TLS mapping");
        }
        for (uint64_t i = 0; i < import_count; ++i)
            if (binding_kinds[i] && !mapped(segments, ns, bindings[i], 1, binding_kinds[i] == 1))
                fail("invalid native binding mapping");
    }
    if (fread(names, 128, (size_t)import_count, f) != import_count) fail("truncated import names");
    for (uint64_t i = 0; i < import_count; ++i) {
        if (!names[i][0] || !memchr(names[i], 0, 128)) fail("invalid import name");
        for (char *p = names[i]; *p; ++p)
            if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                  (*p >= '0' && *p <= '9') || *p == '+' || *p == '-' || *p == '#' || *p == '_'))
                fail("unexpected import spelling");
    }
    for (uint64_t i = 0; i < nr; ++i) {
        Reloc *r = &relocs[i];
        r->target = read64(f); r->kind = read64(f); r->value = read64(f); r->addend = read64(f);
        if (!mapped(segments, ns, r->target, 8, 0) || r->kind > 2 ||
            (r->kind && r->value >= import_count) || (!r->kind && r->value >= image_size) ||
            (r->kind != 2 && r->addend) || r->addend >= 4096) fail("invalid relocation");
    }
    image = memory_allocate_image(image_size);
    if(!image)fail("cannot allocate shared image backing");
    if (fread(image, 1, (size_t)image_size, f) != image_size || fgetc(f) != EOF) fail("wrong image payload length");
    fclose(f);
    size_t trap_bytes = page_round(import_count ? import_count * 16 : 1);
    traps = low_allocate(trap_bytes, PAGE_READWRITE);
    memset(traps, 0xcc, trap_bytes);
    for (uint64_t i = 0; i < import_count; ++i) {
        traps[i * 16] = 0xcc; // INT3: Windows VEH identifies the exact import.
        traps[i * 16 + 1] = 0x0f; traps[i * 16 + 2] = 0x0b; // UD2 if unexpectedly resumed.
    }
    objects = low_allocate(page_round(import_count ? import_count * 4096 : 1), PAGE_NOACCESS);
    if (linked) runtime_configure(image, image_size, modules, module_count, procparam);
    for(uint64_t i=0;i<ns;++i)memory_register_image(segments[i].address,segments[i].size,(uint32_t)segments[i].flags);
    if (!AddVectoredExceptionHandler(1, fault)) fail("cannot install diagnostic exception handler");
    watch_start(); // after the handler: a hit before it would be an unhandled single-step
    if(linked&&!strcmp(argv[2],"--entry")){
        uint32_t sdk;memcpy(&sdk,image+procparam+16,4);
        if(gpu_start(sdk,(uint64_t)(uintptr_t)image))fail("graphics initialization failed");
    }
    for (uint64_t i = 0; i < nr; ++i) {
        Reloc *r = &relocs[i];
        uintptr_t value = r->kind == 0 ? (uintptr_t)image + r->value :
                          r->kind == 1 ? (uintptr_t)traps + r->value * 16 :
                                         (uintptr_t)objects + r->value * 4096 + r->addend;
        if (linked && r->kind) {
            if (binding_kinds[r->value]) {
                if (binding_kinds[r->value] != r->kind) fail("native binding type mismatch");
                value = (uintptr_t)image + bindings[r->value] + r->addend;
                if (r->kind == 1) { // optional diagnostic wrappers around a native export
                    uintptr_t wrapper = runtime_wrap_binding(names[r->value], value - r->addend);
                    if (wrapper) value = wrapper + r->addend;
                }
            }
            uintptr_t host = runtime_resolve(names[r->value], r->kind == 2);
            if (host) value = host + r->addend;
        }
        memcpy(image + r->target, &value, 8);
    }
    protect(image, page_round(image_size), PAGE_NOACCESS);
    for (uint64_t i = 0; i < ns; ++i)
        protect(image + segments[i].address, page_round(segments[i].size), permissions(segments[i].flags));
    protect(traps, trap_bytes, PAGE_EXECUTE_READ);
    if (!FlushInstructionCache(GetCurrentProcess(), NULL, 0)) fail("instruction cache flush failed");
    printf("Mapped eboot: base=0x%llx entry=+0x%llx bytes=%llu relocations=%llu imports=%llu\n",
           (unsigned long long)(uintptr_t)image, (unsigned long long)entry, (unsigned long long)image_size,
           (unsigned long long)nr, (unsigned long long)import_count);
    if (!strcmp(argv[2], "--map-only")) {
        puts("PROBE_EVENT {\"status\":\"mapped_only\",\"guest_executed\":false}");
        VirtualFree(objects, 0, MEM_RELEASE); VirtualFree(traps, 0, MEM_RELEASE); UnmapViewOfFile(image);
        free(names); free(relocs); free(segments); free(bindings); free(binding_kinds);
        return 0;
    }
    if (linked) {
        // Inherited installers patch inFAMOUS and Ghost of Tsushima addresses. They are not
        // called. native_install_hooks itself also refuses unless ISS_LEGACY_HOOKS=1.
        (void)native_install_hooks;
        runtime_tls_attach();
        phase = "module_init";
        for (uint64_t m = 1; m < module_count; ++m) {
            // game.elf waits for the stub's sceKernelLoadStartModule. Its TLS is already
            // reserved above; only its code is held back.
            if (modules[m].flags & 1) continue;
            if (modules[m].init == UINT64_MAX) continue;
            active_module = m;
            printf("Starting native module %llu at +0x%llx\n", (unsigned long long)m, (unsigned long long)modules[m].init);
            fflush(stdout);
            // Same contract as game.elf above: DT_INIT is void, so a non-zero eax is not a failure.
            typedef void (GUEST_ABI *ModuleInit)(uint64_t, void *, void *);
            ((ModuleInit)(image + modules[m].init))(0, NULL, NULL);
            printf("Native module %llu initializer returned\n", (unsigned long long)m);
        }
    }
    phase = "guest_entry";
    active_module = UINT64_MAX;
    struct { uint64_t count; const char *args[2]; } guest_args = {1, {"/app0/eboot.bin", NULL}};
    puts("Entering original x86-64 eboot; unresolved imports remain diagnostic traps.");
    fflush(stdout);
    // clang emits the Windows -> SysV transition. The stub's startup reads RDI/RSI.
    // A linked image already has guest TLS attached, including game.elf's block.
    ((GuestEntry)(image + entry))(&guest_args, NULL);
    puts("PROBE_EVENT {\"status\":\"entry_returned\"}");
    return 22;
}
