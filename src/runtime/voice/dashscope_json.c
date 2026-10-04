#include "dashscope_json.h"

#include <yyjson.h>

#include <SDL3/SDL.h>

#include <stdlib.h>
#include <string.h>

/* A turn ends at whichever boundary arrives first: the local energy gate after
   a short silence, or this sentence end from the recognizer. Keep the server
   window near its default so a noisy room, where the energy gate cannot find a
   quiet half second, still closes within about a second of real speech. */
#define BONGO_CAT_DS_MAX_SENTENCE_SILENCE_MS 800

static void format_task_id(char *out, size_t capacity, Uint64 *state) {
    static const char *hex_digits = "0123456789abcdef";
    char hex[33];
    for (int i = 0; i < 32; ++i)
        hex[i] = hex_digits[((unsigned)SDL_rand_bits_r(state)) & 0xfu];
    hex[12] = '4'; /* version 4 */
    hex[16] = hex_digits[8u + ((unsigned)hex[16] & 3u)]; /* variant 10xx */
    int written = SDL_snprintf(out, capacity,
        "%.8s-%.4s-%.4s-%.4s-%.12s", hex, hex + 8, hex + 12, hex + 16,
        hex + 20);
    if (written < 0 || (size_t)written >= capacity) out[0] = '\0';
}

void bongo_cat_dashscope_new_task_id(char *out, size_t capacity) {
    if (!out || capacity < 37) return;
    Uint64 state = SDL_GetTicksNS() ^ (Uint64)(uintptr_t)out;
    out[0] = '\0';
    format_task_id(out, capacity, &state);
}

static bool write_document(char *out, size_t capacity, yyjson_mut_doc *doc) {
    size_t length = 0;
    char *json = yyjson_mut_write(doc, 0, &length);
    yyjson_mut_doc_free(doc);
    if (!json) return false;
    bool ok = length && length + 1 <= capacity;
    if (ok) {
        memcpy(out, json, length);
        out[length] = '\0';
    }
    free(json);
    return ok;
}

static bool task_id_valid(const char *task_id) {
    return task_id && task_id[0]
        && strlen(task_id) < BONGO_CAT_DS_TASK_ID_CAP;
}

bool bongo_cat_dashscope_build_run_task(char *out, size_t capacity,
    const char *task_id, const BongoCatDashscopeAsrConfig *config) {
    if (!out || !capacity || !task_id_valid(task_id) || !config
        || !config->model || !*config->model || !config->format
        || !*config->format || config->sample_rate <= 0
        || (!config->language_hints && config->language_hint_count)
        || config->language_hint_count > 8) return false;
    for (size_t i = 0; i < config->language_hint_count; ++i)
        if (!config->language_hints[i] || !*config->language_hints[i])
            return false;
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) return false;
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_val *header = yyjson_mut_obj(doc);
    yyjson_mut_val *payload = yyjson_mut_obj(doc);
    yyjson_mut_val *parameters = yyjson_mut_obj(doc);
    yyjson_mut_val *input = yyjson_mut_obj(doc);
    if (!root || !header || !payload || !parameters || !input) {
        yyjson_mut_doc_free(doc);
        return false;
    }
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, header, "action", "run-task");
    yyjson_mut_obj_add_strcpy(doc, header, "task_id", task_id);
    yyjson_mut_obj_add_str(doc, header, "streaming", "duplex");
    yyjson_mut_obj_add_str(doc, payload, "task_group", "audio");
    yyjson_mut_obj_add_str(doc, payload, "task", "asr");
    yyjson_mut_obj_add_str(doc, payload, "function", "recognition");
    yyjson_mut_obj_add_strcpy(doc, payload, "model", config->model);
    yyjson_mut_obj_add_strcpy(doc, parameters, "format", config->format);
    yyjson_mut_obj_add_int(doc, parameters, "sample_rate",
        config->sample_rate);
    yyjson_mut_obj_add_int(doc, parameters, "max_sentence_silence",
        BONGO_CAT_DS_MAX_SENTENCE_SILENCE_MS);
    if (config->language_hint_count) {
        yyjson_mut_val *hints = yyjson_mut_arr(doc);
        for (size_t i = 0; i < config->language_hint_count; ++i)
            yyjson_mut_arr_add_strcpy(doc, hints,
                config->language_hints[i]);
        yyjson_mut_obj_add_val(doc, parameters, "language_hints", hints);
    }
    if (config->disfluency_removal_enabled)
        yyjson_mut_obj_add_bool(doc, parameters,
            "disfluency_removal_enabled", true);
    if (config->semantic_punctuation_enabled)
        yyjson_mut_obj_add_bool(doc, parameters,
            "semantic_punctuation_enabled", true);
    yyjson_mut_obj_add_val(doc, payload, "parameters", parameters);
    yyjson_mut_obj_add_val(doc, payload, "input", input);
    yyjson_mut_obj_add_val(doc, root, "header", header);
    yyjson_mut_obj_add_val(doc, root, "payload", payload);
    return write_document(out, capacity, doc);
}

bool bongo_cat_dashscope_build_finish_task(char *out, size_t capacity,
    const char *task_id) {
    if (!out || !capacity || !task_id_valid(task_id)) return false;
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) return false;
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_val *header = yyjson_mut_obj(doc);
    yyjson_mut_val *payload = yyjson_mut_obj(doc);
    yyjson_mut_val *input = yyjson_mut_obj(doc);
    if (!root || !header || !payload || !input) {
        yyjson_mut_doc_free(doc);
        return false;
    }
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, header, "action", "finish-task");
    yyjson_mut_obj_add_strcpy(doc, header, "task_id", task_id);
    yyjson_mut_obj_add_str(doc, header, "streaming", "duplex");
    yyjson_mut_obj_add_val(doc, payload, "input", input);
    yyjson_mut_obj_add_val(doc, root, "header", header);
    yyjson_mut_obj_add_val(doc, root, "payload", payload);
    return write_document(out, capacity, doc);
}

bool bongo_cat_dashscope_build_tts_run_task(char *out, size_t capacity,
    const char *task_id, const BongoCatDashscopeTtsConfig *config) {
    if (!out || !capacity || !task_id_valid(task_id) || !config
        || !config->model || !*config->model || !config->voice
        || !*config->voice || !config->format || !*config->format
        || config->sample_rate <= 0) return false;
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) return false;
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_val *header = yyjson_mut_obj(doc);
    yyjson_mut_val *payload = yyjson_mut_obj(doc);
    yyjson_mut_val *parameters = yyjson_mut_obj(doc);
    yyjson_mut_val *input = yyjson_mut_obj(doc);
    if (!root || !header || !payload || !parameters || !input) {
        yyjson_mut_doc_free(doc);
        return false;
    }
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, header, "action", "run-task");
    yyjson_mut_obj_add_strcpy(doc, header, "task_id", task_id);
    yyjson_mut_obj_add_str(doc, header, "streaming", "duplex");
    yyjson_mut_obj_add_str(doc, payload, "task_group", "audio");
    yyjson_mut_obj_add_str(doc, payload, "task", "tts");
    yyjson_mut_obj_add_str(doc, payload, "function", "SpeechSynthesizer");
    yyjson_mut_obj_add_strcpy(doc, payload, "model", config->model);
    yyjson_mut_obj_add_str(doc, parameters, "text_type", "PlainText");
    yyjson_mut_obj_add_strcpy(doc, parameters, "voice", config->voice);
    yyjson_mut_obj_add_strcpy(doc, parameters, "format", config->format);
    yyjson_mut_obj_add_int(doc, parameters, "sample_rate",
        config->sample_rate);
    yyjson_mut_obj_add_val(doc, payload, "parameters", parameters);
    yyjson_mut_obj_add_val(doc, payload, "input", input);
    yyjson_mut_obj_add_val(doc, root, "header", header);
    yyjson_mut_obj_add_val(doc, root, "payload", payload);
    return write_document(out, capacity, doc);
}

bool bongo_cat_dashscope_build_tts_continue_task(char *out, size_t capacity,
    const char *task_id, const char *text) {
    if (!out || !capacity || !task_id_valid(task_id) || !text || !text[0])
        return false;
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) return false;
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_val *header = yyjson_mut_obj(doc);
    yyjson_mut_val *payload = yyjson_mut_obj(doc);
    yyjson_mut_val *input = yyjson_mut_obj(doc);
    if (!root || !header || !payload || !input) {
        yyjson_mut_doc_free(doc);
        return false;
    }
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, header, "action", "continue-task");
    yyjson_mut_obj_add_strcpy(doc, header, "task_id", task_id);
    yyjson_mut_obj_add_str(doc, header, "streaming", "duplex");
    yyjson_mut_obj_add_strcpy(doc, input, "text", text);
    yyjson_mut_obj_add_val(doc, payload, "input", input);
    yyjson_mut_obj_add_val(doc, root, "header", header);
    yyjson_mut_obj_add_val(doc, root, "payload", payload);
    return write_document(out, capacity, doc);
}

static void copy_string(char *dst, size_t capacity, const char *src) {
    if (!capacity) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    SDL_strlcpy(dst, src, capacity);
}

bool bongo_cat_dashscope_parse_event(const char *json, size_t length,
    BongoCatDashscopeEvent *event) {
    if (!json || !event) return false;
    if (!length) length = strlen(json);
    yyjson_doc *doc = yyjson_read(json, length, 0);
    if (!doc) return false;
    memset(event, 0, sizeof(*event));
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *header = yyjson_obj_get(root, "header");
    const char *name = header
        ? yyjson_get_str(yyjson_obj_get(header, "event")) : NULL;
    if (name && strcmp(name, "task-started") == 0)
        event->kind = BONGO_CAT_DS_EVENT_TASK_STARTED;
    else if (name && strcmp(name, "result-generated") == 0)
        event->kind = BONGO_CAT_DS_EVENT_RESULT;
    else if (name && strcmp(name, "task-finished") == 0)
        event->kind = BONGO_CAT_DS_EVENT_TASK_FINISHED;
    else if (name && strcmp(name, "task-failed") == 0)
        event->kind = BONGO_CAT_DS_EVENT_TASK_FAILED;
    else event->kind = BONGO_CAT_DS_EVENT_IGNORED;
    if (header) {
        copy_string(event->task_id, sizeof(event->task_id),
            yyjson_get_str(yyjson_obj_get(header, "task_id")));
        copy_string(event->error_code, sizeof(event->error_code),
            yyjson_get_str(yyjson_obj_get(header, "error_code")));
        copy_string(event->error_message, sizeof(event->error_message),
            yyjson_get_str(yyjson_obj_get(header, "error_message")));
    }
    if (event->kind == BONGO_CAT_DS_EVENT_TASK_FAILED) event->status_code = 400;
    else if (event->kind != BONGO_CAT_DS_EVENT_IGNORED) event->status_code = 200;
    yyjson_val *payload = yyjson_obj_get(root, "payload");
    yyjson_val *output = payload ? yyjson_obj_get(payload, "output") : NULL;
    yyjson_val *sentence = output
        ? yyjson_obj_get(output, "sentence") : NULL;
    if (event->kind == BONGO_CAT_DS_EVENT_RESULT && sentence) {
        copy_string(event->text, sizeof(event->text),
            yyjson_get_str(yyjson_obj_get(sentence, "text")));
        event->sentence_end = yyjson_is_true(
            yyjson_obj_get(sentence, "sentence_end"));
        event->heartbeat = yyjson_is_true(
            yyjson_obj_get(sentence, "heartbeat"));
    }
    yyjson_doc_free(doc);
    return true;
}
