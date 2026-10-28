// SPDX-License-Identifier: GPL-2.0-or-later
#include "runtime.h"
#include <tlhelp32.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
typedef void *(GUEST_ABI *Entry)(void *);
typedef void (GUEST_ABI *Destructor)(void *);
typedef struct Attribute {
    uint64_t stacksize, affinity; int policy, priority, inherit, detached, active;
    void *stackaddr; struct Attribute *next;
} Attribute;
typedef struct Thread {
    HANDLE host; DWORD id; Entry entry; void *arg, *result;
    Attribute attr; int joined, detached; char name[64]; struct Thread *next;
} Thread;
static SRWLOCK registry = SRWLOCK_INIT;
static Attribute *attrs;
static Thread *threads;
static _Thread_local Thread *current;
static struct { DWORD slot; Destructor dtor; int active; } keys[1024];
static uint32_t key_count;
static int32_t error(int n) { return n ? (int32_t)(UINT32_C(0x80020000) | (unsigned)n) : 0; }
static Attribute defaults(void) {
    Attribute a = {0}; a.stacksize = 1024 * 1024; a.affinity = 127;
    a.priority = 700; a.policy = 1; a.inherit = 4; a.active = 1; return a;
}
static Attribute *attribute(Attribute **p) {
    if (!p || !*p) return NULL;
    AcquireSRWLockShared(&registry); Attribute *a = attrs;
    while (a && (a != *p || !a->active)) a = a->next;
    ReleaseSRWLockShared(&registry); return a;
}
static Thread *thread(Thread *p) {
    AcquireSRWLockShared(&registry); Thread *t = threads;
    while (t && t != p) t = t->next;
    ReleaseSRWLockShared(&registry); return t;
}
static Thread *GUEST_ABI self(void) {
    if (current) return current;
    Thread *t = calloc(1, sizeof(*t)); if (!t) ExitProcess(23);
    t->attr = defaults(); t->id = GetCurrentThreadId();
    ULONG_PTR low, high; GetCurrentThreadStackLimits(&low, &high);
    t->attr.stackaddr = (void *)low; t->attr.stacksize = high - low;
    strcpy(t->name, "main");
    AcquireSRWLockExclusive(&registry); t->next = threads; threads = t; current = t;
    ReleaseSRWLockExclusive(&registry); runtime_set_thread_handle(t); return t;
}
static int32_t GUEST_ABI attr_init(Attribute **p) {
    if (!p) return error(22); Attribute *a = calloc(1, sizeof(*a)); if (!a) return error(12);
    *a = defaults(); AcquireSRWLockExclusive(&registry); a->next = attrs; attrs = a; *p = a;
    ReleaseSRWLockExclusive(&registry); return 0;
}
static int32_t GUEST_ABI attr_destroy(Attribute **p) {
    Attribute *a = attribute(p); if (!a) return error(22); a->active = 0; *p = NULL; return 0;
}
static int32_t GUEST_ABI attr_get(Thread *t, Attribute **p) {
    t = thread(t); Attribute *a = attribute(p); if (!t || !a) return error(22);
    Attribute *next = a->next; *a = t->attr; a->next = next; a->active = 1; return 0;
}
static int32_t GUEST_ABI attr_stack(Attribute **p, uint64_t size) {
    Attribute *a = attribute(p); if (!a || size < 16384 || size > UINT64_C(0x40000000)) return error(22);
    a->stacksize = size; return 0;
}
static int32_t GUEST_ABI attr_affinity(Attribute **p, uint64_t mask) {
    Attribute *a = attribute(p); if (!a || !mask) return error(22); a->affinity = mask; return 0;
}
static int32_t GUEST_ABI attr_get_affinity(Attribute **p, uint64_t *mask) {
    Attribute *a = attribute(p); if (!a || !mask) return error(22); *mask = a->affinity; return 0;
}
static int32_t GUEST_ABI attr_inherit(Attribute **p, int n) {
    Attribute *a = attribute(p); if (!a || (n != 0 && n != 4)) return error(22); a->inherit = n; return 0;
}
static int32_t GUEST_ABI attr_policy(Attribute **p, int n) {
    Attribute *a = attribute(p); if (!a || n < 1 || n > 3) return error(22); a->policy = n; return 0;
}
static int32_t GUEST_ABI attr_priority(Attribute **p, const int *n) {
    Attribute *a = attribute(p); if (!a || !n || *n < 256 || *n > 959) return error(22); a->priority = *n; return 0;
}
static int32_t GUEST_ABI attr_get_priority(Attribute **p, int *n) {
    Attribute *a = attribute(p); if (!a || !n) return error(22); *n = a->priority; return 0;
}
static int32_t GUEST_ABI attr_detach(Attribute **p, int n) {
    Attribute *a = attribute(p); if (!a || (n != 0 && n != 1)) return error(22); a->detached = n; return 0;
}
static void destructors(void) {
    for (int pass = 0; pass < 4; ++pass) {
        int called = 0;
        for (uint32_t i = 0; i < key_count; ++i) {
            AcquireSRWLockShared(&registry);
            DWORD slot = keys[i].slot; Destructor fn = keys[i].dtor;
            int active = keys[i].active; ReleaseSRWLockShared(&registry);
            if (!active || !fn) continue;
            void *value = TlsGetValue(slot);
            if (value) { TlsSetValue(slot, NULL); fn(value); called = 1; }
        }
        if (!called) break;
    }
}
// libc's hook for thread_local destructors (_sceKernelSetThreadDtors), run as a thread ends.
typedef void (GUEST_ABI *ThreadDtors)(void);
static ThreadDtors thread_dtors;
static void GUEST_ABI set_thread_dtors(ThreadDtors fn) { thread_dtors = fn; }
static void thread_ending(void) { if (thread_dtors) thread_dtors(); destructors(); }
static DWORD WINAPI start(void *arg) {
    Thread *t = arg; current = t; runtime_tls_attach(); runtime_set_thread_handle(t);
    t->result = t->entry(t->arg); thread_ending(); runtime_tls_detach(); return 0;
}
static int32_t GUEST_ABI create(Thread **out, Attribute **attr, Entry entry, void *arg, const char *name) {
    if (!out || !entry) return error(22);
    Attribute *a = attr && *attr ? attribute(attr) : NULL;
    if (attr && *attr && !a) return error(22);
    Thread *t = calloc(1, sizeof(*t)); if (!t) return error(12);
    t->attr = a ? *a : defaults(); t->detached = t->attr.detached;
    t->entry = entry; t->arg = arg;
    if (t->attr.stackaddr) { free(t); return error(45); } // custom guest stacks require a stack bridge.
    if (name) strncpy(t->name, name, sizeof(t->name) - 1);
    t->host = CreateThread(NULL, (SIZE_T)(t->attr.stacksize + 256 * 1024), start, t,
                          CREATE_SUSPENDED | STACK_SIZE_PARAM_IS_A_RESERVATION, &t->id);
    if (!t->host) { free(t); return error(35); }
    AcquireSRWLockExclusive(&registry); t->next = threads; threads = t; *out = t;
    ReleaseSRWLockExclusive(&registry); ResumeThread(t->host);
    printf("Created native guest thread: %s id=%lu entry=0x%llx arg=0x%llx\n", t->name, t->id,
           (unsigned long long)(uintptr_t)entry, (unsigned long long)(uintptr_t)arg); return 0;
}
static int32_t GUEST_ABI join(Thread *p, void **result) {
    Thread *t = thread(p); if (!t || t->detached || t->joined) return error(22);
    if (t == self()) return error(11);
    if (WaitForSingleObject(t->host, INFINITE) != WAIT_OBJECT_0) return error(22);
    if (result) *result = t->result; t->joined = 1; CloseHandle(t->host); t->host = NULL; return 0;
}
static int32_t GUEST_ABI detach(Thread *p) {
    Thread *t = thread(p); if (!t || t->detached || t->joined) return error(22); t->detached = 1; return 0;
}
static void GUEST_ABI exit_thread(void *result) {
    self()->result = result; thread_ending(); runtime_tls_detach(); ExitThread(0);
}
static int GUEST_ABI equal(Thread *a, Thread *b) { return a == b; }
static int GUEST_ABI yield(void) { SwitchToThread(); return 0; }
static int32_t GUEST_ABI key_create(uint32_t *out, Destructor dtor) {
    if (!out) return error(22); DWORD slot = TlsAlloc(); if (slot == TLS_OUT_OF_INDEXES) return error(35);
    AcquireSRWLockExclusive(&registry);
    if (key_count == 1024) { ReleaseSRWLockExclusive(&registry); TlsFree(slot); return error(35); }
    uint32_t key = key_count++; keys[key].slot = slot; keys[key].dtor = dtor; keys[key].active = 1; *out = key;
    ReleaseSRWLockExclusive(&registry); return 0;
}
static int32_t GUEST_ABI key_delete(uint32_t key) {
    AcquireSRWLockExclusive(&registry);
    if (key >= key_count || !keys[key].active) { ReleaseSRWLockExclusive(&registry); return error(22); }
    keys[key].active = 0; TlsFree(keys[key].slot); ReleaseSRWLockExclusive(&registry); return 0;
}
static void *GUEST_ABI get_specific(uint32_t key) {
    AcquireSRWLockShared(&registry); void *value = key < key_count && keys[key].active ? TlsGetValue(keys[key].slot) : NULL;
    ReleaseSRWLockShared(&registry); return value;
}
static int32_t GUEST_ABI set_specific(uint32_t key, void *value) {
    AcquireSRWLockShared(&registry); int valid = key < key_count && keys[key].active;
    int result = valid && TlsSetValue(keys[key].slot, value);
    ReleaseSRWLockShared(&registry); return result ? 0 : error(22);
}
static int32_t GUEST_ABI sched_yield_host(void){SwitchToThread();return 0;}
// Affinity and priority are recorded for the guest to read back; host threads keep the Windows
// scheduler's placement, which suits a PC better than the PS4's seven fixed cores.
static int32_t GUEST_ABI get_affinity(Thread *p, uint64_t *mask) { Thread *t = thread(p); if (!t || !mask) return error(22); *mask = t->attr.affinity; return 0; }
static int32_t GUEST_ABI set_affinity(Thread *p, uint64_t mask) { Thread *t = thread(p); if (!t || !mask) return error(22); t->attr.affinity = mask; return 0; }
static int32_t GUEST_ABI get_priority(Thread *p, int *priority) { Thread *t = thread(p); if (!t || !priority) return error(22); *priority = t->attr.priority; return 0; }
static int32_t GUEST_ABI set_priority(Thread *p, int priority) { Thread *t = thread(p); if (!t || priority < 256 || priority > 959) return error(22); t->attr.priority = priority; return 0; }
static int32_t GUEST_ABI get_name(Thread *p, char *name) { Thread *t = thread(p); if (!t || !name) return error(22); snprintf(name, 32, "%s", t->name); return 0; }
static int32_t GUEST_ABI attr_get_detach(Attribute **p, int *n) { Attribute *a = attribute(p); if (!a || !n) return error(22); *n = a->detached; return 0; }
// pthread_once_t starts with its state: 0 never run, 1 done; 2 marks a run in progress here.
static SRWLOCK once_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE once_done = CONDITION_VARIABLE_INIT;
static int32_t GUEST_ABI once(volatile int32_t *state, void (GUEST_ABI *init)(void)) {
    if (!state || !init) return error(22);
    if (__atomic_load_n(state, __ATOMIC_ACQUIRE) == 1) return 0;
    AcquireSRWLockExclusive(&once_lock);
    while (*state == 2) SleepConditionVariableSRW(&once_done, &once_lock, INFINITE, 0);
    int run = *state != 1;
    if (run) *state = 2;
    ReleaseSRWLockExclusive(&once_lock);
    if (!run) return 0;
    init();
    AcquireSRWLockExclusive(&once_lock); __atomic_store_n(state, 1, __ATOMIC_RELEASE); ReleaseSRWLockExclusive(&once_lock);
    WakeAllConditionVariable(&once_done);
    return 0;
}
// Hang report: suspends each guest thread, describes where it is, then resumes it unless the
// process is about to exit.
void runtime_report_threads(HANDLE out, int resume) {
    AcquireSRWLockShared(&registry);
    for (Thread *t = threads; t; t = t->next) {
        if (t->id == GetCurrentThreadId()) continue;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, t->id);
        if (!h) continue;
        DWORD code = 0;
        if (GetExitCodeThread(h, &code) && code == STILL_ACTIVE && SuspendThread(h) != (DWORD)-1) {
            CONTEXT c; memset(&c, 0, sizeof(c)); c.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(h, &c)) runtime_report_thread(out, t->name, t->id, &c);
            if (resume) {
                ResumeThread(h);
                // Seven more samples 50 ms apart: a looping thread shows its loop, a blocked one
                // the same wait. Only the position and chain are repeated.
                for (int sample = 1; sample < 8; ++sample) {
                    Sleep(50);
                    if (SuspendThread(h) == (DWORD)-1) break;
                    memset(&c, 0, sizeof(c)); c.ContextFlags = CONTEXT_FULL;
                    if (GetThreadContext(h, &c)) runtime_report_sample(out, t->name, sample, &c);
                    ResumeThread(h);
                }
            }
        }
        CloseHandle(h);
    }
    ReleaseSRWLockShared(&registry);
    // Then every other thread of the process (renderer, driver, SDL, ...), named by its thread
    // description, with return addresses into iss-gpu.dll / iss-probe.exe found on its stack.
    typedef HRESULT (WINAPI *Describe)(HANDLE, PWSTR *);
    HMODULE kernel = GetModuleHandleW(L"kernelbase.dll");
    Describe describe = kernel ? (Describe)(void *)GetProcAddress(kernel, "GetThreadDescription") : NULL;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 entry; entry.dwSize = sizeof(entry);
    for (BOOL more = snapshot != INVALID_HANDLE_VALUE && Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != GetCurrentProcessId() || entry.th32ThreadID == GetCurrentThreadId()) continue;
        int guest = 0;
        AcquireSRWLockShared(&registry);
        for (Thread *t = threads; t && !guest; t = t->next) guest = t->id == entry.th32ThreadID;
        ReleaseSRWLockShared(&registry);
        if (guest) continue;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
        if (!h) continue;
        char name[96] = "host";
        PWSTR wide = NULL;
        if (describe && SUCCEEDED(describe(h, &wide)) && wide) {
            if (wide[0]) { snprintf(name, sizeof(name), "host:"); WideCharToMultiByte(CP_UTF8, 0, wide, -1, name + 5, (int)sizeof(name) - 5, NULL, NULL); }
            LocalFree(wide);
        }
        if (SuspendThread(h) != (DWORD)-1) {
            CONTEXT c; memset(&c, 0, sizeof(c)); c.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(h, &c)) runtime_report_host_thread(out, name, entry.th32ThreadID, &c);
            if (resume) ResumeThread(h);
        }
        CloseHandle(h);
    }
    if (snapshot != INVALID_HANDLE_VALUE) CloseHandle(snapshot);
}
uintptr_t thread_resolve(const char *identity) {
    if (!strstr(identity,"#libkernel#1#libkernel#257")) return 0;
    const char *api = runtime_api_name(identity);
#define API(n,f) if (!strcmp(api,n)) return (uintptr_t)f
    API("sched_yield",sched_yield_host);
    API("scePthreadAttrInit",attr_init); API("scePthreadAttrDestroy",attr_destroy); API("scePthreadAttrGet",attr_get);
    API("scePthreadAttrSetstacksize",attr_stack); API("scePthreadAttrSetaffinity",attr_affinity);
    API("scePthreadAttrGetaffinity",attr_get_affinity); API("scePthreadAttrSetinheritsched",attr_inherit);
    API("scePthreadAttrSetschedpolicy",attr_policy); API("scePthreadAttrSetschedparam",attr_priority);
    API("scePthreadAttrGetschedparam",attr_get_priority); API("scePthreadAttrSetdetachstate",attr_detach);
    API("scePthreadSelf",self); API("scePthreadCreate",create); API("scePthreadJoin",join);
    API("scePthreadDetach",detach); API("scePthreadExit",exit_thread); API("scePthreadEqual",equal);
    API("scePthreadYield",yield); API("scePthreadKeyCreate",key_create); API("scePthreadKeyDelete",key_delete);
    API("scePthreadGetspecific",get_specific); API("scePthreadSetspecific",set_specific);
    API("scePthreadGetaffinity",get_affinity); API("scePthreadSetaffinity",set_affinity);
    API("scePthreadGetprio",get_priority); API("scePthreadSetprio",set_priority);
    API("scePthreadGetname",get_name); API("scePthreadAttrGetdetachstate",attr_get_detach);
    API("scePthreadOnce",once); API("_sceKernelSetThreadDtors",set_thread_dtors);
#undef API
    return 0;
}
