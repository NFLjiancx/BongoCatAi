#ifndef BONGO_CAT_ASSISTANT_INTERNAL_H
#define BONGO_CAT_ASSISTANT_INTERNAL_H

#include "assistant_runner.h"
#include "bongo_cat/config.h"

#include <SDL3/SDL.h>
#include <yyjson.h>

#define ASSISTANT_MODEL_CAP 96
#define ASSISTANT_API_KEY_CAP 512
#define ASSISTANT_PROMPT_CAP 4096
#define ASSISTANT_RESPONSE_CAP (512 * 1024)
#define ASSISTANT_HISTORY_CAP (256 * 1024)
#define ASSISTANT_TOOL_RESULT_CAP 8192
#define ASSISTANT_FILE_READ_CAP 32768
#define ASSISTANT_MAX_ROUNDS 8

enum AssistantActionKind {
    ACTION_LIST_BEHAVIORS,
    ACTION_PLAY_BEHAVIOR,
    ACTION_SET_VISIBILITY,
    ACTION_SET_SOUND
};

struct AssistantAction {
    enum AssistantActionKind kind;
    char arg[512];
    bool flag;
    bool pending, done;
    char result[ASSISTANT_TOOL_RESULT_CAP];
};

struct BongoCatAssistantRunner {
    BongoCatVoice *voice;
    char model[ASSISTANT_MODEL_CAP];
    char *prompt; /* Owned by the worker; freed when it finishes. */
    /* Turn this run answers; set before the thread starts, read only by it. */
    unsigned epoch;
    SDL_Thread *thread;
    SDL_AtomicInt cancel;
    SDL_AtomicInt running;
    SDL_Mutex *action_mutex;
    SDL_Condition *action_cond;
    struct AssistantAction action;
};

/* Serialized message objects joined by ',', without the surrounding
   brackets. Empty (length 1) when only the system turn exists. */
typedef struct {
    char *items;
    size_t length; /* Bytes used, including the NUL. */
} AssistantHistory;

/* assistant_runner.c */
void bongo_cat_assistant_progress(BongoCatAssistantRunner *runner,
    const char *line);
void bongo_cat_assistant_publish(BongoCatAssistantRunner *runner,
    const char *text, bool is_error);

/* assistant_http.c */
/* Serialize any text as a JSON string literal; caller frees. */
char *bongo_cat_assistant_json_quote(const char *text);
bool bongo_cat_assistant_history_init(AssistantHistory *history);
void bongo_cat_assistant_history_free(AssistantHistory *history);
bool bongo_cat_assistant_history_append_text(AssistantHistory *history,
    const char *role, const char *tool_call_id, const char *content);
bool bongo_cat_assistant_history_append_assistant(AssistantHistory *history,
    yyjson_val *content, yyjson_val *tool_calls);
/* Whole chat-completions body, SDL-allocated; caller frees with
   SDL_free(). */
char *bongo_cat_assistant_build_request(const char *model,
    const AssistantHistory *history, const char *tools_json);
/* Blocking HTTPS POST. Returns the parsed document, or NULL with a message
   in error; an empty error string means the request was cancelled. */
yyjson_doc *bongo_cat_assistant_chat_request(BongoCatAssistantRunner *runner,
    const char *api_key, const char *request_json, char *error, size_t cap);

/* assistant_tools.c */
void bongo_cat_assistant_tool_execute(BongoCatAssistantRunner *runner,
    const char *name, const char *arguments, char *out, size_t cap);
/* Serializes the OpenAI-format tool list; caller releases the result
   with free(), not SDL_free(), because yyjson allocated it. */
char *bongo_cat_assistant_tools_json(void);
/* Reads one string argument out of a tool-call arguments JSON object. */
bool bongo_cat_assistant_arg_string(const char *arguments, const char *key,
    char *out, size_t cap);
/* Formats the local date and time; used by the system prompt. */
void bongo_cat_assistant_now(char *out, size_t cap);

#endif
