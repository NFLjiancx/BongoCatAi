#include "assistant_internal.h"
#include "voice_internal.h"
#include "runtime.h"
#include "bongo_cat/audio.h"
#include "bongo_cat/log.h"
#include "bongo_cat/model.h"

#include <SDL3/SDL.h>
#include <yyjson.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void bongo_cat_assistant_progress(BongoCatAssistantRunner *runner,
    const char *line) {
    BongoCatVoice *voice = runner->voice;
    size_t cap = BONGO_CAT_VOICE_TEXT_CAP - 1;
    SDL_LockMutex(voice->mutex);
    size_t length = SDL_strlen(voice->result_text);
    size_t need = SDL_strlen(line);
    if (need > cap) { line += need - cap; need = cap; }
    if (length + 1 + need > cap) {
        size_t keep = cap - need - 1;
        if (length > keep)
            SDL_memmove(voice->result_text,
                voice->result_text + (length - keep), keep);
        else keep = length;
        voice->result_text[keep] = '\0';
        length = keep;
    }
    if (length) voice->result_text[length++] = '\n';
    SDL_memcpy(voice->result_text + length, line, need);
    voice->result_text[length + need] = '\0';
    SDL_UnlockMutex(voice->mutex);
    bongo_cat_voice_post_turn(voice, BONGO_CAT_VOICE_EV_ASSISTANT_PROGRESS,
        runner->epoch);
}

void bongo_cat_assistant_publish(BongoCatAssistantRunner *runner,
    const char *text, bool is_error) {
    BongoCatVoice *voice = runner->voice;
    SDL_LockMutex(voice->mutex);
    snprintf(voice->result_text, sizeof(voice->result_text), "%s", text);
    voice->partial_text[0] = '\0';
    voice->final_text[0] = '\0';
    SDL_UnlockMutex(voice->mutex);
    bongo_cat_voice_post_turn(voice, is_error
        ? BONGO_CAT_VOICE_EV_ASSISTANT_ERROR
        : BONGO_CAT_VOICE_EV_ASSISTANT_RESULT, runner->epoch);
}

void bongo_cat_assistant_pump(BongoCatAssistantRunner *runner) {
    if (!runner) return;
    SDL_LockMutex(runner->action_mutex);
    if (!runner->action.pending || runner->action.done) {
        SDL_UnlockMutex(runner->action_mutex);
        return;
    }
    struct AssistantAction request = runner->action;
    SDL_UnlockMutex(runner->action_mutex);

    BongoCatApp *app = runner->voice->app;
    char result[ASSISTANT_TOOL_RESULT_CAP] = "";
    switch (request.kind) {
        case ACTION_LIST_BEHAVIORS: {
            size_t used = 0;
            for (size_t i = 0; i < app->behaviors.count
                && used < sizeof(result) - 96; ++i) {
                BongoCatBehaviorEntry *entry = &app->behaviors.entries[i];
                used += (size_t)SDL_snprintf(result + used,
                    sizeof(result) - used, "%s\t%s\n", entry->id,
                    entry->label);
            }
            if (!used) SDL_strlcpy(result, "(no behaviors)",
                sizeof(result));
            break;
        }
        case ACTION_PLAY_BEHAVIOR: {
            BongoCatBehaviorEntry *matched = NULL;
            for (size_t i = 0; i < app->behaviors.count; ++i) {
                BongoCatBehaviorEntry *entry = &app->behaviors.entries[i];
                if (!SDL_strcasecmp(entry->label, request.arg)
                    || !SDL_strcasecmp(entry->id, request.arg)) {
                    matched = entry;
                    break;
                }
            }
            if (!matched) {
                for (size_t i = 0; i < app->behaviors.count; ++i) {
                    BongoCatBehaviorEntry *entry =
                        &app->behaviors.entries[i];
                    char *hit = SDL_strcasestr(entry->label, request.arg);
                    if (!hit) hit = SDL_strcasestr(entry->id, request.arg);
                    if (hit) { matched = entry; break; }
                }
            }
            if (!matched)
                SDL_snprintf(result, sizeof(result),
                    "error: behavior '%s' not found", request.arg);
            else if (bongo_cat_app_run_behavior(app, matched))
                SDL_snprintf(result, sizeof(result), "playing %s (%s)",
                    matched->id, matched->label);
            else
                SDL_snprintf(result, sizeof(result), "error: cannot play %s",
                    matched->id);
            break;
        }
        case ACTION_SET_VISIBILITY:
            bongo_cat_window_set_visible(app, request.flag);
            SDL_strlcpy(result, request.flag ? "pet shown" : "pet hidden",
                sizeof(result));
            break;
        case ACTION_SET_SOUND:
            if (app->audio) bongo_cat_audio_set_enabled(app->audio,
                request.flag);
            SDL_strlcpy(result, request.flag ? "sound on" : "sound off",
                sizeof(result));
            break;
    }

    SDL_LockMutex(runner->action_mutex);
    SDL_strlcpy(runner->action.result, result,
        sizeof(runner->action.result));
    runner->action.done = true;
    SDL_BroadcastCondition(runner->action_cond);
    SDL_UnlockMutex(runner->action_mutex);
}

static void fail_worker(BongoCatAssistantRunner *runner,
    const char *message, AssistantHistory *history, char *api_key) {
    bongo_cat_assistant_publish(runner, message, true);
    bongo_cat_voice_report_error(runner->voice, message);
    bongo_cat_assistant_history_free(history);
    SDL_memset(api_key, 0, ASSISTANT_API_KEY_CAP);
    SDL_SetAtomicInt(&runner->running, 0);
}

/* One chat-completions round: ask, run any requested tools, ask again.
   Stops at the first plain-text answer, an error, or the round limit. */
static int assistant_worker(void *userdata) {
    BongoCatAssistantRunner *runner = userdata;
    BongoCatVoice *voice = runner->voice;
    char api_key[ASSISTANT_API_KEY_CAP];
    api_key[0] = '\0';
    AssistantHistory history = {NULL, 0};
    char final_text[BONGO_CAT_VOICE_TEXT_CAP] = "";
    bool succeeded = false;
    if (!bongo_cat_voice_resolve_api_key(voice, api_key, sizeof(api_key))) {
        fail_worker(runner, "DashScope API key is not set (AI page)",
            &history, api_key);
        SDL_free(runner->prompt);
        runner->prompt = NULL;
        return 0;
    }
    if (!bongo_cat_assistant_history_init(&history)
        || !bongo_cat_assistant_history_append_text(&history, "user", NULL,
            runner->prompt)) {
        fail_worker(runner, "assistant out of memory", &history, api_key);
        SDL_free(runner->prompt);
        runner->prompt = NULL;
        return 0;
    }
    const char *model = runner->model[0]
        ? runner->model : BONGO_CAT_AI_DEFAULT_ASSISTANT_MODEL;
    char *tools_json = bongo_cat_assistant_tools_json();
    bool cancelled = false;
    if (!tools_json) {
        fail_worker(runner, "assistant out of memory", &history, api_key);
        SDL_free(runner->prompt);
        runner->prompt = NULL;
        return 0;
    }
    for (int round = 0; round < ASSISTANT_MAX_ROUNDS; ++round) {
        if (SDL_GetAtomicInt(&runner->cancel)) { cancelled = true; break; }
        char *request = bongo_cat_assistant_build_request(model, &history,
            tools_json);
        if (!request) {
            SDL_strlcpy(final_text, "assistant out of memory",
                sizeof(final_text));
            break;
        }
        char error[256] = "";
        yyjson_doc *response = bongo_cat_assistant_chat_request(runner,
            api_key, request, error, sizeof(error));
        SDL_free(request);
        if (!response) {
            if (SDL_GetAtomicInt(&runner->cancel)) { cancelled = true; break; }
            SDL_strlcpy(final_text, error, sizeof(final_text));
            break;
        }
        yyjson_val *root = yyjson_doc_get_root(response);
        yyjson_val *error_object = yyjson_obj_get(root, "error");
        yyjson_val *choice = NULL, *message = NULL;
        if (error_object) {
            const char *msg = yyjson_get_str(
                yyjson_obj_get(error_object, "message"));
            SDL_snprintf(final_text, sizeof(final_text),
                "api error: %s", msg ? msg : "unknown");
        } else if (!(choice = yyjson_arr_get_first(
                yyjson_obj_get(root, "choices")))
            || !(message = yyjson_obj_get(choice, "message"))) {
            SDL_strlcpy(final_text, "assistant returned no message",
                sizeof(final_text));
        } else {
            yyjson_val *content = yyjson_obj_get(message, "content");
            yyjson_val *tool_calls = yyjson_obj_get(message, "tool_calls");
            bool has_tools = tool_calls && yyjson_is_arr(tool_calls)
                && yyjson_arr_size(tool_calls) > 0;
            if (has_tools) {
                bongo_cat_assistant_history_append_assistant(&history,
                    content, tool_calls);
                size_t index, count; yyjson_val *call;
                bool aborted = false;
                yyjson_arr_foreach(tool_calls, index, count, call) {
                    yyjson_val *function = yyjson_obj_get(call, "function");
                    const char *id = yyjson_get_str(
                        yyjson_obj_get(call, "id"));
                    const char *name = yyjson_get_str(
                        yyjson_obj_get(function, "name"));
                    const char *arguments = yyjson_get_str(
                        yyjson_obj_get(function, "arguments"));
                    if (!id || !name) continue;
                    char line[640];
                    SDL_snprintf(line, sizeof(line), "▶ %s %s", name,
                        arguments ? arguments : "");
                    bongo_cat_assistant_progress(runner, line);
                    char result[ASSISTANT_TOOL_RESULT_CAP];
                    bongo_cat_assistant_tool_execute(runner, name,
                        arguments, result, sizeof(result));
                    if (SDL_GetAtomicInt(&runner->cancel)) {
                        aborted = true;
                        break;
                    }
                    if (!bongo_cat_assistant_history_append_text(&history,
                        "tool", id, result)) { aborted = true; break; }
                }
                yyjson_doc_free(response);
                if (aborted) { cancelled = true; break; }
                continue;
            }
            if (yyjson_is_str(content) && yyjson_get_str(content)[0]) {
                SDL_strlcpy(final_text, yyjson_get_str(content),
                    sizeof(final_text));
                succeeded = true;
            } else {
                SDL_strlcpy(final_text, "assistant returned an empty reply",
                    sizeof(final_text));
                }
        }
        yyjson_doc_free(response);
        break;
    }
    free(tools_json);
    bongo_cat_assistant_history_free(&history);
    SDL_memset(api_key, 0, sizeof(api_key));
    bool was_cancelled = cancelled || SDL_GetAtomicInt(&runner->cancel);
    SDL_SetAtomicInt(&runner->cancel, 0);
    if (!was_cancelled && final_text[0])
        bongo_cat_assistant_publish(runner, final_text, !succeeded);
    SDL_SetAtomicInt(&runner->running, 0);
    /* Keep runner->thread intact: only assistant_stop may join it; clearing
       it here would strand the SDL thread object unjoined. */
    SDL_free(runner->prompt);
    runner->prompt = NULL;
    return 0;
}

BongoCatAssistantRunner *bongo_cat_assistant_create(BongoCatVoice *voice,
    const char *model) {
    if (!voice) return NULL;
    BongoCatAssistantRunner *runner = SDL_calloc(1, sizeof(*runner));
    if (!runner) return NULL;
    runner->voice = voice;
    SDL_strlcpy(runner->model, model ? model : "", ASSISTANT_MODEL_CAP);
    runner->action_mutex = SDL_CreateMutex();
    runner->action_cond = SDL_CreateCondition();
    if (!runner->action_mutex || !runner->action_cond) {
        if (runner->action_mutex) SDL_DestroyMutex(runner->action_mutex);
        if (runner->action_cond) SDL_DestroyCondition(runner->action_cond);
        SDL_free(runner);
        return NULL;
    }
    return runner;
}

bool bongo_cat_assistant_running(const BongoCatAssistantRunner *runner) {
    return runner && SDL_GetAtomicInt(
        (SDL_AtomicInt *)&runner->running) != 0;
}

void bongo_cat_assistant_cancel(BongoCatAssistantRunner *runner) {
    if (!runner) return;
    SDL_SetAtomicInt(&runner->cancel, 1);
    SDL_LockMutex(runner->action_mutex);
    SDL_BroadcastCondition(runner->action_cond);
    SDL_UnlockMutex(runner->action_mutex);
}

bool bongo_cat_assistant_start(BongoCatAssistantRunner *runner,
    const char *prompt, unsigned epoch) {
    if (!runner || !prompt || !prompt[0]) return false;
    bongo_cat_assistant_stop(runner); /* Reap any finished previous worker. */
    if (runner->prompt || SDL_GetAtomicInt(&runner->running)) return false;
    runner->prompt = SDL_strdup(prompt);
    if (!runner->prompt) return false;
    runner->epoch = epoch;
    SDL_SetAtomicInt(&runner->cancel, 0);
    SDL_SetAtomicInt(&runner->running, 1);
    runner->thread = SDL_CreateThread(assistant_worker, "bongo_assistant",
        runner);
    if (!runner->thread) {
        SDL_free(runner->prompt);
        runner->prompt = NULL;
        SDL_SetAtomicInt(&runner->running, 0);
        return false;
    }
    return true;
}

void bongo_cat_assistant_stop(BongoCatAssistantRunner *runner) {
    if (!runner) return;
    bongo_cat_assistant_cancel(runner);
    if (runner->thread) {
        SDL_WaitThread(runner->thread, NULL);
        runner->thread = NULL;
    }
    SDL_SetAtomicInt(&runner->cancel, 0);
    SDL_LockMutex(runner->action_mutex);
    runner->action.pending = false;
    runner->action.done = true;
    SDL_UnlockMutex(runner->action_mutex);
}

void bongo_cat_assistant_destroy(BongoCatAssistantRunner *runner) {
    if (!runner) return;
    bongo_cat_assistant_stop(runner);
    SDL_DestroyMutex(runner->action_mutex);
    SDL_DestroyCondition(runner->action_cond);
    SDL_free(runner);
}
