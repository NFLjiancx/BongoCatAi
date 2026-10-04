#ifndef BONGO_CAT_BUBBLE_WINDOW_H
#define BONGO_CAT_BUBBLE_WINDOW_H

#include "bongo_cat/voice_status.h"

#include <SDL3/SDL.h>

#include <stdbool.h>

typedef struct BongoCatBubble BongoCatBubble;

typedef struct BongoCatBubbleLabels {
    const char *state_names[BONGO_CAT_VOICE_STATE_COUNT];
    const char *cancel;
    const char *close;
    const char *utterances;
} BongoCatBubbleLabels;

BongoCatBubble *bongo_cat_bubble_create(void);
void bongo_cat_bubble_destroy(BongoCatBubble *bubble);
/* Main-thread tick. The native window is created lazily on first reveal and
   follows the pet window while visible. status and labels must outlive the
   call; their content is copied into the bubble. */
void bongo_cat_bubble_update(BongoCatBubble *bubble, SDL_Window *owner,
    const BongoCatVoiceStatus *status, const BongoCatBubbleLabels *labels,
    Uint64 now_ns);
/* Consumes events addressed to the bubble window. Returns true when the
   event belonged to the bubble and must not reach the pet logic. */
bool bongo_cat_bubble_event(BongoCatBubble *bubble, const SDL_Event *event);
bool bongo_cat_bubble_take_cancel_request(BongoCatBubble *bubble);

#endif
