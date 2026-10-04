#include "voice_internal.h"
#include "asr_worker.h"
#include "tts_worker.h"
#include "bongo_cat/audio.h"
#include "bongo_cat/log.h"
#include "bongo_cat/model.h"

#include <stdlib.h>

/* Barge-in keeps the microphone live while the pet talks, so its own voice is
   heard too. The TTS envelope says how loud each 20 ms of playback should be,
   and the capture level says how loud it actually arrives: their ratio is the
   echo gain of this machine (speaker volume, mic gain, distance). Multiplying
   the envelope by that gain, with headroom, gives the level a real user has to
   clear before the pet stops talking. */
#define BARGE_CALIBRATION_NS (1200ull * 1000000ull)
#define BARGE_ENVELOPE_MIN 0.25f
#define BARGE_HEADROOM 1.6f
#define BARGE_GAIN_EMA 0.25f
#define BARGE_REF_DECAY 0.92f
#define BARGE_GAIN_MIN 0.02f
#define BARGE_GAIN_MAX 1.0f

void bongo_cat_voice_echo_begin(BongoCatVoice *voice, Uint64 now) {
    voice->echo_gain = 0.0f;
    voice->barge_ref = 0.0f;
    voice->barge_calibrate_until_ns = now + BARGE_CALIBRATION_NS;
    SDL_SetAtomicInt(&voice->barge_ref_milli, 0);
}

void bongo_cat_voice_echo_reset(BongoCatVoice *voice) {
    voice->echo_gain = 0.0f;
    voice->barge_ref = 0.0f;
    voice->barge_calibrate_until_ns = 0;
    SDL_SetAtomicInt(&voice->barge_ref_milli, 0);
}

float bongo_cat_voice_echo_floor(BongoCatVoice *voice) {
    return (float)SDL_GetAtomicInt(&voice->barge_ref_milli) / 1000.0f;
}

void bongo_cat_voice_echo_update(BongoCatVoice *voice, Uint64 now,
    float envelope) {
    if (envelope > BARGE_ENVELOPE_MIN && envelope <= 1.0f &&
        now < voice->barge_calibrate_until_ns) {
        float mic = (float)SDL_GetAtomicInt(&voice->level_milli) / 1000.0f;
        float gain = mic / envelope;
        if (gain > BARGE_GAIN_MAX) gain = BARGE_GAIN_MAX;
        if (gain < BARGE_GAIN_MIN) gain = BARGE_GAIN_MIN;
        voice->echo_gain = voice->echo_gain > 0.0f
            ? voice->echo_gain + (gain - voice->echo_gain) * BARGE_GAIN_EMA
            : gain;
    }
    float ref = voice->echo_gain * envelope * BARGE_HEADROOM;
    /* Peaks of a diphthong must not drop the floor between frames, or the
       tail of our own sentence re-triggers the onset gate. */
    if (ref < voice->barge_ref * BARGE_REF_DECAY)
        ref = voice->barge_ref * BARGE_REF_DECAY;
    voice->barge_ref = ref;
    int milli = (int)(ref * 1000.0f);
    if (milli > 1000) milli = 1000;
    if (milli < 0) milli = 0;
    SDL_SetAtomicInt(&voice->barge_ref_milli, milli);
}

void bongo_cat_voice_barge_in(BongoCatVoice *voice) {
    char path[BONGO_CAT_PATH_CAP];
    SDL_LockMutex(voice->mutex);
    SDL_snprintf(path, sizeof(path), "%s", voice->tts_path);
    voice->speaking_until_ns = 0;
    voice->synthesizing = false;
    voice->mouth_value = 0.0f;
    voice->partial_text[0] = '\0';
    voice->final_text[0] = '\0';
    voice->result_text[0] = '\0';
    free(voice->tts_envelope);
    voice->tts_envelope = NULL;
    voice->tts_envelope_count = 0;
    /* Keep vad.speaking: the utterance that interrupted playback is already
       open in the recognizer and has to survive until its own offset. */
    voice->state = BONGO_CAT_VOICE_STATE_LISTENING;
    voice->turn_open_ns = SDL_GetTicksNS();
    voice->listen_until_ns = voice->turn_open_ns + BONGO_CAT_LISTEN_IDLE_NS;
    SDL_UnlockMutex(voice->mutex);
    if (path[0]) bongo_cat_audio_stop_path(voice->app->audio, path);
    bongo_cat_live2d_set_parameter(voice->app->live2d, "ParamMouthOpenY",
        0.0f);
    /* The interrupted answer is over: stop paying for its request and free the
       synthesizer, which refuses a new task while busy. */
    bongo_cat_voice_new_turn(voice);
    if (voice->assistant) bongo_cat_assistant_cancel(voice->assistant);
    if (voice->tts) bongo_cat_tts_cancel(voice->tts);
    /* Report what this machine actually learned, before the reset wipes it: the
       gain is the only evidence of whether the echo ever got measured. */
    float learned_gain = voice->echo_gain;
    bongo_cat_voice_echo_reset(voice);
    if (voice->asr) bongo_cat_asr_notify(voice->asr,
        BONGO_CAT_ASR_NOTIFY_BEGIN);
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
        "Voice barge-in: playback stopped, echo gain %.3f", learned_gain);
}
