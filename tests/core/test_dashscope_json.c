#include "test.h"
#include "dashscope_json.h"

#include <yyjson.h>

#include <string.h>

int bongo_cat_test_failures;

static void test_task_id(void) {
    char first[BONGO_CAT_DS_TASK_ID_CAP];
    char second[BONGO_CAT_DS_TASK_ID_CAP];
    bongo_cat_dashscope_new_task_id(first, sizeof(first));
    bongo_cat_dashscope_new_task_id(second, sizeof(second));
    CHECK(strlen(first) == 36);
    CHECK(first[8] == '-' && first[13] == '-' && first[18] == '-'
        && first[23] == '-');
    CHECK(first[14] == '4');
    CHECK(first[19] == '8' || first[19] == '9' || first[19] == 'a'
        || first[19] == 'b');
    CHECK(strcmp(first, second) != 0);
    char small[36] = "preset";
    bongo_cat_dashscope_new_task_id(small, sizeof(small));
    CHECK(strcmp(small, "preset") == 0); /* Too small: untouched. */
}

static BongoCatDashscopeAsrConfig make_asr_config(
    const char *const *hints, size_t hint_count) {
    BongoCatDashscopeAsrConfig config;
    memset(&config, 0, sizeof(config));
    config.model = "paraformer-realtime-v2";
    config.format = "pcm";
    config.sample_rate = 16000;
    config.language_hints = hints;
    config.language_hint_count = hint_count;
    return config;
}

static void test_run_task(void) {
    char body[4096];
    BongoCatDashscopeAsrConfig config = make_asr_config(NULL, 0);
    CHECK(bongo_cat_dashscope_build_run_task(body, sizeof(body), "task-1",
        &config));
    yyjson_doc *doc = yyjson_read(body, strlen(body), 0);
    CHECK(doc);
    if (!doc) return;
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *header = yyjson_obj_get(root, "header");
    yyjson_val *payload = yyjson_obj_get(root, "payload");
    yyjson_val *parameters = yyjson_obj_get(payload, "parameters");
    CHECK(strcmp(yyjson_get_str(yyjson_obj_get(header, "action")),
        "run-task") == 0);
    CHECK(strcmp(yyjson_get_str(yyjson_obj_get(header, "task_id")),
        "task-1") == 0);
    CHECK(strcmp(yyjson_get_str(yyjson_obj_get(header, "streaming")),
        "duplex") == 0);
    CHECK(strcmp(yyjson_get_str(yyjson_obj_get(payload, "task_group")),
        "audio") == 0);
    CHECK(strcmp(yyjson_get_str(yyjson_obj_get(payload, "task")),
        "asr") == 0);
    CHECK(strcmp(yyjson_get_str(yyjson_obj_get(payload, "function")),
        "recognition") == 0);
    CHECK(strcmp(yyjson_get_str(yyjson_obj_get(payload, "model")),
        "paraformer-realtime-v2") == 0);
    CHECK(strcmp(yyjson_get_str(yyjson_obj_get(parameters, "format")),
        "pcm") == 0);
    CHECK(yyjson_get_sint(yyjson_obj_get(parameters, "sample_rate"))
        == 16000);
    CHECK(yyjson_get_sint(yyjson_obj_get(parameters, "max_sentence_silence"))
        == 800);
    CHECK(!yyjson_obj_get(parameters, "language_hints"));
    CHECK(!yyjson_obj_get(parameters, "disfluency_removal_enabled"));
    CHECK(!yyjson_obj_get(parameters, "semantic_punctuation_enabled"));
    CHECK(yyjson_is_obj(yyjson_obj_get(payload, "input")));
    yyjson_doc_free(doc);

    static const char *hints[] = {"zh"};
    config = make_asr_config(hints, 1);
    config.disfluency_removal_enabled = true;
    config.semantic_punctuation_enabled = true;
    CHECK(bongo_cat_dashscope_build_run_task(body, sizeof(body), "task-2",
        &config));
    doc = yyjson_read(body, strlen(body), 0);
    root = yyjson_doc_get_root(doc);
    parameters = yyjson_obj_get(yyjson_obj_get(root, "payload"),
        "parameters");
    yyjson_val *hint_array = yyjson_obj_get(parameters, "language_hints");
    CHECK(yyjson_is_arr(hint_array) && yyjson_arr_size(hint_array) == 1);
    CHECK(strcmp(yyjson_get_str(yyjson_arr_get(hint_array, 0)),
        "zh") == 0);
    CHECK(yyjson_is_true(yyjson_obj_get(parameters,
        "disfluency_removal_enabled")));
    CHECK(yyjson_is_true(yyjson_obj_get(parameters,
        "semantic_punctuation_enabled")));
    yyjson_doc_free(doc);
}

static void test_builder_rejects(void) {
    char body[4096] = "keep";
    BongoCatDashscopeAsrConfig config = make_asr_config(NULL, 0);
    CHECK(!bongo_cat_dashscope_build_run_task(body, sizeof(body), NULL,
        &config));
    CHECK(!bongo_cat_dashscope_build_run_task(body, sizeof(body), "",
        &config));
    config.model = NULL;
    CHECK(!bongo_cat_dashscope_build_run_task(body, sizeof(body), "task-1",
        &config));
    config = make_asr_config(NULL, 0);
    CHECK(!bongo_cat_dashscope_build_run_task(body, 16, "task-1", &config));
    CHECK(strcmp(body, "keep") == 0); /* Failures leave `out` untouched. */
    CHECK(!bongo_cat_dashscope_build_finish_task(body, 16, "task-1"));
    CHECK(strcmp(body, "keep") == 0);
    char huge[BONGO_CAT_DS_TASK_ID_CAP + 8];
    memset(huge, 'x', sizeof(huge) - 1);
    huge[sizeof(huge) - 1] = '\0';
    CHECK(!bongo_cat_dashscope_build_run_task(body, sizeof(body), huge,
        &config));
}

static void test_finish_task(void) {
    char body[4096];
    CHECK(bongo_cat_dashscope_build_finish_task(body, sizeof(body),
        "task-9"));
    yyjson_doc *doc = yyjson_read(body, strlen(body), 0);
    CHECK(doc);
    if (!doc) return;
    yyjson_val *header = yyjson_obj_get(yyjson_doc_get_root(doc), "header");
    yyjson_val *payload = yyjson_obj_get(yyjson_doc_get_root(doc), "payload");
    CHECK(strcmp(yyjson_get_str(yyjson_obj_get(header, "action")),
        "finish-task") == 0);
    CHECK(strcmp(yyjson_get_str(yyjson_obj_get(header, "task_id")),
        "task-9") == 0);
    CHECK(yyjson_is_obj(yyjson_obj_get(payload, "input")));
    yyjson_doc_free(doc);
}

static void test_parse_task_started(void) {
    BongoCatDashscopeEvent event;
    const char *json = "{\"header\":{\"task_id\":\"t-1\","
        "\"event\":\"task-started\",\"attributes\":{}},\"payload\":{}}";
    CHECK(bongo_cat_dashscope_parse_event(json, 0, &event));
    CHECK(event.kind == BONGO_CAT_DS_EVENT_TASK_STARTED);
    CHECK(strcmp(event.task_id, "t-1") == 0);
    CHECK(event.status_code == 200);
}

static void test_parse_result(void) {
    BongoCatDashscopeEvent event;
    const char *partial = "{\"header\":{\"task_id\":\"t-1\","
        "\"event\":\"result-generated\"},\"payload\":{\"output\":"
        "{\"sentence\":{\"begin_time\":170,\"text\":\"你好\","
        "\"heartbeat\":false,\"sentence_end\":false,"
        "\"words\":[]}},\"usage\":null}}";
    CHECK(bongo_cat_dashscope_parse_event(partial, 0, &event));
    CHECK(event.kind == BONGO_CAT_DS_EVENT_RESULT);
    CHECK(strcmp(event.text, "你好") == 0);
    CHECK(!event.sentence_end);
    CHECK(!event.heartbeat);
    const char *final = "{\"header\":{\"event\":\"result-generated\"},"
        "\"payload\":{\"output\":{\"sentence\":{\"text\":\"我知道。\","
        "\"sentence_end\":true}},\"usage\":{\"duration\":3}}}";
    CHECK(bongo_cat_dashscope_parse_event(final, 0, &event));
    CHECK(event.kind == BONGO_CAT_DS_EVENT_RESULT);
    CHECK(event.sentence_end);
    CHECK(strcmp(event.text, "我知道。") == 0);
    const char *heartbeat = "{\"header\":{\"event\":\"result-generated\"},"
        "\"payload\":{\"output\":{\"sentence\":{\"text\":\"\",\"heartbeat\":"
        "true,\"sentence_end\":false}}}}";
    CHECK(bongo_cat_dashscope_parse_event(heartbeat, 0, &event));
    CHECK(event.heartbeat);
    CHECK(!event.sentence_end);
}

static void test_parse_lifecycle(void) {
    BongoCatDashscopeEvent event;
    CHECK(bongo_cat_dashscope_parse_event("{\"header\":{\"event\":"
        "\"task-finished\"},\"payload\":{\"output\":{},\"usage\":null}}",
        0, &event));
    CHECK(event.kind == BONGO_CAT_DS_EVENT_TASK_FINISHED);
    CHECK(bongo_cat_dashscope_parse_event("{\"header\":{\"task_id\":\"t\","
        "\"event\":\"task-failed\",\"error_code\":\"CLIENT_ERROR\","
        "\"error_message\":\"request timeout after 23 seconds.\"},"
        "\"payload\":{}}", 0, &event));
    CHECK(event.kind == BONGO_CAT_DS_EVENT_TASK_FAILED);
    CHECK(strcmp(event.error_code, "CLIENT_ERROR") == 0);
    CHECK(strcmp(event.error_message, "request timeout after 23 seconds.")
        == 0);
    CHECK(bongo_cat_dashscope_parse_event("{\"header\":{\"event\":"
        "\"unknown-event\"},\"payload\":{}}", 0, &event));
    CHECK(event.kind == BONGO_CAT_DS_EVENT_IGNORED);
    CHECK(bongo_cat_dashscope_parse_event("{}", 0, &event));
    CHECK(event.kind == BONGO_CAT_DS_EVENT_IGNORED);
    CHECK(bongo_cat_dashscope_parse_event("{\"header\":{\"event\":"
        "\"result-generated\"}}", 0, &event)); /* No payload. */
    CHECK(event.kind == BONGO_CAT_DS_EVENT_RESULT);
    CHECK(event.text[0] == '\0');
    CHECK(!bongo_cat_dashscope_parse_event("{oops", 0, &event));
    CHECK(!bongo_cat_dashscope_parse_event(NULL, 0, &event));
}

static void test_tts_builders(void) {
    char body[4096];
    BongoCatDashscopeTtsConfig config = {"qwen-audio-3.1-tts-flash",
        "yunhuanhuan_v3.1", "pcm", 24000};
    CHECK(bongo_cat_dashscope_build_tts_run_task(body, sizeof(body),
        "task-9", &config));
    yyjson_doc *doc = yyjson_read(body, strlen(body), 0);
    CHECK(doc);
    if (!doc) return;
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *header = yyjson_obj_get(root, "header");
    yyjson_val *payload = yyjson_obj_get(root, "payload");
    yyjson_val *parameters = yyjson_obj_get(payload, "parameters");
    CHECK(strcmp(yyjson_get_str(yyjson_obj_get(header, "action")),
        "run-task") == 0);
    CHECK(strcmp(yyjson_get_str(yyjson_obj_get(header, "task_id")),
        "task-9") == 0);
    CHECK(strcmp(yyjson_get_str(yyjson_obj_get(payload, "task_group")),
        "audio") == 0);
    CHECK(strcmp(yyjson_get_str(yyjson_obj_get(payload, "task")),
        "tts") == 0);
    CHECK(strcmp(yyjson_get_str(yyjson_obj_get(payload, "function")),
        "SpeechSynthesizer") == 0);
    CHECK(strcmp(yyjson_get_str(yyjson_obj_get(payload, "model")),
        "qwen-audio-3.1-tts-flash") == 0);
    CHECK(strcmp(yyjson_get_str(yyjson_obj_get(parameters, "voice")),
        "yunhuanhuan_v3.1") == 0);
    CHECK(strcmp(yyjson_get_str(yyjson_obj_get(parameters, "format")),
        "pcm") == 0);
    CHECK(yyjson_get_sint(yyjson_obj_get(parameters, "sample_rate"))
        == 24000);
    CHECK(strcmp(yyjson_get_str(yyjson_obj_get(parameters, "text_type")),
        "PlainText") == 0);
    yyjson_doc_free(doc);

    CHECK(bongo_cat_dashscope_build_tts_continue_task(body, sizeof(body),
        "task-9", "\xe4\xbd\xa0\xe5\xa5\xbd"));
    doc = yyjson_read(body, strlen(body), 0);
    CHECK(doc);
    if (!doc) return;
    root = yyjson_doc_get_root(doc);
    header = yyjson_obj_get(root, "header");
    payload = yyjson_obj_get(root, "payload");
    CHECK(strcmp(yyjson_get_str(yyjson_obj_get(header, "action")),
        "continue-task") == 0);
    CHECK(strcmp(yyjson_get_str(yyjson_obj_get(
        yyjson_obj_get(payload, "input"), "text")), "\xe4\xbd\xa0\xe5\xa5\xbd")
        == 0);
    yyjson_doc_free(doc);

    /* Rejections: empty text, missing fields, oversized buffers. */
    CHECK(!bongo_cat_dashscope_build_tts_continue_task(body, sizeof(body),
        "task-9", ""));
    CHECK(!bongo_cat_dashscope_build_tts_continue_task(body, sizeof(body),
        "task-9", NULL));
    BongoCatDashscopeTtsConfig broken = {NULL, "v", "pcm", 24000};
    CHECK(!bongo_cat_dashscope_build_tts_run_task(body, sizeof(body),
        "task-9", &broken));
    broken = (BongoCatDashscopeTtsConfig){"m", "v", "pcm", 0};
    CHECK(!bongo_cat_dashscope_build_tts_run_task(body, sizeof(body),
        "task-9", &broken));
    char small[16];
    CHECK(!bongo_cat_dashscope_build_tts_run_task(small, sizeof(small),
        "task-9", &config));
}

int main(void) {
    test_task_id();
    test_run_task();
    test_builder_rejects();
    test_finish_task();
    test_tts_builders();
    test_parse_task_started();
    test_parse_result();
    test_parse_lifecycle();
    return bongo_cat_test_failures ? 1 : 0;
}
