#ifndef BONGO_CAT_VOICE_INTERNAL_H
#define BONGO_CAT_VOICE_INTERNAL_H

#include "voice_service.h"
#include "voice_ring.h"
#include "voice_vad.h"
#include "assistant_runner.h"
#include "bongo_cat/app.h"
#include "bongo_cat/path.h"

#include <SDL3/SDL.h>
#include <miniaudio.h>

#define BONGO_CAT_VOICE_TEXT_CAP 2048
/* Event codes are packed with the turn epoch into one pointer, so they have to
   fit the low half of it. */
#define BONGO_CAT_VOICE_CODE_MASK 0xFFFFu
#define BONGO_CAT_VOICE_ERROR_HOLD_NS (5ull * 1000000000ull)
#define BONGO_CAT_LISTEN_IDLE_NS (2ull * 1000000000ull)
/* Progress must keep arriving, not the whole task: a tool round is slow. */
#define BONGO_CAT_ASSISTANT_WATCHDOG_NS (200ull * 1000000000ull)

struct BongoCatAsr;
struct BongoCatTts;

struct BongoCatVoice {
    BongoCatApp *app;
    SDL_Mutex *mutex;
    Uint32 event_type;
    BongoCatVoiceState state;
    char error[256];
    char partial_text[BONGO_CAT_VOICE_TEXT_CAP];
    char final_text[BONGO_CAT_VOICE_TEXT_CAP];
    char result_text[BONGO_CAT_VOICE_TEXT_CAP];
    Uint64 error_hold_until_ns;
    Uint64 recognize_until_ns;
    Uint64 thinking_until_ns;
    /* LISTENING has no offset of its own when a transient noise closes the
       gate too early, so silence for this long abandons the attempt. */
    Uint64 listen_until_ns;
    bool shutting_down;
    /* Identifies the current answer. Workers stamp the value they were started
       for onto every event they post, so a result that arrives after the user
       interrupted is dropped instead of being spoken as the new answer. */
    unsigned turn_epoch;
    struct BongoCatAsr *asr;
    BongoCatAssistantRunner *assistant;
    struct BongoCatTts *tts;
    /* TTS output, guarded by mutex and consumed by the SPEAKING state. */
    char tts_path[BONGO_CAT_PATH_CAP];
    float *tts_envelope;
    size_t tts_envelope_count;
    int tts_sample_rate;
    bool synthesizing;
    Uint64 speaking_until_ns;
    float mouth_value;
    ma_device capture_device;
    bool capturing;
    BongoCatVoiceRing ring;
    BongoCatVad vad;
    int16_t lookback[16000u * BONGO_CAT_VAD_LOOKBACK_MS / 1000u];
    size_t lookback_pos, lookback_fill;
    int16_t frame_carry[BONGO_CAT_VAD_FRAME_SAMPLES];
    unsigned frame_carry_count;
    SDL_AtomicInt utterances;
    SDL_AtomicInt level_milli; /* Capture energy x1000 for the UI meter. */
    /* Turn boundary: the main thread bumps this when an utterance ends, and
       the capture thread, which owns the VAD, releases its gate when it sees
       a new value. The moment the gate opened, guarded by mutex. */
    SDL_AtomicInt turn;
    unsigned turn_seen;
    Uint64 turn_open_ns;
    unsigned meter_ticks;
    /* Echo cancellation for barge-in: the main thread maps the TTS envelope
       onto the captured level while speaking and publishes the resulting
       floor for the capture thread; only level_milli and barge_ref_milli are
       shared, the rest is main-thread state. */
    float echo_gain;
    float barge_ref;
    Uint64 barge_calibrate_until_ns;
    SDL_AtomicInt barge_ref_milli;
};

void bongo_cat_voice_post(BongoCatVoice *voice, int code);
/* Post an event tagged with the turn it belongs to. Untagged posts carry 0 and
   are never dropped as stale. */
void bongo_cat_voice_post_turn(BongoCatVoice *voice, int code, unsigned epoch);
/* Enter ERROR_HOLD with the given message; safe from worker threads. */
void bongo_cat_voice_report_error(BongoCatVoice *voice, const char *message);
/* Resolves the DashScope key from BONGO_CAT_DASHSCOPE_API_KEY or
   <config_root>/dashscope.key. The key must never be logged or copied. */
bool bongo_cat_voice_resolve_api_key(BongoCatVoice *voice, char *out,
    size_t cap);

/* voice_workers.c: build or tear down the ASR/assistant/TTS workers from the
   current settings and API key, so the AI page can apply changes in place. */
void bongo_cat_voice_spawn_workers(BongoCatVoice *voice);
void bongo_cat_voice_kill_workers(BongoCatVoice *voice);

/* Sub-codes for capture events. They live in a range disjoint from the
   BONGO_CAT_VOICE_EV_* codes pushed by the ASR/assistant workers. */
enum {
    BONGO_CAT_VOICE_CAPTURE_ONSET = 101,
    BONGO_CAT_VOICE_CAPTURE_OFFSET = 102,
    /* Wake-up tick while speech is active so the bubble meter animates. */
    BONGO_CAT_VOICE_CAPTURE_LEVEL = 103
};

/* voice_capture.c */
bool bongo_cat_voice_capture_start(BongoCatVoice *voice);
void bongo_cat_voice_capture_stop(BongoCatVoice *voice);

/* voice_barge.c: full-duplex operation while the pet speaks. echo_begin arms
   a short calibration window, echo_update tracks the captured level against
   the playback envelope, and echo_floor is what the capture thread feeds the
   VAD as an extra onset gate. reset forgets everything (playback ended). */
void bongo_cat_voice_echo_begin(BongoCatVoice *voice, Uint64 now);
void bongo_cat_voice_echo_update(BongoCatVoice *voice, Uint64 now,
    float envelope);
void bongo_cat_voice_echo_reset(BongoCatVoice *voice);
float bongo_cat_voice_echo_floor(BongoCatVoice *voice);
/* Cut playback short and hand the microphone back: SPEAKING -> LISTENING. */
void bongo_cat_voice_barge_in(BongoCatVoice *voice);

/* voice_turn.c: the boundary between listening and answering. */
void bongo_cat_voice_set_state(BongoCatVoice *voice, BongoCatVoiceState state);
/* Start a new answer: returns the epoch to stamp on the workers just launched
   and makes every event from the previous one stale. Main thread only. */
unsigned bongo_cat_voice_new_turn(BongoCatVoice *voice);
/* Ask the capture thread to close its VAD gate (main thread only). */
void bongo_cat_voice_gate_release(BongoCatVoice *voice);
/* End the open utterance and let the recognizer flush it; reason names the
   detector that closed the turn and only appears in the log. */
void bongo_cat_voice_turn_close(BongoCatVoice *voice, const char *reason);
/* Hand the finalized transcript to the assistant (THINKING). */
void bongo_cat_voice_dispatch(BongoCatVoice *voice);

#endif
