#include "voice_internal.h"
#include "asr_worker.h"
#include "tts_worker.h"
#include "tts_envelope.h"
#include "bongo_cat/audio.h"
#include "bongo_cat/log.h"
#include "bongo_cat/model.h"
#include "bongo_cat/path.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BONGO_CAT_TTS_SYNTH_WATCHDOG_NS (45ull * 1000000000ull)
#define BONGO_CAT_SPEAK_GRACE_NS (15ull * 1000000000ull)

static void hold_error(BongoCatVoice *voice, const char *message) {
    SDL_LockMutex(voice->mutex);
    voice->state = BONGO_CAT_VOICE_STATE_ERROR_HOLD;
    snprintf(voice->error, sizeof(voice->error), "%s", message);
    voice->error_hold_until_ns = SDL_GetTicksNS()
        + BONGO_CAT_VOICE_ERROR_HOLD_NS;
    SDL_UnlockMutex(voice->mutex);
    SDL_LogWarn(BONGO_CAT_LOG_LIFECYCLE, "Voice error: %s", message);
    bongo_cat_voice_post(voice, 0);
}

void bongo_cat_voice_report_error(BongoCatVoice *voice, const char *message) {
    if (!voice || !message) return;
    hold_error(voice, message);
}

void bongo_cat_voice_post(BongoCatVoice *voice, int code) {
    bongo_cat_voice_post_turn(voice, code, 0);
}

/* SDL user events only have one free slot and it already holds the event code,
   so a tagged post packs the code and the turn epoch into that pointer. */
void bongo_cat_voice_post_turn(BongoCatVoice *voice, int code,
    unsigned epoch) {
    SDL_Event event = {0};
    event.type = voice->event_type;
    event.user.data1 = voice;
    event.user.data2 = (void *)(uintptr_t)(((uintptr_t)epoch << 16) |
        ((uintptr_t)code & BONGO_CAT_VOICE_CODE_MASK));
    SDL_PushEvent(&event);
}

/* Key resolution order: BONGO_CAT_DASHSCOPE_API_KEY, then
 * <config_root>/dashscope.key. The file must never be logged or copied. */
bool bongo_cat_voice_resolve_api_key(BongoCatVoice *voice, char *out,
    size_t cap) {
    const char *env = SDL_getenv("BONGO_CAT_DASHSCOPE_API_KEY");
    if (env && env[0]) {
        SDL_strlcpy(out, env, cap);
        return out[0] != '\0';
    }
    char path[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_path_join(path, sizeof(path), voice->app->config_root,
        "dashscope.key")) return false;
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    size_t read = fread(out, 1, cap - 1, file);
    fclose(file);
    out[read] = '\0';
    while (read > 0 && (out[read - 1] == '\n' || out[read - 1] == '\r' ||
        out[read - 1] == ' ' || out[read - 1] == '\t')) out[--read] = '\0';
    return out[0] != '\0';
}

BongoCatVoice *bongo_cat_voice_create(BongoCatApp *app) {
    if (!app) return NULL;
    BongoCatVoice *voice = calloc(1, sizeof(*voice));
    if (!voice) return NULL;
    voice->app = app;
    voice->mutex = SDL_CreateMutex();
    voice->event_type = SDL_RegisterEvents(1);
    voice->state = BONGO_CAT_VOICE_STATE_IDLE;
    SDL_SetAtomicInt(&voice->utterances, 0);
    bongo_cat_voice_ring_init(&voice->ring);
    bongo_cat_vad_init(&voice->vad, BONGO_CAT_VAD_TAIL_SILENCE_MS);
    if (!voice->mutex || voice->event_type == (Uint32)-1) {
        if (voice->mutex) SDL_DestroyMutex(voice->mutex);
        free(voice);
        return NULL;
    }
    bongo_cat_voice_spawn_workers(voice);
    if (!voice->asr) voice->state = BONGO_CAT_VOICE_STATE_OFF;
    return voice;
}

bool bongo_cat_voice_event(BongoCatVoice *voice, const SDL_Event *event) {
    if (!voice || !event || event->type != voice->event_type ||
        event->user.data1 != voice) return false;
    uintptr_t packed = (uintptr_t)event->user.data2;
    int code = (int)(packed & BONGO_CAT_VOICE_CODE_MASK);
    unsigned epoch = (unsigned)(packed >> 16);
    SDL_LockMutex(voice->mutex);
    BongoCatVoiceState state = voice->state;
    unsigned turn = voice->turn_epoch;
    Uint64 hold_until = voice->error_hold_until_ns;
    SDL_UnlockMutex(voice->mutex);
    /* An answer the user already talked over belongs to a dead turn: dropping
       it here keeps it from being spoken in place of the new one. */
    if (epoch && epoch != turn) return true;
    if (state == BONGO_CAT_VOICE_STATE_ERROR_HOLD) {
        if (hold_until && SDL_GetTicksNS() >= hold_until)
            bongo_cat_voice_set_state(voice, BONGO_CAT_VOICE_STATE_IDLE);
        return true;
    }
    if (code == BONGO_CAT_VOICE_CAPTURE_ONSET &&
        state == BONGO_CAT_VOICE_STATE_IDLE) {
        SDL_LockMutex(voice->mutex);
        voice->state = BONGO_CAT_VOICE_STATE_LISTENING;
        voice->turn_open_ns = SDL_GetTicksNS();
        voice->listen_until_ns = voice->turn_open_ns
            + BONGO_CAT_LISTEN_IDLE_NS;
        SDL_UnlockMutex(voice->mutex);
        if (voice->asr) bongo_cat_asr_notify(voice->asr,
            BONGO_CAT_ASR_NOTIFY_BEGIN);
    } else if (code == BONGO_CAT_VOICE_CAPTURE_ONSET &&
        state == BONGO_CAT_VOICE_STATE_SPEAKING) {
        /* Energy alone is not an interruption. Doors, keyboards and this
           machine's own speakers all clear the gate; only the recognizer
           saying it heard a word may cut the answer short. */
        bongo_cat_voice_barge_arm(voice);
    } else if ((code == BONGO_CAT_VOICE_EV_ASR_PARTIAL ||
        code == BONGO_CAT_VOICE_EV_ASR_FINAL) &&
        state == BONGO_CAT_VOICE_STATE_SPEAKING &&
        bongo_cat_voice_barge_armed(voice) &&
        bongo_cat_voice_barge_confirm(voice)) {
        bongo_cat_voice_barge_in(voice);
        if (code == BONGO_CAT_VOICE_EV_ASR_FINAL) {
            /* The word heard was already a whole sentence: answer it now
               rather than wait for an offset that may never come. */
            bongo_cat_voice_turn_close(voice, "interrupt");
            if (voice->assistant) bongo_cat_voice_dispatch(voice);
            else bongo_cat_voice_set_state(voice, BONGO_CAT_VOICE_STATE_IDLE);
        }
    } else if (code == BONGO_CAT_VOICE_EV_ASR_PARTIAL &&
        state == BONGO_CAT_VOICE_STATE_LISTENING) {
        /* Words the recognizer actually heard are what keeps the listening
           window open. Raw energy no longer postpones it, so a loud room that
           produces no transcript expires instead of holding the gate for the
           whole maximum segment length. */
        SDL_LockMutex(voice->mutex);
        voice->listen_until_ns = SDL_GetTicksNS() + BONGO_CAT_LISTEN_IDLE_NS;
        SDL_UnlockMutex(voice->mutex);
    } else if (code == BONGO_CAT_VOICE_CAPTURE_OFFSET &&
        state == BONGO_CAT_VOICE_STATE_LISTENING) {
        bongo_cat_voice_turn_close(voice, "silence");
    } else if (code == BONGO_CAT_VOICE_CAPTURE_OFFSET &&
        state == BONGO_CAT_VOICE_STATE_THINKING) {
        /* A comment made while the answer is on its way has nowhere to go,
           but it must not leak into the next utterance either. */
        bongo_cat_voice_ring_reset(&voice->ring);
        bongo_cat_voice_gate_release(voice);
    } else if (code == BONGO_CAT_VOICE_EV_ASR_FINAL &&
        state == BONGO_CAT_VOICE_STATE_LISTENING) {
        /* The recognizer committed a sentence while the room stayed loud:
           that transcript is the turn. Answer it now - the flush after
           finish-task is what turned trailing noise into a bogus sentence
           earlier, and waiting for it costs seconds. */
        bongo_cat_voice_turn_close(voice, "sentence");
        if (voice->assistant) bongo_cat_voice_dispatch(voice);
        else bongo_cat_voice_set_state(voice, BONGO_CAT_VOICE_STATE_IDLE);
    } else if (code == BONGO_CAT_VOICE_EV_ASR_FINAL &&
        state == BONGO_CAT_VOICE_STATE_RECOGNIZING) {
        bongo_cat_voice_ring_reset(&voice->ring);
        bongo_cat_voice_gate_release(voice);
        if (voice->assistant) bongo_cat_voice_dispatch(voice);
        else bongo_cat_voice_set_state(voice, BONGO_CAT_VOICE_STATE_IDLE);
    } else if (code == BONGO_CAT_VOICE_EV_ASSISTANT_PROGRESS &&
        state == BONGO_CAT_VOICE_STATE_THINKING) {
        /* Tool activity keeps the turn alive: restart the watchdog. */
        SDL_LockMutex(voice->mutex);
        if (!voice->synthesizing)
            voice->thinking_until_ns = SDL_GetTicksNS()
                + BONGO_CAT_ASSISTANT_WATCHDOG_NS;
        SDL_UnlockMutex(voice->mutex);
    } else if (code == BONGO_CAT_VOICE_EV_ASSISTANT_RESULT &&
        state == BONGO_CAT_VOICE_STATE_THINKING) {
        char text[BONGO_CAT_VOICE_TEXT_CAP];
        SDL_LockMutex(voice->mutex);
        snprintf(text, sizeof(text), "%s", voice->result_text);
        voice->thinking_until_ns = 0;
        SDL_UnlockMutex(voice->mutex);
        if (voice->tts && text[0] &&
            bongo_cat_tts_speak(voice->tts, text, epoch)) {
            SDL_LockMutex(voice->mutex);
            voice->synthesizing = true;
            voice->thinking_until_ns = SDL_GetTicksNS()
                + BONGO_CAT_TTS_SYNTH_WATCHDOG_NS;
            SDL_UnlockMutex(voice->mutex);
            /* Stay THINKING (bubble keeps the result text) until the
               synthesis finishes and playback starts. */
        } else {
            bongo_cat_voice_set_state(voice, BONGO_CAT_VOICE_STATE_IDLE);
        }
    } else if (code == BONGO_CAT_VOICE_EV_TTS_READY &&
        state == BONGO_CAT_VOICE_STATE_THINKING) {
        char path[BONGO_CAT_PATH_CAP];
        Uint64 duration_ns = 0;
        SDL_LockMutex(voice->mutex);
        voice->synthesizing = false;
        voice->thinking_until_ns = 0;
        snprintf(path, sizeof(path), "%s", voice->tts_path);
        duration_ns = (Uint64)voice->tts_envelope_count * 20000000ull;
        SDL_UnlockMutex(voice->mutex);
        /* Full-duplex: the microphone stays open so the user can interrupt,
           and the envelope drives the echo floor of the onset gate. */
        bongo_cat_voice_echo_begin(voice, SDL_GetTicksNS());
        if (!path[0] || bongo_cat_audio_play(voice->app->audio, path, NULL)
            != BONGO_CAT_OK) {
            SDL_LogWarn(BONGO_CAT_LOG_LIFECYCLE, "TTS playback could not start");
            bongo_cat_voice_set_state(voice, BONGO_CAT_VOICE_STATE_IDLE);
        } else {
            SDL_LockMutex(voice->mutex);
            voice->speaking_until_ns = SDL_GetTicksNS() + duration_ns
                + BONGO_CAT_SPEAK_GRACE_NS;
            SDL_UnlockMutex(voice->mutex);
            bongo_cat_voice_set_state(voice, BONGO_CAT_VOICE_STATE_SPEAKING);
        }
    } else if (code == BONGO_CAT_VOICE_EV_TTS_ERROR &&
        state == BONGO_CAT_VOICE_STATE_THINKING) {
        SDL_LockMutex(voice->mutex);
        bool was_synthesizing = voice->synthesizing;
        voice->synthesizing = false;
        voice->thinking_until_ns = 0;
        SDL_UnlockMutex(voice->mutex);
        if (was_synthesizing)
            hold_error(voice, "voice could not synthesize the answer");
    } else if (code == BONGO_CAT_VOICE_EV_ASSISTANT_ERROR &&
        state == BONGO_CAT_VOICE_STATE_THINKING) {
        char message[BONGO_CAT_VOICE_TEXT_CAP];
        SDL_LockMutex(voice->mutex);
        snprintf(message, sizeof(message), "%s", voice->result_text);
        voice->thinking_until_ns = 0;
        voice->synthesizing = false;
        SDL_UnlockMutex(voice->mutex);
        hold_error(voice, message[0] ? message : "assistant task failed");
    }
    /* EV_ASR_PARTIAL and EV_ASR_ERROR carry data already stored by the
       worker; the main loop repaints the bubble from the status snapshot. */
    return true;
}

/* Drives the Live2D mouth from the TTS envelope while speaking and detects
   the end of playback. Caller runs on the main thread only. */
static void voice_update_speaking(BongoCatVoice *voice, Uint64 now) {
    char path[BONGO_CAT_PATH_CAP];
    SDL_LockMutex(voice->mutex);
    snprintf(path, sizeof(path), "%s", voice->tts_path);
    bool done = (voice->speaking_until_ns && now >= voice->speaking_until_ns);
    float target = 0.0f;
    if (!done && path[0] && voice->tts_envelope &&
        voice->tts_envelope_count && voice->tts_sample_rate > 0 &&
        bongo_cat_audio_is_playing(voice->app->audio, path)) {
        uint64_t cursor_us = 0;
        if (bongo_cat_audio_play_position(voice->app->audio, path,
            &cursor_us)) {
            uint64_t index = cursor_us /
                (BONGO_CAT_TTS_ENVELOPE_FRAME_MS * 1000ull);
            if (index < voice->tts_envelope_count)
                target = voice->tts_envelope[index];
        }
    } else if (!done && path[0] && !bongo_cat_audio_is_playing(
        voice->app->audio, path)) {
        done = true;
    }
    /* Fast attack, slower release keeps the mouth natural. */
    voice->mouth_value = target > voice->mouth_value ? target
        : voice->mouth_value * 0.80f;
    bool armed = voice->interrupt_until_ns != 0;
    bool interrupt_expired = armed && now >= voice->interrupt_until_ns;
    float value = done ? 0.0f : voice->mouth_value;
    if (done) {
        voice->speaking_until_ns = 0;
        free(voice->tts_envelope);
        voice->tts_envelope = NULL;
        voice->tts_envelope_count = 0;
        voice->mouth_value = 0.0f;
    }
    SDL_UnlockMutex(voice->mutex);
    bongo_cat_live2d_set_parameter(voice->app->live2d, "ParamMouthOpenY",
        value);
    if (done) {
        bongo_cat_voice_echo_reset(voice);
        if (armed) {
            /* She was being cut off as the answer ran out: those words are
               already with the recognizer, so go on listening for them rather
               than closing the turn on top of them. */
            bongo_cat_voice_barge_in(voice);
            return;
        }
        bongo_cat_voice_ring_reset(&voice->ring);
        bongo_cat_voice_gate_release(voice);
        bongo_cat_voice_set_state(voice, BONGO_CAT_VOICE_STATE_IDLE);
        return;
    }
    if (interrupt_expired) {
        bongo_cat_voice_barge_disarm(voice);
        return;
    }
    /* A candidate interruption in the room is not the echo of this answer:
       feeding it to the calibration would raise the floor until the next
       real interrupt could not clear it. */
    if (!armed) bongo_cat_voice_echo_update(voice, now, target);
}

void bongo_cat_voice_update(BongoCatVoice *voice, Uint64 now) {
    if (!voice) return;
    /* Main-thread halves of assistant tools run from here, every frame. */
    bongo_cat_assistant_pump(voice->assistant);
    SDL_LockMutex(voice->mutex);
    BongoCatVoiceState state = voice->state;
    bool expired = state == BONGO_CAT_VOICE_STATE_ERROR_HOLD &&
        voice->error_hold_until_ns && now >= voice->error_hold_until_ns;
    bool recognize_done = state == BONGO_CAT_VOICE_STATE_RECOGNIZING &&
        voice->recognize_until_ns && now >= voice->recognize_until_ns;
    bool think_expired = state == BONGO_CAT_VOICE_STATE_THINKING &&
        voice->thinking_until_ns && now >= voice->thinking_until_ns;
    bool listen_expired = state == BONGO_CAT_VOICE_STATE_LISTENING &&
        voice->listen_until_ns && now >= voice->listen_until_ns;
    SDL_UnlockMutex(voice->mutex);
    if (state == BONGO_CAT_VOICE_STATE_SPEAKING) {
        voice_update_speaking(voice, now);
        return;
    }
    if (expired) {
        bongo_cat_voice_set_state(voice, BONGO_CAT_VOICE_STATE_IDLE);
        state = BONGO_CAT_VOICE_STATE_IDLE;
    }
    if (think_expired) {
        SDL_LockMutex(voice->mutex);
        bool synthesizing = voice->synthesizing;
        voice->synthesizing = false;
        voice->thinking_until_ns = 0;
        SDL_UnlockMutex(voice->mutex);
        if (synthesizing) {
            if (voice->tts) bongo_cat_tts_cancel(voice->tts);
            SDL_LogWarn(BONGO_CAT_LOG_LIFECYCLE, "TTS synthesis timed out");
            hold_error(voice, "voice synthesis timed out");
            return;
        } else {
            if (voice->assistant) bongo_cat_assistant_cancel(
                voice->assistant);
            hold_error(voice, "assistant task timed out");
            return;
        }
    }
    if (recognize_done) {
        bongo_cat_voice_ring_reset(&voice->ring);
        SDL_LockMutex(voice->mutex);
        bool heard = voice->final_text[0] != '\0';
        SDL_UnlockMutex(voice->mutex);
        if (heard) {
            /* The flush after finish-task is still in flight, but the
               recognizer already committed this sentence: answer it rather
               than make the user wait out a watchdog. */
            if (voice->assistant) bongo_cat_voice_dispatch(voice);
            else bongo_cat_voice_set_state(voice, BONGO_CAT_VOICE_STATE_IDLE);
            return;
        }
        if (voice->asr) bongo_cat_asr_notify(voice->asr,
            BONGO_CAT_ASR_NOTIFY_ABORT);
        /* The gate opened on something the recognizer does not consider
           speech. Connection failures report themselves from the worker, so
           stay quiet here instead of blaming the network. */
        bongo_cat_voice_set_state(voice, BONGO_CAT_VOICE_STATE_IDLE);
        return;
    }
    if (listen_expired) {
        /* A transient noise opened the gate and closed it again below the
           minimum length, so no offset arrived: give the attempt up quietly
           instead of sitting in LISTENING forever. */
        if (voice->asr) bongo_cat_asr_notify(voice->asr,
            BONGO_CAT_ASR_NOTIFY_ABORT);
        SDL_LockMutex(voice->mutex);
        voice->listen_until_ns = 0;
        voice->partial_text[0] = '\0';
        SDL_UnlockMutex(voice->mutex);
        bongo_cat_voice_ring_reset(&voice->ring);
        bongo_cat_voice_gate_release(voice);
        SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
            "Voice listening closed: the recognizer heard no words");
        bongo_cat_voice_set_state(voice, BONGO_CAT_VOICE_STATE_IDLE);
        return;
    }
    if (state == BONGO_CAT_VOICE_STATE_IDLE && !voice->capturing &&
        !voice->shutting_down) {
        if (!bongo_cat_voice_capture_start(voice))
            hold_error(voice, voice->error);
        else bongo_cat_voice_set_state(voice, BONGO_CAT_VOICE_STATE_IDLE);
    }
}

void bongo_cat_voice_status(const BongoCatVoice *voice,
    BongoCatVoiceStatus *snapshot) {
    if (!snapshot) return;
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->state = BONGO_CAT_VOICE_STATE_OFF;
    if (!voice) return;
    SDL_LockMutex(voice->mutex);
    snapshot->state = voice->state;
    snprintf(snapshot->error, sizeof(snapshot->error), "%s", voice->error);
    if (voice->partial_text[0])
        snprintf(snapshot->text, sizeof(snapshot->text), "%s",
            voice->partial_text);
    else if (voice->final_text[0])
        snprintf(snapshot->text, sizeof(snapshot->text), "%s",
            voice->final_text);
    else if (voice->result_text[0])
        snprintf(snapshot->text, sizeof(snapshot->text), "%s",
            voice->result_text);
    SDL_UnlockMutex(voice->mutex);
    BongoCatVoice *mutable_voice = (BongoCatVoice *)voice;
    snapshot->utterances = SDL_GetAtomicInt(&mutable_voice->utterances);
    snapshot->level = (float)SDL_GetAtomicInt(&mutable_voice->level_milli)
        / 1000.0f;
}

void bongo_cat_voice_cancel(BongoCatVoice *voice) {
    if (!voice) return;
    SDL_LockMutex(voice->mutex);
    BongoCatVoiceState state = voice->state;
    SDL_UnlockMutex(voice->mutex);
    if (state == BONGO_CAT_VOICE_STATE_OFF ||
        state == BONGO_CAT_VOICE_STATE_IDLE) return;
    bongo_cat_voice_new_turn(voice);
    if (voice->asr) bongo_cat_asr_notify(voice->asr,
        BONGO_CAT_ASR_NOTIFY_ABORT);
    if (voice->assistant) bongo_cat_assistant_cancel(voice->assistant);
    if (voice->tts) bongo_cat_tts_cancel(voice->tts);
    char path[BONGO_CAT_PATH_CAP];
    SDL_LockMutex(voice->mutex);
    snprintf(path, sizeof(path), "%s", voice->tts_path);
    voice->partial_text[0] = '\0';
    voice->final_text[0] = '\0';
    voice->listen_until_ns = 0;
    voice->thinking_until_ns = 0;
    voice->speaking_until_ns = 0;
    voice->synthesizing = false;
    voice->mouth_value = 0.0f;
    SDL_UnlockMutex(voice->mutex);
    if (path[0]) bongo_cat_audio_stop_path(voice->app->audio, path);
    bongo_cat_live2d_set_parameter(voice->app->live2d, "ParamMouthOpenY",
        0.0f);
    bongo_cat_voice_echo_reset(voice);
    bongo_cat_voice_ring_reset(&voice->ring);
    bongo_cat_voice_gate_release(voice);
    bongo_cat_voice_set_state(voice, BONGO_CAT_VOICE_STATE_IDLE);
}

void bongo_cat_voice_reload(BongoCatVoice *voice) {
    if (!voice) return;
    bongo_cat_voice_cancel(voice);
    bongo_cat_voice_kill_workers(voice);
    bongo_cat_voice_spawn_workers(voice);
    if (!voice->asr) bongo_cat_voice_capture_stop(voice);
    bongo_cat_voice_set_state(voice, voice->asr ? BONGO_CAT_VOICE_STATE_IDLE
        : BONGO_CAT_VOICE_STATE_OFF);
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE, "Voice assistant reloaded");
}

void bongo_cat_voice_destroy(BongoCatVoice *voice) {
    if (!voice) return;
    bongo_cat_voice_kill_workers(voice);
    bongo_cat_voice_capture_stop(voice);
    char path[BONGO_CAT_PATH_CAP];
    SDL_LockMutex(voice->mutex);
    snprintf(path, sizeof(path), "%s", voice->tts_path);
    voice->tts_path[0] = '\0';
    SDL_UnlockMutex(voice->mutex);
    if (path[0]) bongo_cat_audio_stop_path(voice->app->audio, path);
    SDL_LockMutex(voice->mutex);
    voice->shutting_down = true;
    SDL_UnlockMutex(voice->mutex);
    SDL_Event event;
    while (SDL_PeepEvents(&event, 1, SDL_GETEVENT, voice->event_type,
        voice->event_type) > 0) {}
    SDL_DestroyMutex(voice->mutex);
    free(voice->tts_envelope);
    free(voice);
}
