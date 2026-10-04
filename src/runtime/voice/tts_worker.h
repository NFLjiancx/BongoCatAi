#ifndef BONGO_CAT_TTS_WORKER_H
#define BONGO_CAT_TTS_WORKER_H

#include <stdbool.h>
#include <stddef.h>

typedef struct BongoCatTts BongoCatTts;
typedef struct BongoCatVoice BongoCatVoice;

typedef struct BongoCatTtsConfig {
    const char *model;       /* e.g. "qwen-audio-3.1-tts-flash". */
    const char *voice;       /* e.g. "yunhuanhuan_v3.1". */
    int sample_rate;         /* Requested PCM rate, e.g. 24000. */
} BongoCatTtsConfig;

/* The worker synthesizes one utterance at a time on its own thread. On
   success it stores the WAV path, the mouth envelope and the sample rate in
   the voice (under its mutex) and posts BONGO_CAT_VOICE_EV_TTS_READY;
   failures log a warning and post BONGO_CAT_VOICE_EV_TTS_ERROR. */
BongoCatTts *bongo_cat_tts_create(BongoCatVoice *voice,
    const BongoCatTtsConfig *config);
bool bongo_cat_tts_start(BongoCatTts *tts, const char *api_key);
/* Returns false when the worker is missing or the text is empty. The epoch is
   the voice turn being answered; READY/ERROR carry it back. A request that
   arrives while an answer is still synthesizing replaces it, because only the
   last word of a turn is ever meant to be heard. */
bool bongo_cat_tts_speak(BongoCatTts *tts, const char *text, unsigned epoch);
void bongo_cat_tts_cancel(BongoCatTts *tts);
bool bongo_cat_tts_running(const BongoCatTts *tts);
void bongo_cat_tts_stop(BongoCatTts *tts);
void bongo_cat_tts_destroy(BongoCatTts *tts);

#endif
