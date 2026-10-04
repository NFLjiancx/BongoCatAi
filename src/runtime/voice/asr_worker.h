#ifndef BONGO_CAT_ASR_WORKER_H
#define BONGO_CAT_ASR_WORKER_H

#include <stdbool.h>

#include "dashscope_json.h"

typedef struct BongoCatVoice BongoCatVoice;
typedef struct BongoCatAsr BongoCatAsr;

/* Owner of the DashScope WebSocket connection and of the capture ring as
   its only consumer. Runs a private worker thread; all state lives there.
   Results travel back to the main loop as BONGO_CAT_VOICE_EV_ASR_* events
   posted through the voice service, with partial/final text written under
   the voice mutex before each post. */

/* Why the worker should change the current recognition task. */
typedef enum BongoCatAsrNotify {
    /* VAD onset: prepare a task so the next ring data lands in it. */
    BONGO_CAT_ASR_NOTIFY_BEGIN = 0,
    /* VAD offset: close the current task and wait for its final sentence. */
    BONGO_CAT_ASR_NOTIFY_END,
    /* Cancel/shutdown: drop any in-flight task without reporting it. */
    BONGO_CAT_ASR_NOTIFY_ABORT
} BongoCatAsrNotify;

/* Returns NULL when the WebSocket backend is unavailable. The voice pointer
   must outlive the worker. */
BongoCatAsr *bongo_cat_asr_create(BongoCatVoice *voice,
    const BongoCatDashscopeAsrConfig *config);

/* Connects and keeps the connection alive (5 s backoff after drops). api_key
   is copied; the worker never logs or stores it beyond that copy. Safe to
   call again after stop() to reuse the worker. */
bool bongo_cat_asr_start(BongoCatAsr *asr, const char *api_key);

/* Main-thread-side notification; takes the voice lock off the caller and
   returns without blocking on the worker. */
void bongo_cat_asr_notify(BongoCatAsr *asr, BongoCatAsrNotify notify);

bool bongo_cat_asr_running(const BongoCatAsr *asr);

/* Joins the worker thread; pending tasks are aborted silently. */
void bongo_cat_asr_stop(BongoCatAsr *asr);

/* Implies stop(). */
void bongo_cat_asr_destroy(BongoCatAsr *asr);

#endif
