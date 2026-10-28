// SPDX-License-Identifier: GPL-2.0-or-later
// libSceAudio3d mixed to stereo. The bed is folded down like shadPS4 does (centre and surrounds at
// -3 dB, LFE dropped); each object is mono, scaled by its GAIN attribute (silent until one is set,
// as in shadPS4) and panned with equal power by the azimuth of its POSITION. There is no distance
// model, HRTF or spread. A port's mixed frames queue up to its depth and play on an SDL stream.
#include "runtime.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ERR_PORT ((int32_t)UINT32_C(0x80EA0002))
#define ERR_OBJECT ((int32_t)UINT32_C(0x80EA0003))
#define ERR_PARAMETER ((int32_t)UINT32_C(0x80EA0004))
#define ERR_MEMORY ((int32_t)UINT32_C(0x80EA0005))
#define ERR_RESOURCES ((int32_t)UINT32_C(0x80EA0006))
#define ERR_NOT_READY ((int32_t)UINT32_C(0x80EA0007))
#define ERR_NOT_SUPPORTED ((int32_t)UINT32_C(0x80EA0008))
enum { PORTS = 4, MAX_DEPTH = 64, MAX_GRANULARITY = 8192 };
enum { NO_ADVANCE = 0, ADVANCE_NO_PUSH = 1, ADVANCE_AND_PUSH = 2 };
enum { ATTR_PCM = 1, ATTR_POSITION = 2, ATTR_GAIN = 3, ATTR_PASSTHROUGH = 6, ATTR_RESET = 9 };

typedef struct { uint64_t size; uint32_t granularity, rate, max_objects, depth, mode, pad, beds; } OpenParameters;
typedef struct { uint32_t format, pad; const void *buffer; uint32_t samples; } Pcm;
typedef struct { uint32_t id, pad; const void *value; uint64_t size; } Attribute;
_Static_assert(sizeof(Pcm) == 24 && sizeof(Attribute) == 24, "Audio3d ABI");

// A queue of fixed-size frames (granularity * channels floats).
typedef struct { float *data; uint32_t capacity, head, count, floats; } Ring;
static int ring_init(Ring *r, uint32_t capacity, uint32_t floats) {
    r->data = calloc((size_t)capacity * floats, sizeof(float));
    r->capacity = capacity; r->floats = floats; r->head = r->count = 0;
    return r->data != NULL;
}
static void ring_free(Ring *r) { free(r->data); memset(r, 0, sizeof(*r)); }
static float *ring_push(Ring *r) {
    if (r->count == r->capacity) return NULL;
    float *slot = r->data + (size_t)((r->head + r->count++) % r->capacity) * r->floats;
    memset(slot, 0, r->floats * sizeof(float));
    return slot;
}
static float *ring_front(Ring *r) { return r->count ? r->data + (size_t)r->head * r->floats : NULL; }
static void ring_pop(Ring *r) { if (r->count) { r->head = (r->head + 1) % r->capacity; --r->count; } }

typedef struct { int active; uint32_t id, passthrough; float gain, x, y, z; Ring pcm; } Object;
typedef struct {
    int active; uint32_t granularity, depth, mode, max_objects, next_id;
    Ring bed, mixed; Object *objects; SDL_AudioStream *stream;
} Port;
static SRWLOCK lock = SRWLOCK_INIT;
static Port ports[PORTS];
static int initialized;

static float sample(const void *buffer, uint32_t format, size_t index) {
    return format ? ((const float *)buffer)[index] : (float)((const int16_t *)buffer)[index] / 32768.0f;
}
// Folds an interleaved buffer of 1, 2, 6 or 8 channels into stereo; frames past `frames` stay 0.
static void fold(const void *buffer, uint32_t format, uint32_t channels, uint32_t frames, float *out) {
    for (uint32_t i = 0; i < frames; ++i) {
        size_t base = (size_t)i * channels;
        float left = sample(buffer, format, base), right = channels > 1 ? sample(buffer, format, base + 1) : left;
        if (channels >= 6) {
            float centre = 0.7071f * sample(buffer, format, base + 2);
            left += centre + 0.7071f * sample(buffer, format, base + 4);
            right += centre + 0.7071f * sample(buffer, format, base + 5);
        }
        if (channels >= 8) { left += 0.7071f * sample(buffer, format, base + 6); right += 0.7071f * sample(buffer, format, base + 7); }
        out[2 * i] = left; out[2 * i + 1] = right;
    }
}
static Port *port_of(uint32_t id) { return id < PORTS && ports[id].active ? &ports[id] : NULL; }
static Object *object_of(Port *p, uint32_t id) {
    for (uint32_t i = 0; i < p->max_objects; ++i) if (p->objects[i].active && p->objects[i].id == id) return &p->objects[i];
    return NULL;
}
static void submit(Port *p) {
    float *frame;
    while ((frame = ring_front(&p->mixed))) {
        for (uint32_t i = 0; i < p->mixed.floats; ++i) frame[i] = frame[i] > 1.0f ? 1.0f : frame[i] < -1.0f ? -1.0f : frame[i];
        SDL_PutAudioStreamData(p->stream, frame, (int)(p->mixed.floats * sizeof(float)));
        ring_pop(&p->mixed);
    }
}
static uint32_t queued_frames(Port *p) {
    int bytes = SDL_GetAudioStreamQueued(p->stream);
    return (bytes > 0 ? (uint32_t)bytes / (p->granularity * 2 * sizeof(float)) : 0) + p->mixed.count;
}
static void close_port(Port *p) {
    if (p->stream) SDL_DestroyAudioStream(p->stream);
    for (uint32_t i = 0; p->objects && i < p->max_objects; ++i) ring_free(&p->objects[i].pcm);
    free(p->objects); ring_free(&p->bed); ring_free(&p->mixed);
    memset(p, 0, sizeof(*p));
}

static int32_t GUEST_ABI initialize(int64_t reserved) {
    if (reserved) return ERR_PARAMETER;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) { fprintf(stderr, "Audio3d init: %s\n", SDL_GetError()); return ERR_NOT_READY; }
    AcquireSRWLockExclusive(&lock); initialized = 1; ReleaseSRWLockExclusive(&lock);
    return 0;
}
static int32_t GUEST_ABI terminate(void) {
    AcquireSRWLockExclusive(&lock);
    if (!initialized) { ReleaseSRWLockExclusive(&lock); return ERR_NOT_READY; }
    for (int i = 0; i < PORTS; ++i) if (ports[i].active) close_port(&ports[i]);
    initialized = 0; ReleaseSRWLockExclusive(&lock);
    return 0;
}
// Parameter sizes 0x10/0x18/0x20/0x28 carry progressively more fields (as shadPS4 reads them).
static int32_t GUEST_ABI port_open(int32_t user, const OpenParameters *params, uint32_t *id) {
    if (id) *id = UINT32_MAX;
    if (user != 0xff || !params || !id) return ERR_PARAMETER;
    uint32_t granularity = params->granularity, depth = 2, objects = 512, mode = NO_ADVANCE;
    switch (params->size & ~UINT64_C(7)) {
    case 0x10: break;
    case 0x18: objects = params->max_objects; depth = params->depth; mode = ADVANCE_NO_PUSH; break;
    case 0x20: case 0x28: objects = params->max_objects; depth = params->depth; mode = params->mode; break;
    default: return ERR_PARAMETER;
    }
    if (params->rate || granularity < 256 || granularity % 256 || granularity > MAX_GRANULARITY ||
        !objects || objects > 4096 || !depth || mode > ADVANCE_AND_PUSH) return ERR_PARAMETER;
    if (depth > MAX_DEPTH) depth = MAX_DEPTH;
    AcquireSRWLockExclusive(&lock);
    if (!initialized) { ReleaseSRWLockExclusive(&lock); return ERR_NOT_READY; }
    int slot = -1;
    for (int i = 0; i < PORTS && slot < 0; ++i) if (!ports[i].active) slot = i;
    if (slot < 0) { ReleaseSRWLockExclusive(&lock); return ERR_RESOURCES; }
    Port *p = &ports[slot];
    p->granularity = granularity; p->depth = depth; p->mode = mode; p->max_objects = objects; p->next_id = 1;
    SDL_AudioSpec spec = {SDL_AUDIO_F32, 2, 48000};
    p->objects = calloc(objects, sizeof(Object));
    p->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (!p->objects || !p->stream || !ring_init(&p->bed, depth, granularity * 2) || !ring_init(&p->mixed, depth, granularity * 2)) {
        if (!p->stream) fprintf(stderr, "Audio3d open: %s\n", SDL_GetError());
        close_port(p); ReleaseSRWLockExclusive(&lock); return ERR_MEMORY;
    }
    p->active = 1; SDL_ResumeAudioStreamDevice(p->stream);
    ReleaseSRWLockExclusive(&lock);
    *id = (uint32_t)slot;
    printf("Audio3d port %d: granularity %u, depth %u, mode %u, %u objects\n", slot, granularity, depth, mode, objects);
    return 0;
}
static int32_t GUEST_ABI port_close(uint32_t id) {
    AcquireSRWLockExclusive(&lock);
    Port *p = port_of(id);
    if (p) close_port(p);
    ReleaseSRWLockExclusive(&lock);
    return p ? 0 : ERR_PORT;
}
static int32_t GUEST_ABI port_set_attribute(uint32_t id, uint32_t attribute, const void *value, uint64_t size) {
    (void)attribute; (void)value; (void)size;
    AcquireSRWLockShared(&lock); int ok = port_of(id) != NULL; ReleaseSRWLockShared(&lock);
    return ok ? 0 : ERR_PORT;
}
static int32_t GUEST_ABI bed_write(uint32_t id, uint32_t channels, uint32_t format, const void *buffer, uint32_t samples) {
    if (format > 1 || (channels != 2 && channels != 6 && channels != 8) || !buffer || !samples ||
        ((uintptr_t)buffer & (format ? 3 : 1))) return ERR_PARAMETER;
    AcquireSRWLockExclusive(&lock);
    Port *p = port_of(id);
    float *slot = p ? ring_push(&p->bed) : NULL;
    if (slot) fold(buffer, format, channels, samples < p->granularity ? samples : p->granularity, slot);
    ReleaseSRWLockExclusive(&lock);
    return !p ? ERR_PORT : slot ? 0 : ERR_NOT_READY;
}
static int32_t GUEST_ABI object_reserve(uint32_t id, uint32_t *object) {
    if (!object) return ERR_PARAMETER;
    AcquireSRWLockExclusive(&lock);
    Port *p = port_of(id);
    if (!p) { ReleaseSRWLockExclusive(&lock); return ERR_PORT; }
    Object *o = NULL;
    for (uint32_t i = 0; i < p->max_objects && !o; ++i) if (!p->objects[i].active) o = &p->objects[i];
    if (!o) { ReleaseSRWLockExclusive(&lock); return ERR_RESOURCES; }
    if (!o->pcm.data && !ring_init(&o->pcm, p->depth, p->granularity)) { ReleaseSRWLockExclusive(&lock); return ERR_MEMORY; }
    o->pcm.head = o->pcm.count = 0;
    o->active = 1; o->id = p->next_id++; o->gain = 0; o->x = o->y = o->z = 0; o->passthrough = 0;
    *object = o->id;
    ReleaseSRWLockExclusive(&lock);
    return 0;
}
static int32_t GUEST_ABI object_unreserve(uint32_t id, uint32_t object) {
    AcquireSRWLockExclusive(&lock);
    Port *p = port_of(id);
    Object *o = p ? object_of(p, object) : NULL;
    if (o) o->active = 0;
    ReleaseSRWLockExclusive(&lock);
    return !p ? ERR_PORT : o ? 0 : ERR_OBJECT;
}
static int32_t GUEST_ABI object_set_attributes(uint32_t id, uint32_t object, uint64_t count, const Attribute *list) {
    if (!count || !list) return ERR_PARAMETER;
    AcquireSRWLockExclusive(&lock);
    Port *p = port_of(id);
    Object *o = p ? object_of(p, object) : NULL;
    int32_t result = !p ? ERR_PORT : !o ? ERR_OBJECT : 0;
    for (uint64_t i = 0; !result && i < count; ++i)
        if (list[i].id == ATTR_RESET) { o->pcm.head = o->pcm.count = 0; o->gain = o->x = o->y = o->z = 0; o->passthrough = 0; }
    for (uint64_t i = 0; !result && i < count; ++i) {
        const Attribute *a = &list[i];
        if (!a->value) continue;
        if (a->id == ATTR_PCM && a->size >= sizeof(Pcm)) {
            const Pcm *pcm = a->value;
            if (pcm->format > 1 || !pcm->buffer || !pcm->samples) { result = ERR_PARAMETER; break; }
            float *slot = ring_push(&o->pcm);
            if (!slot) { result = ERR_NOT_READY; break; }
            uint32_t n = pcm->samples < p->granularity ? pcm->samples : p->granularity;
            for (uint32_t s = 0; s < n; ++s) slot[s] = sample(pcm->buffer, pcm->format, s);
        } else if (a->id == ATTR_POSITION && a->size >= 12) {
            const float *v = a->value; o->x = v[0]; o->y = v[1]; o->z = v[2];
        } else if (a->id == ATTR_GAIN && a->size >= 4) {
            memcpy(&o->gain, a->value, 4);
        } else if (a->id == ATTR_PASSTHROUGH && a->size >= 4) {
            memcpy(&o->passthrough, a->value, 4);
        }
    }
    ReleaseSRWLockExclusive(&lock);
    return result;
}
// One frame: the oldest bed frame plus the oldest PCM frame of every object.
static int32_t GUEST_ABI port_advance(uint32_t id) {
    AcquireSRWLockExclusive(&lock);
    Port *p = port_of(id);
    if (!p) { ReleaseSRWLockExclusive(&lock); return ERR_PORT; }
    if (p->mode == NO_ADVANCE) { ReleaseSRWLockExclusive(&lock); return ERR_NOT_SUPPORTED; }
    float *out = ring_push(&p->mixed);
    if (!out) { ReleaseSRWLockExclusive(&lock); return ERR_NOT_READY; }
    const float *bed = ring_front(&p->bed);
    if (bed) { memcpy(out, bed, p->granularity * 2 * sizeof(float)); ring_pop(&p->bed); }
    for (uint32_t i = 0; i < p->max_objects; ++i) {
        Object *o = &p->objects[i];
        const float *pcm = o->active ? ring_front(&o->pcm) : NULL;
        if (!pcm) continue;
        float left, right;
        if (o->passthrough == 1) { left = 1; right = 0; }
        else if (o->passthrough == 2) { left = 0; right = 1; }
        else {
            float horizontal = sqrtf(o->x * o->x + o->z * o->z);
            float pan = horizontal > 1e-6f ? o->x / horizontal : 0; // -1 left .. 1 right
            float angle = (pan + 1) * 0.78539816f;
            left = cosf(angle); right = sinf(angle);
        }
        left *= o->gain; right *= o->gain;
        if (left != 0 || right != 0)
            for (uint32_t s = 0; s < p->granularity; ++s) { out[2 * s] += pcm[s] * left; out[2 * s + 1] += pcm[s] * right; }
        ring_pop(&o->pcm);
    }
    if (p->mode == ADVANCE_NO_PUSH) submit(p);
    ReleaseSRWLockExclusive(&lock);
    return 0;
}
// Sends the mixed frames to the device; a blocking push waits until no more than the port's
// depth is still queued, so the game is paced by real playback.
static int32_t GUEST_ABI port_push(uint32_t id, uint32_t blocking) {
    AcquireSRWLockExclusive(&lock);
    Port *p = port_of(id);
    if (!p) { ReleaseSRWLockExclusive(&lock); return ERR_PORT; }
    if (p->mode != ADVANCE_AND_PUSH) { ReleaseSRWLockExclusive(&lock); return ERR_NOT_SUPPORTED; }
    submit(p);
    ReleaseSRWLockExclusive(&lock);
    for (int waited = 0; blocking && waited < 250; ++waited) {
        AcquireSRWLockShared(&lock);
        p = port_of(id);
        int full = p && queued_frames(p) > p->depth;
        ReleaseSRWLockShared(&lock);
        if (!full) break;
        SDL_Delay(1);
    }
    return 0;
}
static int32_t GUEST_ABI queue_level(uint32_t id, uint32_t *level, uint32_t *available) {
    if (!level && !available) return ERR_PARAMETER;
    AcquireSRWLockShared(&lock);
    Port *p = port_of(id);
    if (!p) { ReleaseSRWLockShared(&lock); return ERR_PORT; }
    uint32_t queued = queued_frames(p);
    if (queued > p->depth) queued = p->depth;
    if (level) *level = queued;
    if (available) *available = p->depth - queued;
    ReleaseSRWLockShared(&lock);
    return 0;
}

uintptr_t audio3d_resolve(const char *identity) {
    if (!strstr(identity, "#libSceAudio3d#1#libSceAudio3d#257")) return 0;
    const char *api = runtime_api_name(identity);
#define API(n, f) if (!strcmp(api, n)) return (uintptr_t)f
    API("sceAudio3dInitialize", initialize); API("sceAudio3dTerminate", terminate);
    API("sceAudio3dPortOpen", port_open); API("sceAudio3dPortClose", port_close);
    API("sceAudio3dPortSetAttribute", port_set_attribute); API("sceAudio3dBedWrite", bed_write);
    API("sceAudio3dObjectReserve", object_reserve); API("sceAudio3dObjectUnreserve", object_unreserve);
    API("sceAudio3dObjectSetAttributes", object_set_attributes); API("sceAudio3dPortAdvance", port_advance);
    API("sceAudio3dPortPush", port_push); API("sceAudio3dPortGetQueueLevel", queue_level);
#undef API
    return 0;
}
