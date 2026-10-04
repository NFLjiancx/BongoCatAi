#ifndef BONGO_CAT_DASHSCOPE_JSON_H
#define BONGO_CAT_DASHSCOPE_JSON_H

#include <stdbool.h>
#include <stddef.h>

/* Pure DashScope WebSocket inference protocol codecs. These functions do
   no I/O so they stay unit-testable; transport lives in ws_client and the
   ASR/TTS workers only move the produced strings and parsed events.

   Protocol (paraformer-realtime-v2, duplex streaming):
   1. client sends run-task JSON, then raw binary PCM frames;
   2. server answers task-started, then result-generated events while the
      utterance is recognized;
   3. client sends finish-task to force the last sentence;
   4. server answers the final result-generated events and task-finished,
      or task-failed with error_code/error_message at any point. */

#define BONGO_CAT_DS_TASK_ID_CAP 64
#define BONGO_CAT_DS_TEXT_CAP 2048
#define BONGO_CAT_DS_ERROR_CAP 256

typedef struct BongoCatDashscopeAsrConfig {
    const char *model;       /* e.g. "paraformer-realtime-v2". */
    const char *format;      /* "pcm" keeps the stream decoder-free. */
    int sample_rate;         /* 16000 for the capture pipeline. */
    const char *const *language_hints; /* e.g. {"zh"}; NULL for auto. */
    size_t language_hint_count;
    bool disfluency_removal_enabled;
    bool semantic_punctuation_enabled;
} BongoCatDashscopeAsrConfig;

/* Writes a random UUIDv4-shaped task id. Always succeeds. */
void bongo_cat_dashscope_new_task_id(char *out, size_t capacity);

/* Builders serialize the run-task/finish-task control frames into `out`.
   They return false when the buffer is too small or inputs are invalid and
   leave `out` untouched. */
bool bongo_cat_dashscope_build_run_task(char *out, size_t capacity,
    const char *task_id, const BongoCatDashscopeAsrConfig *config);
bool bongo_cat_dashscope_build_finish_task(char *out, size_t capacity,
    const char *task_id);

typedef struct BongoCatDashscopeTtsConfig {
    const char *model;  /* e.g. "qwen-audio-3.1-tts-flash". */
    const char *voice;  /* e.g. "yunhuanhuan_v3.1" (case sensitive). */
    const char *format; /* "pcm" keeps the pipeline decoder-free. */
    int sample_rate;    /* Output rate, e.g. 24000. */
} BongoCatDashscopeTtsConfig;

/* TTS is duplex too: run-task (parameters only), then one continue-task
   carrying payload.input.text, then finish-task; audio arrives as binary
   PCM frames interleaved with result-generated events. */
bool bongo_cat_dashscope_build_tts_run_task(char *out, size_t capacity,
    const char *task_id, const BongoCatDashscopeTtsConfig *config);
bool bongo_cat_dashscope_build_tts_continue_task(char *out, size_t capacity,
    const char *task_id, const char *text);

typedef enum BongoCatDashscopeEventKind {
    BONGO_CAT_DS_EVENT_IGNORED = 0, /* Not a protocol event we consume. */
    BONGO_CAT_DS_EVENT_TASK_STARTED,
    BONGO_CAT_DS_EVENT_RESULT,
    BONGO_CAT_DS_EVENT_TASK_FINISHED,
    BONGO_CAT_DS_EVENT_TASK_FAILED
} BongoCatDashscopeEventKind;

typedef struct BongoCatDashscopeEvent {
    BongoCatDashscopeEventKind kind;
    char task_id[BONGO_CAT_DS_TASK_ID_CAP];
    int status_code;
    char error_code[64];
    char error_message[BONGO_CAT_DS_ERROR_CAP];
    /* RESULT fields: the full current-sentence text as recommended by the
       paraformer protocol (payload.output.sentence.text). sentence_end
       marks the completed final sentence for the current task. */
    char text[BONGO_CAT_DS_TEXT_CAP];
    bool sentence_end;
    bool heartbeat; /* Heartbeat results carry no user-visible text. */
} BongoCatDashscopeEvent;

/* Parses one server text frame. Returns false only for malformed JSON;
   unknown events yield kind == BONGO_CAT_DS_EVENT_IGNORED. */
bool bongo_cat_dashscope_parse_event(const char *json, size_t length,
    BongoCatDashscopeEvent *event);

#endif
