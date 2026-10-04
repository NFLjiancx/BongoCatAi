#ifndef BONGO_CAT_VOICE_STATUS_H
#define BONGO_CAT_VOICE_STATUS_H

#include <stdbool.h>

typedef enum BongoCatVoiceState {
    BONGO_CAT_VOICE_STATE_OFF,
    BONGO_CAT_VOICE_STATE_IDLE,
    BONGO_CAT_VOICE_STATE_LISTENING,
    BONGO_CAT_VOICE_STATE_RECOGNIZING,
    BONGO_CAT_VOICE_STATE_THINKING,
    BONGO_CAT_VOICE_STATE_SPEAKING,
    BONGO_CAT_VOICE_STATE_ERROR_HOLD
} BongoCatVoiceState;

#define BONGO_CAT_VOICE_STATE_COUNT 7

typedef struct BongoCatVoiceStatus {
    BongoCatVoiceState state;
    char error[256];
    /* Latest bubble body text: ASR partial/final or assistant result. */
    char text[2048];
    int utterances;
    float level; /* 0..1 smoothed capture energy for the meter. */
} BongoCatVoiceStatus;

#endif
