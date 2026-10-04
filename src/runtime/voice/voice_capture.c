#include "voice_internal.h"
#include "bongo_cat/log.h"

#include <string.h>

#define VOICE_SAMPLE_RATE 16000u
#define VOICE_LOOKBACK_SAMPLES (VOICE_SAMPLE_RATE * BONGO_CAT_VAD_LOOKBACK_MS / 1000u)
/* Below this there is nothing but the machine: fan, handling noise and mains
   hum. Speech fundamental for the voice she answers with starts above it. */
#define VOICE_HPF_CUTOFF_HZ 180.0f

static void voice_hpf_init(BongoCatVoice *voice) {
    float rate = (float)VOICE_SAMPLE_RATE;
    float w0 = 6.2831853f * VOICE_HPF_CUTOFF_HZ / rate;
    float cos_w0 = SDL_cosf(w0), sin_w0 = SDL_sinf(w0);
    /* Q = 0.7071: a Butterworth response, no bump at the cutoff. */
    float alpha = sin_w0 / 1.4142135f;
    float a0 = 1.0f + alpha;
    voice->hpf_b0 = (1.0f + cos_w0) / 2.0f / a0;
    voice->hpf_b1 = -(1.0f + cos_w0) / a0;
    voice->hpf_b2 = (1.0f + cos_w0) / 2.0f / a0;
    voice->hpf_a1 = -2.0f * cos_w0 / a0;
    voice->hpf_a2 = (1.0f - alpha) / a0;
    voice->hpf_x1 = voice->hpf_x2 = voice->hpf_y1 = voice->hpf_y2 = 0.0f;
}

/* In place, across the whole frame, with the state carried into the next one
   so a filter break cannot ring the audio at every 20 ms boundary. */
static void voice_hpf_apply(BongoCatVoice *voice, int16_t *samples,
    unsigned count) {
    float x1 = voice->hpf_x1, x2 = voice->hpf_x2;
    float y1 = voice->hpf_y1, y2 = voice->hpf_y2;
    for (unsigned i = 0; i < count; ++i) {
        float x = (float)samples[i] / 32768.0f;
        float y = voice->hpf_b0 * x + voice->hpf_b1 * x1 + voice->hpf_b2 * x2
            - voice->hpf_a1 * y1 - voice->hpf_a2 * y2;
        x2 = x1; x1 = x; y2 = y1; y1 = y;
        float out = y * 32768.0f;
        if (out > 32767.0f) out = 32767.0f;
        else if (out < -32768.0f) out = -32768.0f;
        samples[i] = (int16_t)out;
    }
    voice->hpf_x1 = x1; voice->hpf_x2 = x2;
    voice->hpf_y1 = y1; voice->hpf_y2 = y2;
}

/* Runs on the miniaudio capture thread. Never touches the mutex; ring and
   VAD fields are producer-only here, the consumer is the ASR worker. */
static void voice_process_frame(BongoCatVoice *voice, int16_t *frame) {
    voice_hpf_apply(voice, frame, BONGO_CAT_VAD_FRAME_SAMPLES);
    unsigned turn = (unsigned)SDL_GetAtomicInt(&voice->turn);
    if (turn != voice->turn_seen) {
        /* The main thread ended the turn, most often on the recognizer's own
           sentence boundary while this room was still loud. Nothing else may
           close the gate, so do it here and drop the pre-speech history too. */
        voice->turn_seen = turn;
        bongo_cat_vad_reset(&voice->vad);
        voice->lookback_pos = 0;
        voice->lookback_fill = 0;
    }
    float rms = bongo_cat_vad_frame_rms(frame, BONGO_CAT_VAD_FRAME_SAMPLES);
    float echo_floor = bongo_cat_voice_echo_floor(voice);
    BongoCatVadEvent event = bongo_cat_vad_feed(&voice->vad, rms, echo_floor);
    int milli = (int)(rms * 1000.0f);
    if (milli > 1000) milli = 1000;
    SDL_SetAtomicInt(&voice->level_milli, milli);
    /* Keep the ring buffer of pre-speech audio current every frame. */
    SDL_memcpy(voice->lookback + voice->lookback_pos, frame,
        BONGO_CAT_VAD_FRAME_SAMPLES * sizeof(int16_t));
    voice->lookback_pos =
        (voice->lookback_pos + BONGO_CAT_VAD_FRAME_SAMPLES)
        % VOICE_LOOKBACK_SAMPLES;
    if (voice->lookback_fill < VOICE_LOOKBACK_SAMPLES)
        voice->lookback_fill += BONGO_CAT_VAD_FRAME_SAMPLES;
    if (event == BONGO_CAT_VAD_EVENT_ONSET) {
        size_t oldest = (voice->lookback_pos + VOICE_LOOKBACK_SAMPLES
            - voice->lookback_fill) % VOICE_LOOKBACK_SAMPLES;
        /* Copy oldest-first: the confirmed onset is ~240 ms into the
           utterance, so this recovers its beginning plus a short lead-in. */
        size_t first = VOICE_LOOKBACK_SAMPLES - oldest;
        if (first > voice->lookback_fill) first = voice->lookback_fill;
        bongo_cat_voice_ring_write(&voice->ring,
            (const unsigned char *)voice->lookback + oldest
            * sizeof(int16_t), first * sizeof(int16_t));
        if (voice->lookback_fill > first)
            bongo_cat_voice_ring_write(&voice->ring,
                (const unsigned char *)voice->lookback,
                (voice->lookback_fill - first) * sizeof(int16_t));
        bongo_cat_voice_post(voice, BONGO_CAT_VOICE_CAPTURE_ONSET);
    }
    if (voice->vad.confirmed || event == BONGO_CAT_VAD_EVENT_OFFSET)
        bongo_cat_voice_ring_write(&voice->ring,
            (const unsigned char *)frame,
            BONGO_CAT_VAD_FRAME_SAMPLES * sizeof(int16_t));
    if (event == BONGO_CAT_VAD_EVENT_OFFSET)
        bongo_cat_voice_post(voice, BONGO_CAT_VOICE_CAPTURE_OFFSET);
    if (voice->vad.confirmed && ++voice->meter_ticks % 8u == 0u)
        bongo_cat_voice_post(voice, BONGO_CAT_VOICE_CAPTURE_LEVEL);
}

static void voice_capture_callback(ma_device *device, void *output,
    const void *input, ma_uint32 frame_count) {
    BongoCatVoice *voice = (BongoCatVoice *)device->pUserData;
    const int16_t *samples = (const int16_t *)input;
    (void)output;
    if (!voice || !input) return;
    unsigned carry = voice->frame_carry_count;
    while (frame_count) {
        unsigned room = BONGO_CAT_VAD_FRAME_SAMPLES - carry;
        unsigned take = frame_count < room ? frame_count : room;
        SDL_memcpy(voice->frame_carry + carry, samples, take
            * sizeof(int16_t));
        carry += take;
        samples += take;
        frame_count -= take;
        if (carry < BONGO_CAT_VAD_FRAME_SAMPLES) continue;
        voice_process_frame(voice, voice->frame_carry);
        carry = 0;
    }
    voice->frame_carry_count = carry;
}

bool bongo_cat_voice_capture_start(BongoCatVoice *voice) {
    if (voice->capturing) return true;
    ma_device_config config = ma_device_config_init(ma_device_type_capture);
    config.capture.format = ma_format_s16;
    config.capture.channels = 1;
    config.sampleRate = VOICE_SAMPLE_RATE;
    config.dataCallback = voice_capture_callback;
    config.pUserData = voice;
    bongo_cat_voice_ring_init(&voice->ring);
    bongo_cat_vad_init(&voice->vad, BONGO_CAT_VAD_TAIL_SILENCE_MS);
    voice_hpf_init(voice);
    voice->lookback_pos = voice->lookback_fill = 0;
    voice->frame_carry_count = 0;
    ma_result result = ma_device_init(NULL, &config, &voice->capture_device);
    if (result != MA_SUCCESS) {
        SDL_snprintf(voice->error, sizeof(voice->error),
            "Microphone open failed (%d)", (int)result);
        SDL_LogWarn(BONGO_CAT_LOG_LIFECYCLE,
            "Voice capture start failed: miniaudio %d", (int)result);
        return false;
    }
    if (ma_device_start(&voice->capture_device) != MA_SUCCESS) {
        ma_device_uninit(&voice->capture_device);
        SDL_strlcpy(voice->error, "Microphone start failed",
            sizeof(voice->error));
        return false;
    }
    voice->capturing = true;
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE, "Voice capture started");
    return true;
}

void bongo_cat_voice_capture_stop(BongoCatVoice *voice) {
    if (!voice->capturing) return;
    ma_device_stop(&voice->capture_device);
    ma_device_uninit(&voice->capture_device);
    voice->capturing = false;
    bongo_cat_voice_ring_reset(&voice->ring);
    bongo_cat_vad_reset(&voice->vad);
    voice->lookback_pos = voice->lookback_fill = 0;
    voice->frame_carry_count = 0;
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE, "Voice capture stopped");
}
