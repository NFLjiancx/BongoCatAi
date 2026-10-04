#if defined(BONGO_CAT_HAS_WS_BACKEND)

#include "ws_client.h"

#include <SDL3/SDL.h>
#include <libwebsockets.h>

#include <stdio.h>
#include <string.h>

#define WS_TEXT_ACCUM_CAP (256 * 1024)
#define WS_QUEUE_CAP (4u * 1024u * 1024u)
#define WS_HEADER_CAP 8

typedef struct WsFrame {
    struct WsFrame *next;
    size_t length;
    bool binary;
    unsigned char data[];
} WsFrame;

struct BongoCatWsClient {
    struct lws_context *context;
    struct lws *wsi;
    BongoCatWsTextCallback on_text;
    BongoCatWsBinaryCallback on_binary;
    BongoCatWsClosedCallback on_state;
    void *userdata;
    char *header_copies[WS_HEADER_CAP];
    WsFrame *out_head, *out_tail;
    size_t queued_bytes;
    char *text_accum;
    size_t text_length, text_capacity;
    char error[256];
    bool connected;
    bool failed;
    bool state_reported;
};

static void report_closed(BongoCatWsClient *client, const char *error) {
    if (client->state_reported) return;
    client->state_reported = true;
    if (client->on_state) client->on_state(client->userdata, false, error);
}

static int ws_protocol_callback(struct lws *wsi,
    enum lws_callback_reasons reason, void *user, void *in, size_t len) {
    BongoCatWsClient *client =
        (BongoCatWsClient *)lws_context_user(lws_get_context(wsi));
    (void)user;
    if (!client) return 0;
    switch (reason) {
        case LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER: {
            unsigned char **cursor = in;
            unsigned char *end = *cursor + len;
            for (int i = 0; i < WS_HEADER_CAP && client->header_copies[i];
                ++i) {
                char *header = client->header_copies[i];
                char *colon = strchr(header, ':');
                if (!colon) continue;
                char name[64];
                size_t name_length = (size_t)(colon - header);
                if (name_length == 0 || name_length >= sizeof(name) - 1)
                    continue;
                memcpy(name, header, name_length);
                name[name_length] = ':';
                name[name_length + 1] = '\0';
                const char *value = colon + 1;
                while (*value == ' ') ++value;
                if (lws_add_http_header_by_name(wsi, (unsigned char *)name,
                    (unsigned char *)value, (int)strlen(value), cursor,
                    end)) return -1;
            }
            break;
        }
        case LWS_CALLBACK_CLIENT_ESTABLISHED:
            client->connected = true;
            if (client->on_state) client->on_state(client->userdata, true,
                NULL);
            break;
        case LWS_CALLBACK_CLIENT_RECEIVE:
            if (lws_frame_is_binary(wsi)) {
                if (client->on_binary && len)
                    client->on_binary(client->userdata, in, len);
                break;
            }
            if (client->text_length + len + 1 > client->text_capacity) {
                size_t want = client->text_length + len + 4096;
                if (want > WS_TEXT_ACCUM_CAP) want = WS_TEXT_ACCUM_CAP;
                if (client->text_length + len + 1 > want) {
                    client->text_length = 0;
                    break;
                }
                char *grown = SDL_realloc(client->text_accum, want);
                if (!grown) return -1;
                client->text_accum = grown;
                client->text_capacity = want;
            }
            if (len && client->text_length + len < client->text_capacity) {
                SDL_memcpy(client->text_accum + client->text_length, in, len);
                client->text_length += len;
                client->text_accum[client->text_length] = '\0';
            }
            if (lws_is_final_fragment(wsi)) {
                if (client->on_text && client->text_length)
                    client->on_text(client->userdata, client->text_accum,
                        client->text_length);
                client->text_length = 0;
                if (client->text_accum) client->text_accum[0] = '\0';
            }
            break;
        case LWS_CALLBACK_CLIENT_WRITEABLE: {
            WsFrame *frame = client->out_head;
            if (!frame) return 0;
            unsigned char *buffer = SDL_malloc(LWS_PRE + frame->length);
            if (!buffer) return -1;
            SDL_memcpy(buffer + LWS_PRE, frame->data, frame->length);
            int written = lws_write(wsi, buffer + LWS_PRE, frame->length,
                frame->binary ? LWS_WRITE_BINARY : LWS_WRITE_TEXT);
            SDL_free(buffer);
            if (written < (int)frame->length) {
                SDL_snprintf(client->error, sizeof(client->error),
                    "WebSocket frame send failed");
                return -1;
            }
            client->out_head = frame->next;
            if (!client->out_head) client->out_tail = NULL;
            client->queued_bytes -= frame->length;
            SDL_free(frame);
            if (client->out_head) lws_callback_on_writable(wsi);
            break;
        }
        case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
            client->connected = false;
            client->failed = true;
            SDL_snprintf(client->error, sizeof(client->error), "%s",
                in && ((char *)in)[0] ? (char *)in : "connection failed");
            report_closed(client, client->error);
            break;
        case LWS_CALLBACK_CLIENT_CLOSED:
            client->connected = false;
            client->failed = true;
            if (!client->error[0])
                SDL_snprintf(client->error, sizeof(client->error), "closed");
            report_closed(client, client->error);
            break;
        default:
            break;
    }
    return 0;
}

static const struct lws_protocols ws_protocols[] = {
    { "bongo-cat-ws", ws_protocol_callback, 0, 8192, 0, NULL, 0 },
    LWS_PROTOCOL_LIST_TERM
};

bool bongo_cat_ws_backend_available(void) {
    return true;
}

static char g_connect_error[256];

static struct lws_context *g_ssl_keepalive;
static SDL_SpinLock g_ssl_keepalive_lock;

/* lws runs OPENSSL_cleanup() when the last context created with
   LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT is destroyed. OpenSSL 3 cannot be
   re-initialized after that, so the next lws_create_context() crashes inside
   EVP_MD_fetch. Keep one process-lifetime context to pin the refcount. */
static void ensure_ssl_keepalive(void) {
    SDL_LockSpinlock(&g_ssl_keepalive_lock);
    if (!g_ssl_keepalive) {
        struct lws_context_creation_info info = {0};
        info.port = CONTEXT_PORT_NO_LISTEN;
        info.protocols = ws_protocols;
        info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
        g_ssl_keepalive = lws_create_context(&info);
    }
    SDL_UnlockSpinlock(&g_ssl_keepalive_lock);
}

const char *bongo_cat_ws_last_error(const BongoCatWsClient *client) {
    if (!client) return g_connect_error[0] ? g_connect_error
        : "no client";
    return client->error[0] ? client->error : "unknown WebSocket error";
}

static bool enqueue(BongoCatWsClient *client, const void *data,
    size_t length, bool binary) {
    if (!client || !client->connected || !data || !length) return false;
    if (client->queued_bytes + length > WS_QUEUE_CAP) return false;
    WsFrame *frame = SDL_malloc(sizeof(*frame) + length);
    if (!frame) return false;
    frame->next = NULL;
    frame->length = length;
    frame->binary = binary;
    SDL_memcpy(frame->data, data, length);
    if (client->out_tail) client->out_tail->next = frame;
    else client->out_head = frame;
    client->out_tail = frame;
    client->queued_bytes += length;
    lws_callback_on_writable(client->wsi);
    return true;
}

static bool split_url(const char *url, char *host, size_t host_cap,
    char *path, size_t path_cap, int *port, bool *ssl) {
    const char *rest = NULL;
    if (SDL_strncasecmp(url, "wss://", 6) == 0) {
        *ssl = true;
        *port = 443;
        rest = url + 6;
    } else if (SDL_strncasecmp(url, "ws://", 5) == 0) {
        *ssl = false;
        *port = 80;
        rest = url + 5;
    } else return false;
    const char *slash = SDL_strchr(rest, '/');
    size_t authority_length = slash ? (size_t)(slash - rest) : SDL_strlen(rest);
    if (!authority_length || authority_length >= host_cap) return false;
    SDL_memcpy(host, rest, authority_length);
    host[authority_length] = '\0';
    if (slash) SDL_strlcpy(path, slash, path_cap);
    else SDL_strlcpy(path, "/", path_cap);
    char *colon = SDL_strrchr(host, ':');
    if (colon && colon[1] != '\0') {
        char *end = NULL;
        long value = SDL_strtol(colon + 1, &end, 10);
        if (!end || *end || value <= 0 || value > 65535) return false;
        *port = (int)value;
        *colon = '\0';
    }
    return true;
}

static char proxy_host[128];

/* lws reads http_proxy verbatim and misparses URLs like "http://host:port/".
   Normalize the environment value into the plain "host" plus port that the
   context creation info expects. */
static void configure_proxy(struct lws_context_creation_info *info) {
    static const char *names[] = {"https_proxy", "HTTPS_PROXY", "http_proxy",
        "HTTP_PROXY"};
    for (int i = 0; i < 4; ++i) {
        const char *value = SDL_getenv(names[i]);
        if (!value || !*value) continue;
        if (SDL_strncasecmp(value, "http://", 7) == 0) value += 7;
        else if (SDL_strncasecmp(value, "https://", 8) == 0) value += 8;
        char trimmed[160];
        SDL_strlcpy(trimmed, value, sizeof(trimmed));
        char *slash = SDL_strchr(trimmed, '/');
        if (slash) *slash = '\0';
        char *colon = SDL_strrchr(trimmed, ':');
        if (colon) {
            *colon = '\0';
            info->http_proxy_port = SDL_atoi(colon + 1);
        } else info->http_proxy_port = 80;
        if (!*trimmed) continue;
        SDL_strlcpy(proxy_host, trimmed, sizeof(proxy_host));
        info->http_proxy_address = proxy_host;
        return;
    }
}

BongoCatWsClient *bongo_cat_ws_connect(const char *url,
    const char *const *headers, BongoCatWsTextCallback on_text,
    BongoCatWsBinaryCallback on_binary, BongoCatWsClosedCallback on_state,
    void *userdata) {
    if (!url) return NULL;
    char host[256], path[512];
    int port = 0;
    bool ssl = false;
    if (!split_url(url, host, sizeof(host), path, sizeof(path), &port, &ssl))
        return NULL;
    BongoCatWsClient *client = SDL_calloc(1, sizeof(*client));
    if (!client) return NULL;
    client->on_text = on_text;
    client->on_binary = on_binary;
    client->on_state = on_state;
    client->userdata = userdata;
    for (int i = 0; headers && headers[i] && i < WS_HEADER_CAP; ++i) {
        client->header_copies[i] = SDL_strdup(headers[i]);
        if (!client->header_copies[i]) {
            SDL_snprintf(client->error, sizeof(client->error),
                "WebSocket header allocation failed");
            SDL_strlcpy(g_connect_error, client->error,
                sizeof(g_connect_error));
            bongo_cat_ws_close(client);
            return NULL;
        }
    }
    struct lws_context_creation_info context_info = {0};
    ensure_ssl_keepalive();
    context_info.port = CONTEXT_PORT_NO_LISTEN;
    context_info.protocols = ws_protocols;
    context_info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
    context_info.user = client;
    configure_proxy(&context_info);
    if (SDL_getenv("BONGO_CAT_WS_DEBUG"))
        lws_set_log_level(LLL_USER | LLL_ERR | LLL_WARN | LLL_NOTICE
            | LLL_HEADER | LLL_DEBUG | LLL_INFO, NULL);
    client->context = lws_create_context(&context_info);
    if (!client->context) {
        SDL_snprintf(client->error, sizeof(client->error),
            "WebSocket context creation failed");
        SDL_strlcpy(g_connect_error, client->error, sizeof(g_connect_error));
        bongo_cat_ws_close(client);
        return NULL;
    }
    struct lws_client_connect_info connect_info = {0};
    connect_info.context = client->context;
    connect_info.address = host;
    connect_info.host = host;
    connect_info.port = port;
    connect_info.path = path;
    connect_info.protocol = ws_protocols[0].name;
    if (ssl) connect_info.ssl_connection = LCCSCF_USE_SSL;
    connect_info.ietf_version_or_minus_one = -1;
    client->wsi = lws_client_connect_via_info(&connect_info);
    if (!client->wsi) {
        SDL_snprintf(client->error, sizeof(client->error),
            "WebSocket connect request failed: rejected");
        SDL_strlcpy(g_connect_error, client->error, sizeof(g_connect_error));
        bongo_cat_ws_close(client);
        return NULL;
    }
    return client;
}

bool bongo_cat_ws_send_text(BongoCatWsClient *client, const char *json) {
    if (!json) return false;
    return enqueue(client, json, strlen(json), false);
}

bool bongo_cat_ws_send_binary(BongoCatWsClient *client, const void *data,
    size_t length) {
    return enqueue(client, data, length, true);
}

bool bongo_cat_ws_connected(const BongoCatWsClient *client) {
    return client && client->connected;
}

bool bongo_cat_ws_run(BongoCatWsClient *client, int timeout_ms) {
    if (!client || !client->context) return false;
    /* lws >= 4.5 discards a positive lws_service() timeout and waits for
       poll events instead, which starves cross-thread requests while the
       connection is idle. Service non-blocking and sleep ourselves. */
    lws_service(client->context, -1);
    if (timeout_ms > 0) SDL_Delay((Uint32)timeout_ms);
    return !client->failed || client->out_head != NULL;
}

void bongo_cat_ws_close(BongoCatWsClient *client) {
    if (!client) return;
    SDL_free(client->text_accum);
    WsFrame *frame = client->out_head;
    while (frame) {
        WsFrame *next = frame->next;
        SDL_free(frame);
        frame = next;
    }
    for (int i = 0; i < WS_HEADER_CAP; ++i) SDL_free(client->header_copies[i]);
    if (client->context) lws_context_destroy(client->context);
    SDL_free(client);
}

#endif
