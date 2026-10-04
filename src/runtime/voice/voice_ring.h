#ifndef BONGO_CAT_VOICE_RING_H
#define BONGO_CAT_VOICE_RING_H

#include <SDL3/SDL_atomic.h>
#include <stdbool.h>
#include <stddef.h>

/* Lock-free single-producer single-consumer byte ring. Writes that would
   overwrite unread data are truncated for the audio capture path.
   capacity must be a power of two. Producer: write. Consumer: read.
   8 s, not 4 s: the ring is the only thing that keeps an utterance alive
   while the recognizer socket is being reopened for it. */
#define BONGO_CAT_VOICE_RING_CAP 262144

typedef struct BongoCatVoiceRing {
    unsigned char storage[BONGO_CAT_VOICE_RING_CAP];
    SDL_AtomicInt head; /* Producer write index (bytes). */
    SDL_AtomicInt tail; /* Consumer read index (bytes). */
} BongoCatVoiceRing;

void bongo_cat_voice_ring_init(BongoCatVoiceRing *ring);
size_t bongo_cat_voice_ring_write(BongoCatVoiceRing *ring, const void *data,
    size_t length);
size_t bongo_cat_voice_ring_read(BongoCatVoiceRing *ring, void *data,
    size_t length);
size_t bongo_cat_voice_ring_available(const BongoCatVoiceRing *ring);
void bongo_cat_voice_ring_reset(BongoCatVoiceRing *ring);

#endif
