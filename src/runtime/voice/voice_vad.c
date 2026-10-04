#include "voice_vad.h"

#include <math.h>

/* The floor follows the room while the gate is shut and only ever falls while
   it is open. A tracker frozen for the whole segment locks a loud room into
   permanent listening: the gate opens before the estimate has seen anything,
   the threshold stays on its clamp, and the release level then sits below the
   room, so nothing closes the gate again. */
#define VAD_NOISE_GAIN 2.0f
#define VAD_NOISE_RISE 0.02f
#define VAD_NOISE_FALL 0.2f
#define VAD_NOISE_WARM_FRAMES 30u /* 600 ms of room before the first onset. */
#define VAD_ONSET_PEAK 1.35f
#define VAD_RELEASE_RATIO 0.8f

static void update_threshold(BongoCatVad *vad) {
    float scaled = vad->noise_floor * VAD_NOISE_GAIN;
    vad->threshold = scaled > BONGO_CAT_VAD_RMS_FLOOR ? scaled
        : BONGO_CAT_VAD_RMS_FLOOR;
}

float bongo_cat_vad_onset_gate(const BongoCatVad *vad, float echo_floor) {
    /* With echo present the caller already added headroom, so the peak
       multiplier would make an interrupt impossible to trigger. */
    float gate = vad->threshold * (echo_floor > 0.0f ? 1.0f : VAD_ONSET_PEAK);
    return gate > echo_floor ? gate : echo_floor;
}

static void close_gate(BongoCatVad *vad) {
    vad->speaking = false;
    vad->confirmed = false;
    vad->silence_ms = 0;
    vad->speech_ms = 0;
}

static void track_noise(BongoCatVad *vad, float frame_rms) {
    if (frame_rms < vad->noise_floor)
        vad->noise_floor += (frame_rms - vad->noise_floor) * VAD_NOISE_FALL;
    else if (!vad->speaking)
        vad->noise_floor += (frame_rms - vad->noise_floor) * VAD_NOISE_RISE;
    if (vad->noise_floor < BONGO_CAT_VAD_RMS_FLOOR)
        vad->noise_floor = BONGO_CAT_VAD_RMS_FLOOR;
    update_threshold(vad);
    if (vad->warm_frames < VAD_NOISE_WARM_FRAMES) ++vad->warm_frames;
}

void bongo_cat_vad_init(BongoCatVad *vad, unsigned vadu_end_ms) {
    vad->noise_floor = BONGO_CAT_VAD_RMS_FLOOR;
    vad->warm_frames = 0;
    vad->onset_streak = 0;
    vad->silence_ms = 0;
    vad->speech_ms = 0;
    vad->vadu_end_ms = vadu_end_ms >= BONGO_CAT_VAD_FRAME_MS
        ? vadu_end_ms : BONGO_CAT_VAD_TAIL_SILENCE_MS;
    vad->speaking = false;
    vad->confirmed = false;
    update_threshold(vad);
}

void bongo_cat_vad_reset(BongoCatVad *vad) {
    /* Forget the utterance, keep the room: the noise estimate is what makes
       the next onset decision correct rather than starting from scratch. */
    vad->onset_streak = 0;
    vad->silence_ms = 0;
    vad->speech_ms = 0;
    vad->speaking = false;
    vad->confirmed = false;
    update_threshold(vad);
}

BongoCatVadEvent bongo_cat_vad_feed(BongoCatVad *vad, float frame_rms,
    float echo_floor) {
    if (echo_floor <= 0.0f) track_noise(vad, frame_rms);
    if (!vad->speaking) {
        if (frame_rms > bongo_cat_vad_onset_gate(vad, echo_floor))
            ++vad->onset_streak;
        else vad->onset_streak = 0;
        unsigned needed = echo_floor > 0.0f
            ? BONGO_CAT_VAD_BARGE_ONSET_FRAMES : BONGO_CAT_VAD_ONSET_FRAMES;
        if (vad->warm_frames >= VAD_NOISE_WARM_FRAMES &&
            vad->onset_streak >= needed) {
            vad->speaking = true;
            vad->confirmed = false;
            vad->silence_ms = 0;
            vad->speech_ms = 0;
            vad->onset_streak = 0;
        }
        return BONGO_CAT_VAD_EVENT_NONE;
    }
    if (frame_rms < vad->threshold * VAD_RELEASE_RATIO) {
        vad->silence_ms += BONGO_CAT_VAD_FRAME_MS;
        if (vad->silence_ms >= vad->vadu_end_ms) {
            /* An unconfirmed gate never reports anything: the transient it
               was opened for is not an utterance and was never recorded. */
            bool reported = vad->confirmed;
            close_gate(vad);
            return reported ? BONGO_CAT_VAD_EVENT_OFFSET
                : BONGO_CAT_VAD_EVENT_NONE;
        }
        return BONGO_CAT_VAD_EVENT_NONE;
    }
    vad->silence_ms = 0;
    vad->speech_ms += BONGO_CAT_VAD_FRAME_MS;
    if (!vad->confirmed) {
        if (vad->speech_ms >= BONGO_CAT_VAD_CONFIRM_MS) {
            vad->confirmed = true;
            return BONGO_CAT_VAD_EVENT_ONSET;
        }
    } else if (vad->speech_ms >= BONGO_CAT_VAD_MAX_SPEECH_MS) {
        /* A noisy room can keep the gate open forever; close the segment so
           recognition and the state machine recover. */
        close_gate(vad);
        return BONGO_CAT_VAD_EVENT_OFFSET;
    }
    return BONGO_CAT_VAD_EVENT_NONE;
}

float bongo_cat_vad_frame_rms(const int16_t *samples, unsigned count) {
    if (!samples || !count) return 0.0f;
    double sum = 0.0;
    for (unsigned i = 0; i < count; ++i) {
        double value = (double)samples[i] / 32768.0;
        sum += value * value;
    }
    return (float)sqrt(sum / (double)count);
}
