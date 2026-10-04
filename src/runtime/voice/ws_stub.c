#if !defined(BONGO_CAT_HAS_WS_BACKEND)

#include "ws_client.h"

#include <SDL3/SDL.h>

static const char *const NO_BACKEND =
    "No WebSocket backend was compiled into this build";

bool bongo_cat_ws_backend_available(void) {
    return false;
}

const char *bongo_cat_ws_last_error(const BongoCatWsClient *client) {
    (void)client;
    return NO_BACKEND;
}

BongoCatWsClient *bongo_cat_ws_connect(const char *url,
    const char *const *headers, BongoCatWsTextCallback on_text,
    BongoCatWsBinaryCallback on_binary, BongoCatWsClosedCallback on_state,
    void *userdata) {
    (void)url; (void)headers; (void)on_text; (void)on_binary;
    (void)userdata;
    if (on_state) on_state(userdata, false, NO_BACKEND);
    return NULL;
}

bool bongo_cat_ws_send_text(BongoCatWsClient *client, const char *json) {
    (void)client; (void)json;
    return false;
}

bool bongo_cat_ws_send_binary(BongoCatWsClient *client, const void *data,
    size_t length) {
    (void)client; (void)data; (void)length;
    return false;
}

bool bongo_cat_ws_connected(const BongoCatWsClient *client) {
    (void)client;
    return false;
}

bool bongo_cat_ws_run(BongoCatWsClient *client, int timeout_ms) {
    (void)client;
    SDL_Delay((Uint32)(timeout_ms > 0 ? timeout_ms : 0));
    return false;
}

void bongo_cat_ws_close(BongoCatWsClient *client) {
    (void)client;
}

#endif
