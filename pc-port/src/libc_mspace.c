// SPDX-License-Identifier: GPL-2.0-or-later
// Boundary-tag allocator with power-of-two size bins, first fit within a bin. Block headers
// live inside the game's range; the Mspace record itself is host memory.
#include "libc_mspace.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
typedef SRWLOCK Lock;
#define LOCK_INIT(l) InitializeSRWLock(l)
#define LOCK(l) AcquireSRWLockExclusive(l)
#define UNLOCK(l) ReleaseSRWLockExclusive(l)
#else
#include <pthread.h>
typedef pthread_mutex_t Lock;
#define LOCK_INIT(l) pthread_mutex_init(l, NULL)
#define LOCK(l) pthread_mutex_lock(l)
#define UNLOCK(l) pthread_mutex_unlock(l)
#endif

// size: the whole block including this 16-byte header, a multiple of 16; bit 0 marks it in use.
// prev_size: size of the block before it, 0 for the first. next/prev link free blocks only.
// Free blocks are always coalesced, so no two free blocks are adjacent. A 16-byte in-use
// sentinel ends the range.
typedef struct Block { uint64_t size, prev_size; struct Block *next, *prev; } Block;
enum { HEADER = 16, MIN_BLOCK = 32, BINS = 64, USED = 1 };
struct Mspace {
    Lock lock;
    uint8_t *start, *end;
    void *owned;
    Block *bins[BINS];
    uint64_t capacity, inuse, peak;
    char name[32];
};

static uint64_t size_of(const Block *b) { return b->size & ~(uint64_t)15; }
static int in_use(const Block *b) { return (int)(b->size & USED); }
static Block *after(Block *b) { return (Block *)((uint8_t *)b + size_of(b)); }
static int bin_of(uint64_t size) { return 63 - __builtin_clzll(size); }
static void insert(Mspace *m, Block *b) {
    int i = bin_of(size_of(b));
    b->prev = NULL; b->next = m->bins[i];
    if (b->next) b->next->prev = b;
    m->bins[i] = b;
}
static void unlink_block(Mspace *m, Block *b) {
    if (b->prev) b->prev->next = b->next; else m->bins[bin_of(size_of(b))] = b->next;
    if (b->next) b->next->prev = b->prev;
}
static void corrupt(const Mspace *m, const void *p, const char *what) {
    fprintf(stderr, "RUNTIME_ERROR: mspace '%s': %s at %p\n", m->name, what, p);
    fflush(stderr);
    abort();
}

Mspace *GUEST_ABI mspace_create(const char *name, void *base, uint64_t capacity, uint32_t flags) {
    (void)flags; // Always locked, so SCE_LIBC_MSPACE_THREAD_UNSAFE is harmless to ignore.
    Mspace *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    if (!base) base = m->owned = malloc((size_t)capacity);
    uint8_t *start = (uint8_t *)(((uintptr_t)base + 15) & ~(uintptr_t)15);
    uint8_t *end = (uint8_t *)(((uintptr_t)base + capacity) & ~(uintptr_t)15);
    if (!base || end < start || (uint64_t)(end - start) < MIN_BLOCK + HEADER) { free(m->owned); free(m); return NULL; }
    LOCK_INIT(&m->lock);
    m->start = start; m->end = end; m->capacity = capacity;
    snprintf(m->name, sizeof(m->name), "%s", name ? name : "");
    Block *sentinel = (Block *)(end - HEADER), *first = (Block *)start;
    first->size = (uint64_t)((uint8_t *)sentinel - start); first->prev_size = 0;
    sentinel->size = HEADER | USED; sentinel->prev_size = first->size;
    insert(m, first);
    return m;
}

int32_t GUEST_ABI mspace_destroy(Mspace *m) {
    if (!m) return 1;
    free(m->owned); free(m);
    return 0;
}

// The aligned payload address b could hold, or NULL. A gap in front must fit a free block.
static uint8_t *fit(Block *b, uint64_t align, uint64_t size) {
    uint8_t *payload = (uint8_t *)b + HEADER, *limit = (uint8_t *)b + size_of(b);
    uint8_t *p = (uint8_t *)(((uintptr_t)payload + align - 1) & ~(uintptr_t)(align - 1));
    while (p != payload && (uint64_t)(p - payload) < MIN_BLOCK) p += align;
    return p < limit && (uint64_t)(limit - p) >= size ? p : NULL;
}

static void *allocate(Mspace *m, uint64_t align, uint64_t size) {
    if (align < 16) align = 16;
    if ((align & (align - 1)) || size > m->capacity || align > m->capacity) return NULL;
    size = size < 16 ? 16 : (size + 15) & ~(uint64_t)15;
    Block *b = NULL; uint8_t *p = NULL;
    for (int i = bin_of(size + HEADER); i < BINS && !p; ++i)
        for (b = m->bins[i]; b && !(p = fit(b, align, size)); b = b->next) {}
    if (!p) return NULL;
    unlink_block(m, b);
    if (p != (uint8_t *)b + HEADER) {
        Block *aligned = (Block *)(p - HEADER);
        uint64_t front = (uint64_t)((uint8_t *)aligned - (uint8_t *)b);
        aligned->size = size_of(b) - front; aligned->prev_size = front;
        b->size = front;
        after(aligned)->prev_size = aligned->size;
        insert(m, b);
        b = aligned;
    }
    uint64_t need = size + HEADER;
    if (size_of(b) - need >= MIN_BLOCK) {
        Block *rest = (Block *)((uint8_t *)b + need);
        rest->size = size_of(b) - need; rest->prev_size = need;
        b->size = need;
        after(rest)->prev_size = rest->size;
        insert(m, rest);
    }
    b->size |= USED;
    m->inuse += size_of(b);
    if (m->inuse > m->peak) m->peak = m->inuse;
    return p;
}

static void release(Mspace *m, void *p) {
    Block *b = (Block *)((uint8_t *)p - HEADER);
    if ((uint8_t *)b < m->start || (uint8_t *)b >= m->end - HEADER || ((uintptr_t)b & 15)) corrupt(m, p, "free of a foreign pointer");
    if (!in_use(b)) corrupt(m, p, "double free");
    m->inuse -= size_of(b);
    b->size &= ~(uint64_t)USED;
    Block *next = after(b);
    if (!in_use(next)) { unlink_block(m, next); b->size += size_of(next); }
    if (b->prev_size) {
        Block *previous = (Block *)((uint8_t *)b - b->prev_size);
        if (!in_use(previous)) { unlink_block(m, previous); previous->size += size_of(b); b = previous; }
    }
    after(b)->prev_size = size_of(b);
    insert(m, b);
}

void *GUEST_ABI mspace_memalign(Mspace *m, uint64_t boundary, uint64_t size) {
    if (!m) return NULL;
    LOCK(&m->lock);
    void *p = allocate(m, boundary, size);
    UNLOCK(&m->lock);
    return p;
}

void GUEST_ABI mspace_free(Mspace *m, void *p) {
    if (!m || !p) return;
    LOCK(&m->lock);
    release(m, p);
    UNLOCK(&m->lock);
}

uint64_t GUEST_ABI mspace_usable_size(void *p) {
    return p ? size_of((Block *)((uint8_t *)p - HEADER)) - HEADER : 0;
}

void *GUEST_ABI mspace_reallocalign(Mspace *m, void *p, uint64_t boundary, uint64_t size) {
    if (!m) return NULL;
    if (!p) return mspace_memalign(m, boundary, size);
    if (!size) { mspace_free(m, p); return NULL; }
    uint64_t align = boundary < 16 ? 16 : boundary;
    uint64_t usable = mspace_usable_size(p);
    if (!((uintptr_t)p & (align - 1)) && usable >= size) return p;
    LOCK(&m->lock);
    void *moved = allocate(m, boundary, size);
    if (moved) { memcpy(moved, p, (size_t)(usable < size ? usable : size)); release(m, p); }
    UNLOCK(&m->lock);
    return moved;
}

int32_t GUEST_ABI mspace_stats(Mspace *m, MspaceStats *out) {
    if (!m || !out) return 1;
    LOCK(&m->lock);
    out->size = sizeof(*out); out->version = 1; out->reserved = 0;
    out->max_system = out->current_system = m->capacity;
    out->max_inuse = m->peak; out->current_inuse = m->inuse;
    UNLOCK(&m->lock);
    return 0;
}

int mspace_check(Mspace *m) {
    LOCK(&m->lock);
    int result = 0;
    uint64_t used = 0, free_blocks = 0, previous = 0;
    int previous_free = 0;
    Block *b = (Block *)m->start, *sentinel = (Block *)(m->end - HEADER);
    while (b < sentinel) {
        if (b->prev_size != previous || size_of(b) < MIN_BLOCK) { result = 1; break; }
        if (in_use(b)) { used += size_of(b); previous_free = 0; }
        else { if (previous_free) { result = 2; break; } previous_free = 1; ++free_blocks; }
        previous = size_of(b);
        b = after(b);
    }
    if (!result && (b != sentinel || sentinel->prev_size != previous || sentinel->size != (HEADER | USED))) result = 3;
    if (!result && used != m->inuse) result = 4;
    uint64_t listed = 0;
    for (int i = 0; i < BINS && !result; ++i)
        for (Block *f = m->bins[i]; f; f = f->next, ++listed)
            if (in_use(f) || bin_of(size_of(f)) != i || (f->next && f->next->prev != f)) { result = 5; break; }
    if (!result && listed != free_blocks) result = 6;
    UNLOCK(&m->lock);
    return result;
}
