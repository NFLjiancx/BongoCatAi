#include "voice_ring.h"

#include <assert.h>
#include <string.h>

#define RING_MASK (BONGO_CAT_VOICE_RING_CAP - 1)

static_assert((BONGO_CAT_VOICE_RING_CAP & RING_MASK) == 0,
    "voice ring capacity must be a power of two");
void bongo_cat_voice_ring_init(BongoCatVoiceRing *ring) {
    memset(ring->storage, 0, sizeof(ring->storage));
    SDL_SetAtomicInt(&ring->head, 0);
    SDL_SetAtomicInt(&ring->tail, 0);
}

size_t bongo_cat_voice_ring_write(BongoCatVoiceRing *ring, const void *data,
    size_t length) {
    const unsigned char *source = data;
    int head = SDL_GetAtomicInt(&ring->head);
    int tail = SDL_GetAtomicInt(&ring->tail);
    size_t used = (size_t)((head - tail) & RING_MASK);
    size_t free_space = BONGO_CAT_VOICE_RING_CAP - 1 - used;
    if (length > free_space) length = free_space;
    if (!length) return 0;
    size_t first = length;
    int index = head & RING_MASK;
    if (first > BONGO_CAT_VOICE_RING_CAP - (size_t)index)
        first = BONGO_CAT_VOICE_RING_CAP - (size_t)index;
    memcpy(ring->storage + index, source, first);
    if (length > first)
        memcpy(ring->storage, source + first, length - first);
    SDL_SetAtomicInt(&ring->head, (head + (int)length) & RING_MASK);
    return length;
}

size_t bongo_cat_voice_ring_read(BongoCatVoiceRing *ring, void *data,
    size_t length) {
    unsigned char *destination = data;
    int tail = SDL_GetAtomicInt(&ring->tail);
    int head = SDL_GetAtomicInt(&ring->head);
    size_t available = (size_t)((head - tail) & RING_MASK);
    if (length > available) length = available;
    if (!length) return 0;
    size_t first = length;
    int index = tail & RING_MASK;
    if (first > BONGO_CAT_VOICE_RING_CAP - (size_t)index)
        first = BONGO_CAT_VOICE_RING_CAP - (size_t)index;
    memcpy(destination, ring->storage + index, first);
    if (length > first)
        memcpy(destination + first, ring->storage, length - first);
    SDL_SetAtomicInt(&ring->tail, (tail + (int)length) & RING_MASK);
    return length;
}

size_t bongo_cat_voice_ring_available(const BongoCatVoiceRing *ring) {
    int head = SDL_GetAtomicInt(&((BongoCatVoiceRing *)ring)->head);
    int tail = SDL_GetAtomicInt(&((BongoCatVoiceRing *)ring)->tail);
    return (size_t)((head - tail) & RING_MASK);
}

void bongo_cat_voice_ring_reset(BongoCatVoiceRing *ring) {
    SDL_SetAtomicInt(&ring->head, 0);
    SDL_SetAtomicInt(&ring->tail, 0);
}
