// SPDX-License-Identifier: GPL-2.0-or-later
// PS4 ownership semantics implemented with Windows state locks and wait queues.
#include "runtime.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
typedef struct Attr { int type, protocol, active; struct Attr *next; } Attr;
typedef struct Mutex {
    SRWLOCK lock; CONDITION_VARIABLE changed;
    DWORD owner; unsigned depth, waiters;
    int type, active; struct Mutex *next;
} Mutex;
static SRWLOCK registry = SRWLOCK_INIT;
static Attr *attrs;
static Mutex *mutexes;
typedef struct Cond { CONDITION_VARIABLE changed; volatile LONG waiters; int active; struct Cond *next; } Cond;
static Cond *conditions;
typedef struct Reader { DWORD id; unsigned count; struct Reader *next; } Reader;
typedef struct Rwlock { SRWLOCK lock; CONDITION_VARIABLE changed; DWORD writer; unsigned readers,waiters; int active; Reader *owners; struct Rwlock *next; } Rwlock;
static Rwlock *rwlocks;
static int32_t GUEST_ABI rw_init(Rwlock **p,void *attr,const char *name){
    (void)name;if(!p||attr)return (int32_t)UINT32_C(0x80020016);
    Rwlock *r=calloc(1,sizeof(*r));if(!r)return (int32_t)UINT32_C(0x8002000c);
    InitializeSRWLock(&r->lock);InitializeConditionVariable(&r->changed);r->active=1;
    AcquireSRWLockExclusive(&registry);r->next=rwlocks;rwlocks=r;*p=r;ReleaseSRWLockExclusive(&registry);return 0;
}
static Rwlock *rw_find(Rwlock **p){
    if(!p)return NULL;if(!*p&&rw_init(p,NULL,NULL))return NULL;
    AcquireSRWLockShared(&registry);Rwlock *r=rwlocks;while(r&&(r!=*p||!r->active))r=r->next;ReleaseSRWLockShared(&registry);return r;
}
static int32_t rw_acquire(Rwlock **p,int writing,int try_only){
    Rwlock *r=rw_find(p);if(!r){printf("RW invalid: slot=%p value=%p tid=%lu\n",(void *)p,p?(void *)*p:NULL,GetCurrentThreadId());return (int32_t)UINT32_C(0x80020016);}
    DWORD tid=GetCurrentThreadId();AcquireSRWLockExclusive(&r->lock);
    Reader *owner=r->owners;while(owner&&owner->id!=tid)owner=owner->next;
    if(r->writer==tid||(writing&&owner&&owner->count)){printf("RW deadlock: slot=%p tid=%lu writer=%lu reads=%u local_reads=%u\n",(void *)p,tid,r->writer,r->readers,owner?owner->count:0);ReleaseSRWLockExclusive(&r->lock);return (int32_t)UINT32_C(0x8002000b);}
    while(r->writer||(writing&&r->readers)){
        if(try_only){ReleaseSRWLockExclusive(&r->lock);return (int32_t)UINT32_C(0x80020010);}
        ++r->waiters;SleepConditionVariableSRW(&r->changed,&r->lock,INFINITE,0);--r->waiters;
    }
    if(writing)r->writer=tid;else{
        if(!owner){owner=calloc(1,sizeof(*owner));if(!owner){ReleaseSRWLockExclusive(&r->lock);return (int32_t)UINT32_C(0x8002000c);}
            owner->id=tid;owner->next=r->owners;r->owners=owner;}
        ++owner->count;++r->readers;
    }
    ReleaseSRWLockExclusive(&r->lock);return 0;
}
static int32_t GUEST_ABI rw_read(Rwlock **p){return rw_acquire(p,0,0);}
static int32_t GUEST_ABI rw_write(Rwlock **p){return rw_acquire(p,1,0);}
static int32_t GUEST_ABI rw_try_read(Rwlock **p){return rw_acquire(p,0,1);}
static int32_t GUEST_ABI rw_try_write(Rwlock **p){return rw_acquire(p,1,1);}
static int32_t GUEST_ABI rw_unlock(Rwlock **p){
    Rwlock *r=rw_find(p);if(!r)return (int32_t)UINT32_C(0x80020016);
    AcquireSRWLockExclusive(&r->lock);DWORD tid=GetCurrentThreadId();int result=0;
    if(r->writer==tid)r->writer=0;else{Reader *owner=r->owners;while(owner&&owner->id!=tid)owner=owner->next;
        if(!owner||!owner->count)result=1;else{--owner->count;--r->readers;}}
    WakeAllConditionVariable(&r->changed);ReleaseSRWLockExclusive(&r->lock);return result?(int32_t)UINT32_C(0x80020001):0;
}
static int32_t GUEST_ABI rw_destroy(Rwlock **p){
    Rwlock *r=rw_find(p);if(!r)return (int32_t)UINT32_C(0x80020016);
    AcquireSRWLockExclusive(&r->lock);int busy=r->writer||r->readers||r->waiters;
    if(!busy){r->active=0;*p=(Rwlock *)1;}ReleaseSRWLockExclusive(&r->lock);return busy?(int32_t)UINT32_C(0x80020010):0;
}
static Cond *condition(Cond **p, int create) {
    if (!p) return NULL;
    AcquireSRWLockExclusive(&registry);
    if (!*p && create) {
        Cond *c = calloc(1, sizeof(*c));
        if (c) { InitializeConditionVariable(&c->changed); c->active = 1; c->next = conditions; conditions = c; *p = c; }
    }
    Cond *c = conditions; while (c && (c != *p || !c->active)) c = c->next;
    ReleaseSRWLockExclusive(&registry); return c;
}
static int32_t orbis(int e) { return e ? (int32_t)(UINT32_C(0x80020000) | (unsigned)e) : 0; }
static Attr *attribute(Attr **p) {
    if (!p || !*p) return NULL;
    AcquireSRWLockShared(&registry);
    Attr *a = attrs;
    while (a && (a != *p || !a->active)) a = a->next;
    ReleaseSRWLockShared(&registry); return a;
}
static int GUEST_ABI attr_init(Attr **p) {
    if (!p) return 22;
    Attr *a = calloc(1, sizeof(*a)); if (!a) return 12;
    a->type = 1; a->active = 1;
    AcquireSRWLockExclusive(&registry); a->next = attrs; attrs = a; *p = a;
    ReleaseSRWLockExclusive(&registry); return 0;
}
static int GUEST_ABI attr_type(Attr **p, int type) {
    Attr *a = attribute(p); if (!a || type < 1 || type > 4) return 22;
    a->type = type; return 0;
}
static int GUEST_ABI attr_protocol(Attr **p, int protocol) {
    Attr *a = attribute(p); if (!a || protocol < 0 || protocol > 2) return 22;
    a->protocol = protocol; return 0;
}
static int GUEST_ABI attr_destroy(Attr **p) {
    Attr *a = attribute(p); if (!a) return 22;
    AcquireSRWLockExclusive(&registry); a->active = 0; *p = NULL;
    ReleaseSRWLockExclusive(&registry); return 0;
}
static Mutex *create_mutex(int type) {
    Mutex *m = calloc(1, sizeof(*m)); if (!m) return NULL;
    InitializeSRWLock(&m->lock); InitializeConditionVariable(&m->changed);
    m->type = type; m->active = 1; m->next = mutexes; mutexes = m; return m;
}
static int GUEST_ABI mutex_init(Mutex **p, Attr **a, const char *name) {
    (void)name; if (!p) return 22;
    Attr *attr = a && *a ? attribute(a) : NULL;
    if (a && *a && !attr) return 22;
    AcquireSRWLockExclusive(&registry);
    Mutex *m = create_mutex(attr ? attr->type : 1); if (m) *p = m;
    ReleaseSRWLockExclusive(&registry); return m ? 0 : 12;
}
static Mutex *mutex(Mutex **p) {
    if (!p) return NULL;
    AcquireSRWLockExclusive(&registry);
    if ((uintptr_t)*p < 2) { Mutex *m = create_mutex(*p ? 4 : 1); if (m) *p = m; }
    Mutex *m = mutexes; while (m && (m != *p || !m->active)) m = m->next;
    ReleaseSRWLockExclusive(&registry); return m;
}
// timeout_us: UINT64_MAX waits indefinitely.
static int acquire_timed(Mutex **p, int try_only, uint64_t timeout_us) {
    Mutex *m = mutex(p); if (!m) return 22;
    DWORD tid = GetCurrentThreadId(); AcquireSRWLockExclusive(&m->lock);
    if (!m->active) { ReleaseSRWLockExclusive(&m->lock); return 22; }
    if (m->owner == tid && m->type == 2) { ++m->depth; ReleaseSRWLockExclusive(&m->lock); return 0; }
    if (m->owner == tid && (m->type == 1 || m->type == 4) && !try_only) { ReleaseSRWLockExclusive(&m->lock); return 11; }
    if (m->owner && try_only) { ReleaseSRWLockExclusive(&m->lock); return 16; }
    uint64_t deadline = timeout_us == UINT64_MAX ? UINT64_MAX : runtime_process_time_us() + timeout_us;
    while (m->owner) {
        DWORD ms = INFINITE;
        if (deadline != UINT64_MAX) {
            uint64_t now = runtime_process_time_us();
            if (now >= deadline) { ReleaseSRWLockExclusive(&m->lock); return 60; }
            ms = (DWORD)((deadline - now + 999) / 1000);
        }
        ++m->waiters; SleepConditionVariableSRW(&m->changed, &m->lock, ms, 0); --m->waiters;
    }
    m->owner = tid; m->depth = 1; ReleaseSRWLockExclusive(&m->lock); return 0;
}
static int acquire(Mutex **p, int try_only) { return acquire_timed(p, try_only, UINT64_MAX); }
static int GUEST_ABI mutex_lock(Mutex **p) { return acquire(p, 0); }
static int GUEST_ABI mutex_try(Mutex **p) { return acquire(p, 1); }
static int GUEST_ABI mutex_unlock(Mutex **p) {
    Mutex *m = mutex(p); if (!m) return 22;
    AcquireSRWLockExclusive(&m->lock); int e = m->owner != GetCurrentThreadId() ? 1 : 0;
    if (!e && !--m->depth) { m->owner = 0; WakeConditionVariable(&m->changed); }
    ReleaseSRWLockExclusive(&m->lock); return e;
}
static int GUEST_ABI mutex_destroy(Mutex **p) {
    Mutex *m = mutex(p); if (!m) return 22;
    AcquireSRWLockExclusive(&m->lock); int e = m->owner || m->waiters ? 16 : 0;
    if (!e) { m->active = 0; *p = (Mutex *)2; }
    ReleaseSRWLockExclusive(&m->lock); return e;
}
static int GUEST_ABI cond_init(Cond **p, void *attr, const char *name) {
    (void)name; if (!p || attr) return 22; *p = NULL; return condition(p,1) ? 0 : 12;
}
static int cond_wait_impl(Cond **p, Mutex **mp, DWORD timeout) {
    Cond *c = condition(p,1); Mutex *m = mutex(mp); if (!c || !m) return 22;
    AcquireSRWLockExclusive(&m->lock);
    if (m->owner != GetCurrentThreadId()) { ReleaseSRWLockExclusive(&m->lock); return 1; }
    unsigned depth = m->depth;
    m->owner = 0; m->depth = 0; WakeConditionVariable(&m->changed);
    InterlockedIncrement(&c->waiters);
    BOOL signaled = SleepConditionVariableSRW(&c->changed,&m->lock,timeout,0);
    DWORD last = GetLastError(); InterlockedDecrement(&c->waiters);
    while (m->owner) SleepConditionVariableSRW(&m->changed,&m->lock,INFINITE,0);
    m->owner = GetCurrentThreadId(); m->depth = depth;
    ReleaseSRWLockExclusive(&m->lock);
    return signaled ? 0 : last == ERROR_TIMEOUT ? 60 : 22;
}
static int32_t GUEST_ABI orb_cond_init(Cond **p, void *a, const char *n) { return orbis(cond_init(p,a,n)); }
static int32_t GUEST_ABI orb_cond_wait(Cond **p, Mutex **m) { return orbis(cond_wait_impl(p,m,INFINITE)); }
static int32_t GUEST_ABI orb_cond_timedwait(Cond **p, Mutex **m, uint64_t us) {
    uint64_t ms = us/1000 + !!(us%1000);
    return orbis(cond_wait_impl(p,m,(DWORD)(ms >= INFINITE ? INFINITE-1 : ms)));
}
static int32_t GUEST_ABI orb_cond_signal(Cond **p) { Cond *c = condition(p,1); if (!c) return orbis(22); WakeConditionVariable(&c->changed); return 0; }
static int32_t GUEST_ABI orb_cond_broadcast(Cond **p) { Cond *c = condition(p,1); if (!c) return orbis(22); WakeAllConditionVariable(&c->changed); return 0; }
static int32_t GUEST_ABI orb_cond_destroy(Cond **p) {
    Cond *c = condition(p,0); if (!c) return orbis(22);
    if (InterlockedCompareExchange(&c->waiters,0,0)) return orbis(16);
    c->active = 0; *p = (Cond *)2; return 0;
}
#define WRAP1(n,t) static int32_t GUEST_ABI orb_##n(t p) { return orbis(n(p)); }
WRAP1(attr_init, Attr **) WRAP1(attr_destroy, Attr **) WRAP1(mutex_lock, Mutex **)
WRAP1(mutex_try, Mutex **) WRAP1(mutex_unlock, Mutex **) WRAP1(mutex_destroy, Mutex **)
static int32_t GUEST_ABI orb_attr_type(Attr **p, int n) { return orbis(attr_type(p,n)); }
static int32_t GUEST_ABI orb_attr_protocol(Attr **p, int n) { return orbis(attr_protocol(p,n)); }
static int32_t GUEST_ABI orb_mutex_init(Mutex **p, Attr **a, const char *n) { return orbis(mutex_init(p,a,n)); }
static int32_t GUEST_ABI orb_mutex_timedlock(Mutex **p, uint32_t us) { return orbis(acquire_timed(p, 0, us)); }
// POSIX spellings return the errno value itself.
static int posix(int32_t orbis_code) { return orbis_code ? orbis_code & 0xffff : 0; }
static int GUEST_ABI posix_cond_wait(Cond **p, Mutex **m) { return cond_wait_impl(p, m, INFINITE); }
static int GUEST_ABI posix_cond_broadcast(Cond **p) { return posix(orb_cond_broadcast(p)); }
static int GUEST_ABI posix_rw_read(Rwlock **p) { return posix(rw_read(p)); }
static int GUEST_ABI posix_rw_write(Rwlock **p) { return posix(rw_write(p)); }
static int GUEST_ABI posix_rw_unlock(Rwlock **p) { return posix(rw_unlock(p)); }
uintptr_t sync_resolve(const char *name) {
    const char *api = runtime_api_name(name);
    if (strstr(name,"#libkernel#1#libkernel#257")) {
        if (!strcmp(api,"scePthreadRwlockInit")) return (uintptr_t)rw_init;
        if (!strcmp(api,"scePthreadRwlockRdlock")) return (uintptr_t)rw_read;
        if (!strcmp(api,"scePthreadRwlockWrlock")) return (uintptr_t)rw_write;
        if (!strcmp(api,"scePthreadRwlockTryrdlock")) return (uintptr_t)rw_try_read;
        if (!strcmp(api,"scePthreadRwlockTrywrlock")) return (uintptr_t)rw_try_write;
        if (!strcmp(api,"scePthreadRwlockUnlock")) return (uintptr_t)rw_unlock;
        if (!strcmp(api,"scePthreadRwlockDestroy")) return (uintptr_t)rw_destroy;
        if (!strcmp(api,"scePthreadCondInit")) return (uintptr_t)orb_cond_init;
        if (!strcmp(api,"scePthreadCondWait")) return (uintptr_t)orb_cond_wait;
        if (!strcmp(api,"scePthreadCondTimedwait")) return (uintptr_t)orb_cond_timedwait;
        if (!strcmp(api,"scePthreadCondSignal")) return (uintptr_t)orb_cond_signal;
        if (!strcmp(api,"scePthreadCondBroadcast")) return (uintptr_t)orb_cond_broadcast;
        if (!strcmp(api,"scePthreadCondDestroy")) return (uintptr_t)orb_cond_destroy;
        if (!strcmp(api,"scePthreadMutexTimedlock")) return (uintptr_t)orb_mutex_timedlock;
    }
    if (strstr(name,"#libkernel#1#libkernel#257") || strstr(name,"#libScePosix#1#libkernel#257")) {
        if (!strcmp(api,"pthread_cond_wait")) return (uintptr_t)posix_cond_wait;
        if (!strcmp(api,"pthread_cond_broadcast")) return (uintptr_t)posix_cond_broadcast;
        if (!strcmp(api,"pthread_rwlock_rdlock")) return (uintptr_t)posix_rw_read;
        if (!strcmp(api,"pthread_rwlock_wrlock")) return (uintptr_t)posix_rw_write;
        if (!strcmp(api,"pthread_rwlock_unlock")) return (uintptr_t)posix_rw_unlock;
    }
#define RESOLVE(n,fn) if (!strcmp(name,n "#libkernel#1#libkernel#257")||!strcmp(name,n "#libScePosix#1#libkernel#257")) return (uintptr_t)fn
    RESOLVE("F8bUHwAG284",orb_attr_init); RESOLVE("n2MMpvU8igI",orb_attr_init);
    RESOLVE("iMp8QpE+XO4",orb_attr_type); RESOLVE("1FGvU0i9saQ",orb_attr_protocol);
    RESOLVE("smWEktiyyG0",orb_attr_destroy); RESOLVE("cmo1RIYva9o",orb_mutex_init);
    RESOLVE("9UK1vLZQft4",orb_mutex_lock); RESOLVE("upoVrzMHFeE",orb_mutex_try);
    RESOLVE("tn3VlD0hG60",orb_mutex_unlock); RESOLVE("2Of0f+3mhhE",orb_mutex_destroy);
    RESOLVE("dQHWEsJtoE4",attr_init); RESOLVE("mDmgMOGVUqg",attr_type);
    RESOLVE("5txKfcMUAok",attr_protocol); RESOLVE("HF7lK46xzjY",attr_destroy);
    RESOLVE("ttHNfU+qDBU",mutex_init); RESOLVE("7H0iTOciTLo",mutex_lock);
    RESOLVE("K-jXhbt2gn4",mutex_try); RESOLVE("2Z+PpY6CaJg",mutex_unlock);
    RESOLVE("ltCfaGr2JGE",mutex_destroy);
#undef RESOLVE
    return 0;
}
