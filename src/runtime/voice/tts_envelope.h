#ifndef BONGO_CAT_TTS_ENVELOPE_H
#define BONGO_CAT_TTS_ENVELOPE_H

#include <stddef.h>
#include <stdint.h>

/* 20 ms RMS envelope of mono s16 PCM, normalized to 0..1. Used to drive the
   Live2D mouth parameter in step with TTS playback without decoding audio
   per frame. */
#define BONGO_CAT_TTS_ENVELOPE_FRAME_MS 20u

/* Number of envelope frames needed for pcm_frames of input. */
size_t bongo_cat_tts_envelope_count(size_t pcm_frames, int sample_rate);

/* Fills out[0..n) with per-frame loudness; returns the number written
   (capped at out_cap). Silent or invalid input yields zeros. */
size_t bongo_cat_tts_envelope_build(const int16_t *pcm, size_t pcm_frames,
    int sample_rate, float *out, size_t out_cap);

#endif
