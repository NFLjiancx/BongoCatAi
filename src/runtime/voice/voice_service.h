#ifndef BONGO_CAT_VOICE_SERVICE_H
#define BONGO_CAT_VOICE_SERVICE_H

#include "bongo_cat/voice_status.h"

#include <SDL3/SDL_stdinc.h>

typedef struct BongoCatApp BongoCatApp;
typedef union SDL_Event SDL_Event;
typedef struct BongoCatVoice BongoCatVoice;

/* Event codes carried in event.user.data2 of the service event type. */
enum {
    BONGO_CAT_VOICE_EV_CAPTURE = 1,
    BONGO_CAT_VOICE_EV_ASR_PARTIAL,
    BONGO_CAT_VOICE_EV_ASR_FINAL,
    BONGO_CAT_VOICE_EV_ASR_ERROR,
    BONGO_CAT_VOICE_EV_ASSISTANT_PROGRESS,
    BONGO_CAT_VOICE_EV_ASSISTANT_RESULT,
    BONGO_CAT_VOICE_EV_ASSISTANT_ERROR,
    BONGO_CAT_VOICE_EV_TTS_READY,
    BONGO_CAT_VOICE_EV_TTS_ERROR
};

BongoCatVoice *bongo_cat_voice_create(BongoCatApp *app);
bool bongo_cat_voice_event(BongoCatVoice *voice, const SDL_Event *event);
void bongo_cat_voice_update(BongoCatVoice *voice, Uint64 now);
void bongo_cat_voice_status(const BongoCatVoice *voice,
    BongoCatVoiceStatus *snapshot);
/* Drop any in-flight capture/recognition work and return to IDLE. Called
   from the bubble [Cancel] button on the main thread. */
void bongo_cat_voice_cancel(BongoCatVoice *voice);
/* Rebuilds the workers from the settings and key on disk so an AI page edit
   applies without restarting the app. Main thread only. */
void bongo_cat_voice_reload(BongoCatVoice *voice);
void bongo_cat_voice_destroy(BongoCatVoice *voice);

#endif
