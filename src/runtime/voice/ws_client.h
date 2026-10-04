#ifndef BONGO_CAT_WS_CLIENT_H
#define BONGO_CAT_WS_CLIENT_H

#include <stdbool.h>
#include <stddef.h>

/* WebSocket client abstraction owned by exactly one worker thread.
   Callbacks fire from bongo_cat_ws_run() on the calling thread. */
typedef struct BongoCatWsClient BongoCatWsClient;

typedef void (*BongoCatWsTextCallback)(void *userdata, const char *text,
    size_t length);
typedef void (*BongoCatWsBinaryCallback)(void *userdata, const void *data,
    size_t length);
typedef void (*BongoCatWsClosedCallback)(void *userdata, bool connected,
    const char *error);

BongoCatWsClient *bongo_cat_ws_connect(const char *url,
    const char *const *headers, BongoCatWsTextCallback on_text,
    BongoCatWsBinaryCallback on_binary, BongoCatWsClosedCallback on_state,
    void *userdata);
bool bongo_cat_ws_send_text(BongoCatWsClient *client, const char *json);
bool bongo_cat_ws_send_binary(BongoCatWsClient *client, const void *data,
    size_t length);
bool bongo_cat_ws_connected(const BongoCatWsClient *client);
/* Pumps transport events for up to timeout_ms. Returns false once the
   connection is permanently closed. */
bool bongo_cat_ws_run(BongoCatWsClient *client, int timeout_ms);
void bongo_cat_ws_close(BongoCatWsClient *client);
bool bongo_cat_ws_backend_available(void);
const char *bongo_cat_ws_last_error(const BongoCatWsClient *client);

#endif
