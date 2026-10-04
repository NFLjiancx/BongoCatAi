#ifndef BONGO_CAT_VOICE_VAD_H
#define BONGO_CAT_VOICE_VAD_H

#include <stdbool.h>
#include <stdint.h>

/* Energy VAD over 20 ms frames of 16 kHz mono s16 audio. BONGO_CAT_VAD_ONSET_FRAMES
   consecutive frames above threshold * peak open the gate, but nothing is
   reported until the segment has lasted BONGO_CAT_VAD_CONFIRM_MS: a click, a
   cough or a fan gust therefore produces no event at all instead of a bogus
   utterance. The gate stays open until vadu_end_ms of frames fall below
   threshold * release (hysteresis). The threshold is
   max(BONGO_CAT_VAD_RMS_FLOOR, noise_floor * gain), where noise_floor tracks the
   bottom of the level distribution and keeps learning while the gate is open, so
   constant room noise lifts the threshold instead of opening the gate. A frame
   far above the estimate counts as speech, not as room, so speaking cannot raise
   the floor against the speaker. While the pet talks, echo_floor raises the gate
   to the level of its own voice, the onset streak needs
   BONGO_CAT_VAD_BARGE_ONSET_FRAMES frames, and the tracker freezes. */
#define BONGO_CAT_VAD_FRAME_MS 20
#define BONGO_CAT_VAD_FRAME_SAMPLES (16000 * BONGO_CAT_VAD_FRAME_MS / 1000)
#define BONGO_CAT_VAD_LOOKBACK_MS 300u
#define BONGO_CAT_VAD_ONSET_FRAMES 4
#define BONGO_CAT_VAD_BARGE_ONSET_FRAMES 6
#define BONGO_CAT_VAD_CONFIRM_MS 160u
#define BONGO_CAT_VAD_MAX_SPEECH_MS 8000u
#define BONGO_CAT_VAD_TAIL_SILENCE_MS 450u
#define BONGO_CAT_VAD_RMS_FLOOR 0.006f

typedef enum BongoCatVadEvent {
    BONGO_CAT_VAD_EVENT_NONE = 0,
    BONGO_CAT_VAD_EVENT_ONSET,  /* Speech started this frame. */
    BONGO_CAT_VAD_EVENT_OFFSET, /* Speech ended this frame. */
} BongoCatVadEvent;

typedef struct BongoCatVad {
    float noise_floor;
    float threshold;
    unsigned warm_frames; /* Silence the gate waits for before its first onset. */
    unsigned onset_streak;
    unsigned silence_ms;
    unsigned speech_ms; /* Forces an offset in permanently noisy input. */
    unsigned vadu_end_ms;
    bool speaking;   /* Gate open, the segment may still be a transient. */
    bool confirmed;  /* Long enough to be an utterance: ONSET was reported. */
} BongoCatVad;

void bongo_cat_vad_init(BongoCatVad *vad, unsigned vadu_end_ms);
void bongo_cat_vad_reset(BongoCatVad *vad);
BongoCatVadEvent bongo_cat_vad_feed(BongoCatVad *vad, float frame_rms,
    float echo_floor);
float bongo_cat_vad_onset_gate(const BongoCatVad *vad, float echo_floor);
float bongo_cat_vad_frame_rms(const int16_t *samples, unsigned count);

#endif
