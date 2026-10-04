#include "test.h"
#include "assistant_internal.h"

#include <yyjson.h>

#include <stdlib.h>
#include <string.h>

int bongo_cat_test_failures;

static yyjson_doc *parse_request(const char *request) {
    if (!request) return NULL;
    return yyjson_read(request, strlen(request), 0);
}

static void test_history_text(void) {
    AssistantHistory history;
    CHECK(bongo_cat_assistant_history_init(&history));
    CHECK(history.length == 1);
    CHECK(bongo_cat_assistant_history_append_text(&history, "user", NULL,
        "你好\n\"quoted\""));
    CHECK(bongo_cat_assistant_history_append_text(&history, "tool",
        "call-1", "ok"));
    char *request = bongo_cat_assistant_build_request("qwen3.8-flash",
        &history, "[]");
    yyjson_doc *doc = parse_request(request);
    CHECK(doc != NULL);
    if (doc) {
        yyjson_val *root = yyjson_doc_get_root(doc);
        CHECK(!strcmp(yyjson_get_str(yyjson_obj_get(root, "model")),
            "qwen3.8-flash"));
        CHECK(yyjson_is_true(yyjson_obj_get(root, "enable_search")));
        CHECK(yyjson_is_false(yyjson_obj_get(root, "stream")));
        /* A thinking pass would double the time to the first word. */
        CHECK(yyjson_is_false(yyjson_obj_get(root, "enable_thinking")));
        yyjson_val *messages = yyjson_obj_get(root, "messages");
        CHECK(yyjson_arr_size(messages) == 3);
        yyjson_val *user = yyjson_arr_get(messages, 1);
        CHECK(!strcmp(yyjson_get_str(yyjson_obj_get(user, "role")), "user"));
        CHECK(!strcmp(yyjson_get_str(yyjson_obj_get(user, "content")),
            "你好\n\"quoted\""));
        yyjson_val *tool = yyjson_arr_get(messages, 2);
        CHECK(!strcmp(yyjson_get_str(yyjson_obj_get(tool, "role")), "tool"));
        CHECK(!strcmp(yyjson_get_str(yyjson_obj_get(tool, "tool_call_id")),
            "call-1"));
        yyjson_doc_free(doc);
    }
    SDL_free(request);
    bongo_cat_assistant_history_free(&history);
}

static void test_history_assistant_tool_calls(void) {
    static const char *response =
        "{\"choices\":[{\"message\":{\"content\":\"\",\"tool_calls\":"
        "[{\"id\":\"call-9\",\"type\":\"function\",\"function\":{\"name\":"
        "\"run_command\",\"arguments\":\"{\\\"command\\\":\\\"ls\\\"}\"}}]}}]}";
    yyjson_doc *source = yyjson_read(response, strlen(response), 0);
    CHECK(source != NULL);
    if (!source) return;
    yyjson_val *message = yyjson_obj_get(
        yyjson_arr_get_first(yyjson_obj_get(yyjson_doc_get_root(source),
        "choices")), "message");
    AssistantHistory history;
    CHECK(bongo_cat_assistant_history_init(&history));
    CHECK(bongo_cat_assistant_history_append_assistant(&history,
        yyjson_obj_get(message, "content"),
        yyjson_obj_get(message, "tool_calls")));
    char *request = bongo_cat_assistant_build_request("model", &history, "[]");
    yyjson_doc *doc = parse_request(request);
    CHECK(doc != NULL);
    if (doc) {
        yyjson_val *turn = yyjson_arr_get(
            yyjson_obj_get(yyjson_doc_get_root(doc), "messages"), 1);
        yyjson_val *call = yyjson_arr_get_first(
            yyjson_obj_get(turn, "tool_calls"));
        CHECK(!strcmp(yyjson_get_str(yyjson_obj_get(turn, "role")),
            "assistant"));
        CHECK(!strcmp(yyjson_get_str(yyjson_obj_get(call, "id")), "call-9"));
        CHECK(!strcmp(yyjson_get_str(yyjson_obj_get(
            yyjson_obj_get(call, "function"), "name")), "run_command"));
        CHECK(!strcmp(yyjson_get_str(yyjson_obj_get(
            yyjson_obj_get(call, "function"), "arguments")),
            "{\"command\":\"ls\"}"));
        yyjson_doc_free(doc);
    }
    SDL_free(request);
    bongo_cat_assistant_history_free(&history);
    yyjson_doc_free(source);
}

/* The persona lives in a fixed-size buffer, so check it survives assembly. */
static void test_system_prompt_persona(void) {
    AssistantHistory history;
    CHECK(bongo_cat_assistant_history_init(&history));
    CHECK(bongo_cat_assistant_history_append_text(&history, "user", NULL,
        "你好"));
    char *request = bongo_cat_assistant_build_request("model", &history, "[]");
    yyjson_doc *doc = parse_request(request);
    CHECK(doc != NULL);
    if (doc) {
        const char *system = yyjson_get_str(yyjson_obj_get(
            yyjson_arr_get_first(yyjson_obj_get(yyjson_doc_get_root(doc),
            "messages")), "content"));
        CHECK(system && strstr(system, "星见雅") != NULL);
        CHECK(system && strstr(system, "秩序，不可破") != NULL);
        CHECK(system && strstr(system, "当前时间：") != NULL);
        CHECK(system && strstr(system, "语音识别") != NULL);
        size_t length = system ? strlen(system) : 0;
        CHECK(length && strstr(system, "分寸。") != NULL);
        yyjson_doc_free(doc);
    }
    SDL_free(request);
    bongo_cat_assistant_history_free(&history);
}

static void test_history_overflow_refused(void) {
    AssistantHistory history;
    CHECK(bongo_cat_assistant_history_init(&history));
    char *huge = SDL_malloc(ASSISTANT_HISTORY_CAP);
    CHECK(huge != NULL);
    if (huge) {
        memset(huge, 'x', ASSISTANT_HISTORY_CAP - 2);
        huge[ASSISTANT_HISTORY_CAP - 2] = '\0';
        CHECK(!bongo_cat_assistant_history_append_text(&history, "user", NULL,
            huge));
        CHECK(history.length == 1);
    }
    SDL_free(huge);
    bongo_cat_assistant_history_free(&history);
}

/* The tool list is built with yyjson, so its shape still needs a check. */
static void test_tools_json(void) {
    char *tools = bongo_cat_assistant_tools_json();
    CHECK(tools != NULL);
    yyjson_doc *doc = parse_request(tools);
    CHECK(doc != NULL);
    if (doc) {
        yyjson_val *root = yyjson_doc_get_root(doc);
        CHECK(yyjson_is_arr(root));
        CHECK(yyjson_arr_size(root) == 9);
        size_t index, count; yyjson_val *entry;
        yyjson_arr_foreach(root, index, count, entry) {
            yyjson_val *function = yyjson_obj_get(entry, "function");
            CHECK(yyjson_is_obj(function));
            CHECK(yyjson_obj_get(function, "name") != NULL);
            CHECK(yyjson_is_obj(yyjson_obj_get(function, "parameters")));
        }
        yyjson_doc_free(doc);
    }
    free(tools);
}

int main(void) {
    test_history_text();
    test_history_assistant_tool_calls();
    test_history_overflow_refused();
    test_system_prompt_persona();
    test_tools_json();
    return bongo_cat_test_failures == 0 ? 0 : 1;
}
