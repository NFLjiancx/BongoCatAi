#include "tts_envelope.h"

#include <math.h>

/* Speech RMS sits well below full scale; this divisor maps loud syllables
   to a fully open mouth. The power curve lifts quiet and mid syllables so
   the mouth motion reads clearly on screen instead of hugging zero. */
#define TTS_ENVELOPE_FULL_SCALE 3000.0f
#define TTS_ENVELOPE_GAMMA 0.65f

size_t bongo_cat_tts_envelope_count(size_t pcm_frames, int sample_rate) {
    if (!pcm_frames || sample_rate <= 0) return 0;
    size_t frame_samples = (size_t)sample_rate *
        BONGO_CAT_TTS_ENVELOPE_FRAME_MS / 1000u;
    if (!frame_samples) frame_samples = 1;
    return (pcm_frames + frame_samples - 1) / frame_samples;
}

size_t bongo_cat_tts_envelope_build(const int16_t *pcm, size_t pcm_frames,
    int sample_rate, float *out, size_t out_cap) {
    if (!out || !out_cap) return 0;
    size_t count = bongo_cat_tts_envelope_count(pcm_frames, sample_rate);
    if (count > out_cap) count = out_cap;
    if (!pcm || !count) {
        for (size_t i = 0; i < count; ++i) out[i] = 0.0f;
        return count;
    }
    size_t frame_samples = (size_t)sample_rate *
        BONGO_CAT_TTS_ENVELOPE_FRAME_MS / 1000u;
    if (!frame_samples) frame_samples = 1;
    for (size_t i = 0; i < count; ++i) {
        size_t first = i * frame_samples;
        size_t last = first + frame_samples;
        if (last > pcm_frames) last = pcm_frames;
        double sum = 0.0;
        for (size_t s = first; s < last; ++s) {
            double v = (double)pcm[s];
            sum += v * v;
        }
        double rms = last > first ? sqrt(sum / (double)(last - first)) : 0.0;
        float value = (float)(rms / TTS_ENVELOPE_FULL_SCALE);
        if (value > 1.0f) value = 1.0f;
        out[i] = value > 0.0f ? powf(value, TTS_ENVELOPE_GAMMA) : 0.0f;
    }
    return count;
}
