#ifndef BONGO_CAT_ASSISTANT_RUNNER_H
#define BONGO_CAT_ASSISTANT_RUNNER_H

#include <stdbool.h>

typedef struct BongoCatVoice BongoCatVoice;
typedef struct BongoCatAssistantRunner BongoCatAssistantRunner;

/* In-process voice assistant: a DashScope pay-as-you-go chat completion
   loop with locally executed tools (pet behaviors, files, commands). One
   task at a time; progress and results publish onto the voice status under
   its mutex. model may be NULL for the default. */
BongoCatAssistantRunner *bongo_cat_assistant_create(BongoCatVoice *voice,
    const char *model);
/* Starts one answer for the given turn epoch; events are tagged with it so the
   voice service can drop results of turns the user already abandoned. */
bool bongo_cat_assistant_start(BongoCatAssistantRunner *runner,
    const char *prompt, unsigned epoch);
void bongo_cat_assistant_cancel(BongoCatAssistantRunner *runner);
bool bongo_cat_assistant_running(const BongoCatAssistantRunner *runner);
/* Cancels and joins the worker; safe to call when idle. */
void bongo_cat_assistant_stop(BongoCatAssistantRunner *runner);
void bongo_cat_assistant_destroy(BongoCatAssistantRunner *runner);
/* Runs queued main-thread tool requests; call from the voice update tick.
   Safe with NULL. */
void bongo_cat_assistant_pump(BongoCatAssistantRunner *runner);

#endif
