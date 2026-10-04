#include "voice_internal.h"
#include "asr_worker.h"
#include "tts_worker.h"
#include "bongo_cat/audio.h"
#include "bongo_cat/log.h"
#include "bongo_cat/model.h"

#include <stdio.h>
#include <stdlib.h>

/* Barge-in keeps the microphone live while the pet talks, so its own voice is
   heard too. The TTS envelope says how loud each 20 ms of playback should be,
   and the capture level says how loud it actually arrives: their ratio is the
   echo gain of this machine (speaker volume, mic gain, distance). Multiplying
   the envelope by that gain, with headroom, gives the level a candidate
   interruption has to clear before the recognizer is even asked about it. */
#define BARGE_CALIBRATION_NS (1200ull * 1000000ull)
#define BARGE_ENVELOPE_MIN 0.25f
#define BARGE_HEADROOM 1.6f
#define BARGE_GAIN_EMA 0.25f
#define BARGE_REF_DECAY 0.92f
#define BARGE_GAIN_MIN 0.02f
#define BARGE_GAIN_MAX 1.0f
/* What this machine's measured levels say about a loud room: the echo of her
   own speakers sits around 0.09 of the envelope while the peak the microphone
   actually reports is 0.4-0.6, so the loud frames arriving during an answer are
   the room, not her. No floor derived from the playback envelope can separate
   that noise from somebody talking over her, which is why the words the
   recognizer brings back decide, and the floor only keeps the cost down. */
#define BARGE_MIN_CHARS 4u
#define BARGE_ECHO_PERCENT 60u
/* One recognition is what a burst of noise buys; somebody talking over her
   keeps the recognizer answering as the sentence grows. */
#define BARGE_MIN_HITS 2u
/* The recognizer needs a task start plus one partial result to hear the first
   word; past that the onset was noise, not somebody cutting in. */
#define BARGE_ARM_WINDOW_NS (2500ull * 1000000ull)

void bongo_cat_voice_echo_begin(BongoCatVoice *voice, Uint64 now) {
    /* Forgetting the gain at every answer left the first second blind: her own
       voice cleared a floor of zero and she cut herself off a few hundred ms
       in. The ratio of microphone to speaker is a property of this machine, so
       it survives the turn and only the calibration restarts. */
    if (!voice->echo_primed) {
        voice->echo_primed = true;
        voice->echo_gain = 0.0f;
    }
    voice->barge_ref = 0.0f;
    voice->barge_calibrate_until_ns = now + BARGE_CALIBRATION_NS;
    voice->echo_mic_peak = 0.0f;
    voice->echo_env_peak = 0.0f;
    SDL_SetAtomicInt(&voice->barge_ref_milli, 0);
}

void bongo_cat_voice_echo_reset(BongoCatVoice *voice) {
    /* One line per answer: without these numbers the floor is tuned blind, and
       an interruption that should have been refused looks like plain bad luck. */
    if (voice->echo_mic_peak > 0.0f)
        SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
            "Voice echo: gain %.3f, mic peak %.3f, envelope peak %.3f",
            voice->echo_gain, voice->echo_mic_peak, voice->echo_env_peak);
    voice->barge_ref = 0.0f;
    voice->barge_calibrate_until_ns = 0;
    voice->echo_mic_peak = 0.0f;
    voice->echo_env_peak = 0.0f;
    SDL_SetAtomicInt(&voice->barge_ref_milli, 0);
}

float bongo_cat_voice_echo_floor(BongoCatVoice *voice) {
    return (float)SDL_GetAtomicInt(&voice->barge_ref_milli) / 1000.0f;
}

void bongo_cat_voice_echo_update(BongoCatVoice *voice, Uint64 now,
    float envelope) {
    float mic = (float)SDL_GetAtomicInt(&voice->level_milli) / 1000.0f;
    if (mic > voice->echo_mic_peak) voice->echo_mic_peak = mic;
    if (envelope > voice->echo_env_peak) voice->echo_env_peak = envelope;
    if (envelope > BARGE_ENVELOPE_MIN && envelope <= 1.0f &&
        now < voice->barge_calibrate_until_ns) {
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

void bongo_cat_voice_barge_arm(BongoCatVoice *voice) {
    SDL_LockMutex(voice->mutex);
    bool already = voice->interrupt_until_ns != 0;
    if (!already) {
        voice->interrupt_until_ns = SDL_GetTicksNS() + BARGE_ARM_WINDOW_NS;
        voice->interrupt_echo_refused = false;
        voice->interrupt_hits = 0;
    }
    SDL_UnlockMutex(voice->mutex);
    if (already) return;
    /* Start transcribing the candidate right away: by the time the first word
       is certain the audio that proved it must already be on its way. */
    if (voice->asr) bongo_cat_asr_notify(voice->asr,
        BONGO_CAT_ASR_NOTIFY_BEGIN);
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
        "Voice interrupt armed: waiting for words");
}

bool bongo_cat_voice_barge_armed(BongoCatVoice *voice) {
    SDL_LockMutex(voice->mutex);
    bool armed = voice->interrupt_until_ns != 0;
    SDL_UnlockMutex(voice->mutex);
    return armed;
}

/* Keep only what carries meaning: a replay differs from the text that drove
   the speaker in punctuation, and the ASR's own spelling of the language. */
static size_t keep_words(char *buf, size_t cap, const char *src) {
    size_t n = 0;
    while (*src && n + 4 < cap) {
        unsigned char c = (unsigned char)*src;
        size_t len = c < 0x80u ? 1 : (c < 0xE0u ? 2 : (c < 0xF0u ? 3 : 4));
        unsigned int cp = c;
        if (len == 3) cp = ((unsigned)(c & 0x0Fu) << 12)
            | ((unsigned)((unsigned char)src[1] & 0x3Fu) << 6)
            | (unsigned)((unsigned char)src[2] & 0x3Fu);
        bool word = len == 1 ? SDL_isalnum(c) != 0
            : (len == 3 && cp >= 0x4E00u && cp <= 0x9FFFu);
        if (word) {
            SDL_memcpy(buf + n, src, len);
            n += len;
        }
        src += len;
    }
    buf[n] = '\0';
    return n;
}

static size_t count_words(const char *text) {
    size_t n = 0;
    while (*text) {
        unsigned char c = (unsigned char)*text;
        text += c < 0x80u ? 1 : (c < 0xE0u ? 2 : (c < 0xF0u ? 3 : 4));
        ++n;
    }
    return n;
}

bool bongo_cat_voice_barge_is_echo(BongoCatVoice *voice) {
    char heard[BONGO_CAT_VOICE_TEXT_CAP], said[BONGO_CAT_VOICE_TEXT_CAP];
    SDL_LockMutex(voice->mutex);
    snprintf(heard, sizeof(heard), "%s", voice->partial_text[0]
        ? voice->partial_text : voice->final_text);
    snprintf(said, sizeof(said), "%s", voice->speaking_text);
    bool refused = voice->interrupt_echo_refused;
    SDL_UnlockMutex(voice->mutex);
    keep_words(heard, sizeof(heard), heard);
    keep_words(said, sizeof(said), said);
    size_t total = count_words(heard);
    /* Nothing a loud room can produce: Paraformer answers noise with a word or
       two, and a real interruption arrives as a phrase that keeps growing. */
    if (total < BARGE_MIN_CHARS) return true;
    if (!said[0]) return false;
    size_t same = 0;
    for (const char *p = heard; *p; ) {
        unsigned char c = (unsigned char)*p;
        size_t len = c < 0x80u ? 1 : (c < 0xE0u ? 2 : (c < 0xF0u ? 3 : 4));
        char word[5];
        SDL_memcpy(word, p, len);
        word[len] = '\0';
        if (SDL_strstr(said, word)) ++same;
        p += len;
    }
    bool echo = same * 100 >= total * BARGE_ECHO_PERCENT;
    if (echo && !refused) {
        SDL_LockMutex(voice->mutex);
        voice->interrupt_echo_refused = true;
        SDL_UnlockMutex(voice->mutex);
        SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
            "Voice interrupt refused: %zu of %zu words are already being said",
            same, total);
    }
    return echo;
}

bool bongo_cat_voice_barge_confirm(BongoCatVoice *voice) {
    SDL_LockMutex(voice->mutex);
    unsigned hits = ++voice->interrupt_hits;
    /* The candidate is still arriving, so it is still a candidate: keep the
       window open as long as the recognizer keeps answering. */
    voice->interrupt_until_ns = SDL_GetTicksNS() + BARGE_ARM_WINDOW_NS;
    SDL_UnlockMutex(voice->mutex);
    return hits >= BARGE_MIN_HITS && !bongo_cat_voice_barge_is_echo(voice);
}

void bongo_cat_voice_barge_disarm(BongoCatVoice *voice) {
    SDL_LockMutex(voice->mutex);
    voice->interrupt_until_ns = 0;
    SDL_UnlockMutex(voice->mutex);
    if (voice->asr) bongo_cat_asr_notify(voice->asr,
        BONGO_CAT_ASR_NOTIFY_ABORT);
    bongo_cat_voice_ring_reset(&voice->ring);
    bongo_cat_voice_gate_release(voice);
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
        "Voice interrupt given up: the recognizer heard no words");
}

void bongo_cat_voice_barge_in(BongoCatVoice *voice) {
    char path[BONGO_CAT_PATH_CAP];
    SDL_LockMutex(voice->mutex);
    SDL_snprintf(path, sizeof(path), "%s", voice->tts_path);
    /* The armed window already has a task running; only a barge-in that came
       from elsewhere has to open one. */
    bool task_open = voice->interrupt_until_ns != 0;
    voice->interrupt_until_ns = 0;
    voice->speaking_until_ns = 0;
    voice->synthesizing = false;
    voice->mouth_value = 0.0f;
    voice->partial_text[0] = '\0';
    voice->final_text[0] = '\0';
    voice->result_text[0] = '\0';
    voice->speaking_text[0] = '\0';
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
    bongo_cat_voice_echo_reset(voice);
    if (!task_open && voice->asr) bongo_cat_asr_notify(voice->asr,
        BONGO_CAT_ASR_NOTIFY_BEGIN);
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE, "Voice barge-in: playback stopped");
}
