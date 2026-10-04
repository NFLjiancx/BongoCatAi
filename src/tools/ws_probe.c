#include "ws_client.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile int received;
static volatile int got_started;
static volatile int got_finished;

static void on_text(void *userdata, const char *text, size_t length) {
    (void)userdata;
    char preview[512];
    size_t copy = length < sizeof(preview) - 1 ? length : sizeof(preview) - 1;
    memcpy(preview, text, copy);
    preview[copy] = '\0';
    printf("TEXT(%zu): %s\n", length, preview);
    if (strstr(text, "\"task-started\"")) got_started = 1;
    if (strstr(text, "\"task-finished\"")) got_finished = 1;
    if (strstr(text, "\"task-failed\"")) got_finished = 1;
    received = 1;
}

static void on_binary(void *userdata, const void *data, size_t length) {
    (void)userdata; (void)data;
    printf("BINARY(%zu)\n", length);
    received = 1;
}

static void on_state(void *userdata, bool connected, const char *error) {
    (void)userdata;
    if (connected) printf("STATE: connected\n");
    else printf("STATE: closed (%s)\n", error ? error : "unknown");
}

int main(int argc, char **argv) {
    const char *url = argc > 1 ? argv[1]
        : "wss://dashscope.aliyuncs.com/api-ws/v1/inference";
    if (!bongo_cat_ws_backend_available()) {
        fprintf(stderr, "WebSocket backend is not compiled in\n");
        return 2;
    }
    char auth[256];
    const char *key = SDL_getenv("BONGO_CAT_DASHSCOPE_API_KEY");
    const char *headers[3] = {NULL, NULL, NULL};
    int header_count = 0;
    if (key && key[0]) {
        snprintf(auth, sizeof(auth), "Authorization: bearer %s", key);
        headers[header_count++] = auth;
    } else {
        printf("BONGO_CAT_DASHSCOPE_API_KEY not set; handshake will likely "
            "be rejected with 401 (TLS transport still verified)\n");
    }
    BongoCatWsClient *client = bongo_cat_ws_connect(url, headers, on_text,
        on_binary, on_state, NULL);
    if (!client) {
        fprintf(stderr, "connect failed: %s\n",
            bongo_cat_ws_last_error(NULL));
        return 1;
    }
    bool tts_mode = getenv("PROBE_TTS") != NULL;
    const char *tts_run =
        "{\"header\":{\"action\":\"run-task\",\"task_id\":\"probe-tts-1\","
        "\"streaming\":\"duplex\"},\"payload\":{\"task_group\":\"audio\","
        "\"task\":\"tts\",\"function\":\"SpeechSynthesizer\","
        "\"model\":\"qwen-audio-3.1-tts-flash\",\"parameters\":"
        "{\"text_type\":\"PlainText\",\"voice\":\"yunhuanhuan_v3.1\","
        "\"format\":\"pcm\",\"sample_rate\":24000},\"input\":{}}}";
    const char *tts_continue =
        "{\"header\":{\"action\":\"continue-task\",\"task_id\":"
        "\"probe-tts-1\",\"streaming\":\"duplex\"},\"payload\":{\"input\":"
        "{\"text\":\"你好，这是一段语音播报测试。\"}}}";
    const char *tts_finish =
        "{\"header\":{\"action\":\"finish-task\",\"task_id\":\"probe-tts-1\","
        "\"streaming\":\"duplex\"},\"payload\":{\"input\":{}}}";
    int stage = 0;
    Uint64 deadline = SDL_GetTicksNS() + (tts_mode ? 30ull
        : 15ull) * 1000000000ull;
    while (SDL_GetTicksNS() < deadline) {
        if (!bongo_cat_ws_run(client, 200)) break;
        if (bongo_cat_ws_connected(client)) {
            if (!tts_mode) {
                printf("connected=%d\n", 1);
                break;
            }
            if (stage == 0) {
                bongo_cat_ws_send_text(client, tts_run);
                stage = 1;
                printf("SENT run-task\n");
            } else if (stage == 1 && got_started) {
                bongo_cat_ws_send_text(client, tts_continue);
                bongo_cat_ws_send_text(client, tts_finish);
                stage = 2;
                printf("SENT continue+finish\n");
            } else if (stage == 2 && got_finished) {
                printf("TTS PROBE DONE\n");
                break;
            }
        }
    }
    bool ok = bongo_cat_ws_connected(client);
    printf("%s: %s\n", ok ? "HANDSHAKE OK" : "HANDSHAKE FAILED",
        ok ? "" : bongo_cat_ws_last_error(client));
    bongo_cat_ws_close(client);
    return ok ? 0 : 1;
}
