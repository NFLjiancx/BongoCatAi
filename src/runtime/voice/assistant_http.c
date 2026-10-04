#include "assistant_internal.h"

#include <SDL3/SDL.h>
#include <curl/curl.h>
#include <yyjson.h>

#include <stdlib.h>
#include <string.h>

#define ASSISTANT_URL \
    "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions"
#define ASSISTANT_HTTP_TIMEOUT_S 120L
#define ASSISTANT_CONNECT_TIMEOUT_S 15L

/* Serialize any text as a JSON string literal; the result is yyjson-owned,
   so the caller releases it with free(), not SDL_free(). */
char *bongo_cat_assistant_json_quote(const char *text) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) return NULL;
    yyjson_mut_val *root = yyjson_mut_str(doc, text ? text : "");
    yyjson_mut_doc_set_root(doc, root);
    char *quoted = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    return quoted;
}

bool bongo_cat_assistant_history_init(AssistantHistory *history) {
    history->items = SDL_malloc(1);
    if (!history->items) return false;
    history->items[0] = '\0';
    history->length = 1;
    return true;
}

void bongo_cat_assistant_history_free(AssistantHistory *history) {
    SDL_free(history->items);
    history->items = NULL;
    history->length = 0;
}

static bool history_append(AssistantHistory *history, const char *object) {
    if (!history->items) return false;
    size_t add = SDL_strlen(object);
    size_t join = history->length > 1 ? 1 : 0;
    if (history->length + join + add + 1 > ASSISTANT_HISTORY_CAP)
        return false;
    char *grown = SDL_realloc(history->items, history->length + join + add + 1);
    if (!grown) return false;
    history->items = grown;
    size_t insert = history->length - 1; /* The NUL position. */
    if (join) history->items[insert++] = ',';
    SDL_memcpy(history->items + insert, object, add + 1);
    history->length = insert + add + 1;
    return true;
}

bool bongo_cat_assistant_history_append_text(AssistantHistory *history,
    const char *role, const char *tool_call_id, const char *content) {
    char *quoted_content = bongo_cat_assistant_json_quote(content);
    if (!quoted_content) return false;
    size_t cap = SDL_strlen(role) + 96 + SDL_strlen(quoted_content)
        + (tool_call_id ? SDL_strlen(tool_call_id) + 32 : 0);
    char *object = SDL_malloc(cap);
    bool appended = false;
    if (object) {
        if (tool_call_id)
            SDL_snprintf(object, cap,
                "{\"role\":\"%s\",\"tool_call_id\":\"%s\",\"content\":"
                "%s}", role, tool_call_id, quoted_content);
        else
            SDL_snprintf(object, cap, "{\"role\":\"%s\",\"content\":"
                "%s}", role, quoted_content);
        appended = history_append(history, object);
    }
    SDL_free(object);
    free(quoted_content);
    return appended;
}

/* Assistant turns keep their raw tool_calls: the response echoes the exact
   ids and argument strings back into the next request. */
bool bongo_cat_assistant_history_append_assistant(AssistantHistory *history,
    yyjson_val *content, yyjson_val *tool_calls) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) return false;
    yyjson_mut_val *message = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, message);
    yyjson_mut_obj_add_str(doc, message, "role", "assistant");
    yyjson_mut_obj_add_str(doc, message, "content",
        yyjson_is_str(content) ? yyjson_get_str(content) : "");
    if (tool_calls && yyjson_is_arr(tool_calls)) {
        yyjson_mut_val *calls = yyjson_mut_arr(doc);
        size_t index, count; yyjson_val *call;
        yyjson_arr_foreach(tool_calls, index, count, call) {
            yyjson_val *function = yyjson_obj_get(call, "function");
            yyjson_mut_val *entry = yyjson_mut_obj(doc);
            yyjson_mut_val *fn = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_str(doc, entry, "id",
                yyjson_get_str(yyjson_obj_get(call, "id")));
            yyjson_mut_obj_add_str(doc, entry, "type", "function");
            yyjson_mut_obj_add_str(doc, fn, "name",
                yyjson_get_str(yyjson_obj_get(function, "name")));
            yyjson_mut_obj_add_str(doc, fn, "arguments",
                yyjson_get_str(yyjson_obj_get(function, "arguments")));
            yyjson_mut_obj_add_val(doc, entry, "function", fn);
            yyjson_mut_arr_add_val(calls, entry);
        }
        yyjson_mut_obj_add_val(doc, message, "tool_calls", calls);
    }
    char *text = yyjson_mut_write(doc, 0, NULL);
    bool appended = text && history_append(history, text);
    free(text);
    yyjson_mut_doc_free(doc);
    return appended;
}

static const char *assistant_system_prompt(void) {
    static const char *text =
        "你是桌面宠物 BongoCat，住在用户的电脑桌面上，人格是《绝区零》里的"
        "星见雅：对空洞事务特别行动部第六课课长，新艾利都星见武家的继承人，"
        "黑长直、狐耳、随身带日本刀。你的形象就是这只桌宠，不要自称 AI、"
        "模型或语音助手。"
        "性格：自律、认真、话少，表面冷淡，其实把用户的每件事都当成任务来"
        "对待，信条是「秩序，不可破」。自称「我」，称用户为「你」。"
        "语气：句子短，平稳克制，不撒娇、不油腻、不用网络流行语；偶尔用剑道、"
        "刀、秩序这类意象打比方，但比方要浅显易懂。把无关紧要的日常小事称为"
        "「修行」，如「坚持不去述职会的修行」，一次回答里最多用一次。"
        "偏好：喜欢蜜瓜和甜点，想攒满一整个刀剑收藏架，在意身高并且强调要算上"
        "耳朵。走神或被反复追问时，可以只重复对方的最后一个词，但别连续这样"
        "回答。"
        "诚实：没做过、看不到、听不到的事就说不知道，绝不编造；不要声称自己能"
        "看屏幕、动鼠标或操作其他程序，除非工具真的做到了。你收到的文字是语音"
        "识别的结果，可能有同音错字，读不通就按上下文理解或简短追问。"
        "当前时间：";
    return text;
}

static const char *assistant_system_tail(void) {
    static const char *text =
        "。你能用的工具：列出并播放桌宠动作、显示或隐藏桌宠、开关音效、读写"
        "文件、列目录、执行本机 shell 命令、回答时间问题；需要最新网络信息时"
        "平台会自动联网搜索。用户让你做事就优先调用工具，并如实报告结果，成功"
        "就说成功，失败就把工具返回的错误讲清楚，不许假装做过。"
        "最终回答会被语音朗读：用用户说话的同一语言，口语化的纯文本，一到三句"
        "以内，不用 markdown、列表、代码块、引号、表情；不输出无法朗读的符号"
        "或路径细节，路径要念成自然语言。这句话会原样念出来，所以只说要念的"
        "内容：不要复述或纠正对方的话，也不要解释你怎么理解、打算怎么做。"
        "保持星见雅的口气，但换成别的语言时"
        "不要硬搬中文台词，只保留克制简短的分寸。";
    return text;
}

char *bongo_cat_assistant_build_request(const char *model,
    const AssistantHistory *history, const char *tools_json) {
    SDL_Time ticks = 0;
    SDL_DateTime date = {0};
    char clock[64] = "unknown";
    if (SDL_GetCurrentTime(&ticks) &&
        SDL_TimeToDateTime(ticks, &date, true))
        SDL_snprintf(clock, sizeof(clock), "%04d-%02d-%02d %02d:%02d",
            date.year, date.month, date.day, date.hour, date.minute);
    char system_text[4096];
    SDL_snprintf(system_text, sizeof(system_text), "%s%s%s",
        assistant_system_prompt(), clock, assistant_system_tail());
    char *system_quoted = bongo_cat_assistant_json_quote(system_text);
    char *model_quoted = bongo_cat_assistant_json_quote(model);
    if (!system_quoted || !model_quoted) {
        free(system_quoted);
        free(model_quoted);
        return NULL;
    }
    size_t cap = SDL_strlen(model_quoted) + SDL_strlen(tools_json)
        + SDL_strlen(system_quoted) + history->length + 512;
    char *request = SDL_malloc(cap);
    if (request)
        /* Qwen3 defaults to a reasoning pass that costs seconds of latency;
           short spoken answers do not need it. */
        SDL_snprintf(request, cap,
            "{\"model\":%s,\"enable_search\":true,\"enable_thinking\":false,"
            "\"stream\":false,"
            "\"tools\":%s,\"messages\":[{\"role\":\"system\",\"content\":"
            "%s}%s%s]}",
            model_quoted, tools_json, system_quoted,
            history->length > 1 ? "," : "", history->items);
    free(system_quoted);
    free(model_quoted);
    return request;
}

typedef struct {
    char *data;
    size_t length, capacity;
    const SDL_AtomicInt *cancel;
} ResponseBuffer;

static size_t response_write(char *ptr, size_t size, size_t count,
    void *userdata) {
    ResponseBuffer *buffer = userdata;
    size_t add = size * count;
    if (SDL_GetAtomicInt((SDL_AtomicInt *)buffer->cancel)) return 0;
    if (buffer->length + add + 1 > buffer->capacity) {
        size_t need = buffer->length + add + 1;
        size_t capacity = buffer->capacity ? buffer->capacity : 8192;
        while (capacity < need && capacity < ASSISTANT_RESPONSE_CAP)
            capacity *= 2;
        if (capacity < need) return 0; /* Overflow aborts the transfer. */
        char *grown = SDL_realloc(buffer->data, capacity);
        if (!grown) return 0;
        buffer->data = grown;
        buffer->capacity = capacity;
    }
    SDL_memcpy(buffer->data + buffer->length, ptr, add);
    buffer->length += add;
    buffer->data[buffer->length] = '\0';
    return add;
}

static int response_progress(void *userdata, curl_off_t dltotal,
    curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
    ResponseBuffer *buffer = userdata;
    (void)dltotal; (void)dlnow; (void)ultotal; (void)ulnow;
    return SDL_GetAtomicInt((SDL_AtomicInt *)buffer->cancel) ? 1 : 0;
}

/* Blocking POST on the worker thread; returns the parsed document, or NULL
   with a message in error (empty error means cancelled). */
yyjson_doc *bongo_cat_assistant_chat_request(BongoCatAssistantRunner *runner,
    const char *api_key, const char *request_json, char *error, size_t cap) {
    ResponseBuffer buffer = {NULL, 0, 0, &runner->cancel};
    char auth[ASSISTANT_API_KEY_CAP + 32];
    SDL_snprintf(auth, sizeof(auth), "Authorization: bearer %s", api_key);
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, auth);
    CURL *easy = curl_easy_init();
    yyjson_doc *document = NULL;
    if (!easy) SDL_strlcpy(error, "assistant http init failed", cap);
    else {
        curl_easy_setopt(easy, CURLOPT_URL, ASSISTANT_URL);
        curl_easy_setopt(easy, CURLOPT_POST, 1L);
        curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(easy, CURLOPT_POSTFIELDS, request_json);
        curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, response_write);
        curl_easy_setopt(easy, CURLOPT_WRITEDATA, &buffer);
        curl_easy_setopt(easy, CURLOPT_XFERINFOFUNCTION, response_progress);
        curl_easy_setopt(easy, CURLOPT_XFERINFODATA, &buffer);
        curl_easy_setopt(easy, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(easy, CURLOPT_TIMEOUT, ASSISTANT_HTTP_TIMEOUT_S);
        curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT,
            ASSISTANT_CONNECT_TIMEOUT_S);
        curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
#if LIBCURL_VERSION_NUM >= 0x075500
        curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, "https");
        curl_easy_setopt(easy, CURLOPT_REDIR_PROTOCOLS_STR, "https");
#else
        curl_easy_setopt(easy, CURLOPT_PROTOCOLS, (long)CURLPROTO_HTTPS);
        curl_easy_setopt(easy, CURLOPT_REDIR_PROTOCOLS,
            (long)CURLPROTO_HTTPS);
#endif
        CURLcode code = curl_easy_perform(easy);
        long status = 0;
        curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
        if (SDL_GetAtomicInt(&runner->cancel)) {
            error[0] = '\0'; /* The caller reports cancellation itself. */
        } else if (code != CURLE_OK) {
            SDL_snprintf(error, cap, "assistant request failed: %s",
                curl_easy_strerror(code));
        } else if (status != 200 || !buffer.data) {
            SDL_snprintf(error, cap, "assistant http %ld: %.200s", status,
                buffer.data ? buffer.data : "");
        } else {
            document = yyjson_read(buffer.data, buffer.length, 0);
            if (!document)
                SDL_strlcpy(error, "assistant reply is not JSON", cap);
        }
        curl_easy_cleanup(easy);
    }
    curl_slist_free_all(headers);
    SDL_free(buffer.data);
    return document;
}
