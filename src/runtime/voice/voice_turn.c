#include "voice_internal.h"
#include "asr_worker.h"
#include "bongo_cat/log.h"

#include <SDL3/SDL.h>

#include <stdio.h>

#define BONGO_CAT_ASR_RECOGNIZE_TIMEOUT_NS (3ull * 1000000000ull)

void bongo_cat_voice_set_state(BongoCatVoice *voice, BongoCatVoiceState state) {
    SDL_LockMutex(voice->mutex);
    voice->state = state;
    if (state != BONGO_CAT_VOICE_STATE_ERROR_HOLD) voice->error[0] = '\0';
    SDL_UnlockMutex(voice->mutex);
    bongo_cat_voice_post(voice, 0);
}

/* Ask the capture thread to close its VAD gate: only that thread may touch the
   VAD while the microphone is running, so the request travels as a number. */
void bongo_cat_voice_gate_release(BongoCatVoice *voice) {
    SDL_AddAtomicInt(&voice->turn, 1);
}

unsigned bongo_cat_voice_new_turn(BongoCatVoice *voice) {
    SDL_LockMutex(voice->mutex);
    unsigned epoch = ++voice->turn_epoch;
    SDL_UnlockMutex(voice->mutex);
    return epoch;
}

/* End the utterance the moment a boundary is seen, whichever detector found
   it: the energy gate after a short silence, or the recognizer's own sentence
   end, which is the one that survives a room that never goes quiet because it
   only closes on speech. */
void bongo_cat_voice_turn_close(BongoCatVoice *voice, const char *reason) {
    Uint64 now = SDL_GetTicksNS();
    Uint64 opened_ns;
    SDL_AddAtomicInt(&voice->utterances, 1);
    bongo_cat_voice_gate_release(voice);
    SDL_LockMutex(voice->mutex);
    opened_ns = voice->turn_open_ns;
    voice->turn_open_ns = 0;
    voice->state = BONGO_CAT_VOICE_STATE_RECOGNIZING;
    voice->listen_until_ns = 0;
    voice->recognize_until_ns = now + BONGO_CAT_ASR_RECOGNIZE_TIMEOUT_NS;
    SDL_UnlockMutex(voice->mutex);
    bongo_cat_voice_post(voice, 0);
    if (voice->asr)
        bongo_cat_asr_notify(voice->asr, BONGO_CAT_ASR_NOTIFY_END);
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE, "Voice turn closed by %s in %llu ms",
        reason, (unsigned long long)(opened_ns
            ? (now - opened_ns) / 1000000ull : 0));
}

/* Hand the finalized utterance to the in-process assistant. The recognized
   question becomes the first bubble body line; progress appends below it. */
void bongo_cat_voice_dispatch(BongoCatVoice *voice) {
    char prompt[BONGO_CAT_VOICE_TEXT_CAP];
    SDL_LockMutex(voice->mutex);
    snprintf(prompt, sizeof(prompt), "%s", voice->final_text);
    SDL_UnlockMutex(voice->mutex);
    if (!prompt[0]) {
        /* Room noise reaches the recognizer as an empty sentence; that is not
           a failure worth interrupting the user for. */
        bongo_cat_voice_set_state(voice, BONGO_CAT_VOICE_STATE_IDLE);
        return;
    }
    SDL_LockMutex(voice->mutex);
    snprintf(voice->result_text, sizeof(voice->result_text), "%s", prompt);
    voice->partial_text[0] = '\0';
    voice->final_text[0] = '\0';
    voice->state = BONGO_CAT_VOICE_STATE_THINKING;
    voice->thinking_until_ns = SDL_GetTicksNS()
        + BONGO_CAT_ASSISTANT_WATCHDOG_NS;
    SDL_UnlockMutex(voice->mutex);
    bongo_cat_voice_post(voice, 0);
    /* Anything the previous answer's workers still hold is now stale. */
    unsigned epoch = bongo_cat_voice_new_turn(voice);
    if (!voice->assistant ||
        !bongo_cat_assistant_start(voice->assistant, prompt, epoch)) {
        bongo_cat_voice_report_error(voice, "assistant task could not start");
        return;
    }
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE, "Voice task dispatched to assistant");
}
