#include "asr_worker.h"
#include "voice_internal.h"
#include "ws_client.h"
#include "bongo_cat/log.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ASR_URL "wss://dashscope.aliyuncs.com/api-ws/v1/inference"
#define ASR_API_KEY_CAP 512
#define ASR_CHUNK_BYTES 3200 /* 100 ms of 16 kHz s16 mono. */
#define ASR_JSON_CAP 4096
#define ASR_START_TIMEOUT_NS (2500ull * 1000000ull)
#define ASR_FINISH_TIMEOUT_NS (8ull * 1000000000ull)
#define ASR_RECONNECT_NS (2ull * 1000000000ull)
#define ASR_RETRY_WAIT_NS (150ull * 1000000ull)
#define ASR_HANDSHAKE_TIMEOUT_NS (10ull * 1000000000ull)

typedef enum {
    ASR_PHASE_DISCONNECTED = 0,
    ASR_PHASE_CONNECTED,   /* No task in flight. */
    ASR_PHASE_STARTING,    /* run-task sent, waiting for task-started. */
    ASR_PHASE_RUNNING,     /* Streaming PCM frames. */
    ASR_PHASE_FINISHING    /* finish-task sent, waiting for final result. */
} AsrPhase;

typedef struct AsrSession {
    BongoCatAsr *asr;
    BongoCatWsClient *client;
    AsrPhase phase;
    char task_id[BONGO_CAT_DS_TASK_ID_CAP];
    bool finish_pending;
    bool result_seen;
    size_t sent_bytes;
    bool established; /* Handshake completed at least once for this client. */
    bool fresh_reconnect; /* Waiting to re-handshake before next task. */
    bool task_used; /* This client has carried at least one task. */
    Uint64 deadline_ns;
    Uint64 reconnect_at_ns;
    Uint64 begin_requested_ns; /* When the utterance asked for this task. */
} AsrSession;

struct BongoCatAsr {
    BongoCatVoice *voice;
    char model[64];
    char format[32];
    int sample_rate;
    bool disfluency_removal_enabled;
    bool semantic_punctuation_enabled;
    char api_key[ASR_API_KEY_CAP];
    SDL_Thread *thread;
    SDL_AtomicInt quit;
    SDL_AtomicInt want_begin;
    SDL_AtomicInt want_end;
    SDL_AtomicInt want_abort;
    SDL_AtomicInt running;
};

static void session_report_error(AsrSession *session, const char *message) {
    bongo_cat_voice_report_error(session->asr->voice, message);
}

/* Drops the current task without reporting its results. */
static void session_abandon_task(AsrSession *session) {
    if (session->phase == ASR_PHASE_STARTING ||
        session->phase == ASR_PHASE_RUNNING ||
        session->phase == ASR_PHASE_FINISHING) {
        char json[ASR_JSON_CAP];
        if (bongo_cat_dashscope_build_finish_task(json, sizeof(json),
            session->task_id)) bongo_cat_ws_send_text(session->client, json);
    }
    session->finish_pending = false;
    session->result_seen = false;
    session->begin_requested_ns = 0;
    session->phase = session->client && bongo_cat_ws_connected(session->client)
        ? ASR_PHASE_CONNECTED : ASR_PHASE_DISCONNECTED;
}

static void session_send_remaining(AsrSession *session) {
    unsigned char chunk[ASR_CHUNK_BYTES];
    BongoCatVoice *voice = session->asr->voice;
    size_t n;
    while (session->phase == ASR_PHASE_RUNNING &&
        (n = bongo_cat_voice_ring_read(&voice->ring, chunk,
        ASR_CHUNK_BYTES)) > 0) {
        if (!bongo_cat_ws_send_binary(session->client, chunk, n)) {
            SDL_LogWarn(BONGO_CAT_LOG_LIFECYCLE,
                "ASR binary send failed n=%zu ring=%zu", n,
                bongo_cat_voice_ring_available(&voice->ring));
            break;
        }
        session->sent_bytes += n;
        if (n < ASR_CHUNK_BYTES) break;
    }
}

static bool session_start_task(AsrSession *session) {
    BongoCatAsr *asr = session->asr;
    BongoCatDashscopeAsrConfig config = {asr->model, asr->format,
        asr->sample_rate, NULL, 0, asr->disfluency_removal_enabled,
        asr->semantic_punctuation_enabled};
    char json[ASR_JSON_CAP];
    bongo_cat_dashscope_new_task_id(session->task_id,
        sizeof(session->task_id));
    if (!bongo_cat_dashscope_build_run_task(json, sizeof(json),
        session->task_id, &config)) {
        session_report_error(session, "ASR request build failed");
        return false;
    }
    if (!bongo_cat_ws_send_text(session->client, json)) {
        session_report_error(session, "ASR request send failed");
        return false;
    }
    session->finish_pending = false;
    session->result_seen = false;
    session->sent_bytes = 0;
    session->phase = ASR_PHASE_STARTING;
    session->task_used = true;
    session->deadline_ns = SDL_GetTicksNS() + ASR_START_TIMEOUT_NS;
    return true;
}

static void session_finish_task(AsrSession *session) {
    if (session->phase == ASR_PHASE_STARTING) {
        session->finish_pending = true;
        return;
    }
    if (session->phase == ASR_PHASE_CONNECTED ||
        session->phase == ASR_PHASE_DISCONNECTED) {
        /* The utterance ended before any task could start (e.g. no
           connectivity); surface that now instead of idling until the
           recognize watchdog fires. */
        session_report_error(session, "ASR connection unavailable");
        return;
    }
    if (session->phase != ASR_PHASE_RUNNING) return;
    session_send_remaining(session);
    char json[ASR_JSON_CAP];
    if (!bongo_cat_dashscope_build_finish_task(json, sizeof(json),
        session->task_id) ||
        !bongo_cat_ws_send_text(session->client, json)) {
        session_report_error(session, "ASR finish send failed");
        session_abandon_task(session);
        return;
    }
    session->phase = ASR_PHASE_FINISHING;
    session->deadline_ns = SDL_GetTicksNS() + ASR_FINISH_TIMEOUT_NS;
}

static void session_publish_text(AsrSession *session,
    const BongoCatDashscopeEvent *event) {
    BongoCatVoice *voice = session->asr->voice;
    SDL_LockMutex(voice->mutex);
    if (event->sentence_end) {
        snprintf(voice->final_text, sizeof(voice->final_text), "%s",
            event->text);
        voice->partial_text[0] = '\0';
    } else {
        snprintf(voice->partial_text, sizeof(voice->partial_text), "%s",
            event->text);
    }
    SDL_UnlockMutex(voice->mutex);
    if (event->sentence_end)
        SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE, "ASR final: %s", event->text);
    bongo_cat_voice_post(voice, event->sentence_end
        ? BONGO_CAT_VOICE_EV_ASR_FINAL : BONGO_CAT_VOICE_EV_ASR_PARTIAL);
}

static void session_on_text(void *userdata, const char *text,
    size_t length) {
    AsrSession *session = userdata;
    BongoCatDashscopeEvent event;
    if (!bongo_cat_dashscope_parse_event(text, length, &event)) {
        SDL_LogWarn(BONGO_CAT_LOG_LIFECYCLE, "ASR unparseable frame: %.*s",
            (int)(length > 200 ? 200 : length), text);
        return;
    }
    if (event.task_id[0] && strcmp(event.task_id, session->task_id) != 0)
        return; /* Stale task or unrelated channel. */
    switch (event.kind) {
        case BONGO_CAT_DS_EVENT_TASK_STARTED:
            if (session->phase != ASR_PHASE_STARTING) break;
            SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
                "ASR task started in %llu ms", (unsigned long long)(
                session->begin_requested_ns
                    ? (SDL_GetTicksNS() - session->begin_requested_ns)
                    / 1000000ull : 0));
            session->begin_requested_ns = 0;
            session->phase = ASR_PHASE_RUNNING;
            session->deadline_ns = 0;
            if (session->finish_pending) {
                session->finish_pending = false;
                session_finish_task(session);
            }
            break;
        case BONGO_CAT_DS_EVENT_RESULT:
            if (session->phase != ASR_PHASE_RUNNING &&
                session->phase != ASR_PHASE_FINISHING) break;
            if (event.heartbeat || !event.text[0]) break;
            if (event.sentence_end) session->result_seen = true;
            session_publish_text(session, &event);
            break;
        case BONGO_CAT_DS_EVENT_TASK_FINISHED:
            if (session->phase == ASR_PHASE_FINISHING ||
                session->phase == ASR_PHASE_RUNNING) {
                session->phase = ASR_PHASE_CONNECTED;
                session->deadline_ns = 0;
            }
            break;
        case BONGO_CAT_DS_EVENT_TASK_FAILED: {
            char message[BONGO_CAT_DS_ERROR_CAP + 96];
            if (event.error_message[0])
                snprintf(message, sizeof(message), "ASR %s: %s",
                    event.error_code, event.error_message);
            else
                snprintf(message, sizeof(message), "ASR task failed (%s)",
                    event.error_code);
            session_report_error(session, message);
            session_abandon_task(session);
            break;
        }
        default:
            break;
    }
}

static void session_on_binary(void *userdata, const void *data,
    size_t length) {
    (void)userdata; (void)data; (void)length;
}

static void session_on_state(void *userdata, bool connected,
    const char *error) {
    AsrSession *session = userdata;
    if (connected) {
        session->established = true;
        if (session->phase == ASR_PHASE_DISCONNECTED)
            session->phase = ASR_PHASE_CONNECTED;
        return;
    }
    if (session->established &&
        (session->phase == ASR_PHASE_RUNNING ||
            session->phase == ASR_PHASE_STARTING ||
            session->phase == ASR_PHASE_FINISHING))
        session_report_error(session, error && error[0] ? error
            : "ASR connection lost");
    session->phase = ASR_PHASE_DISCONNECTED;
}

static void session_drop_connection(AsrSession *session);

static void session_handle_requests(AsrSession *session) {
    BongoCatAsr *asr = session->asr;
    if (SDL_GetAtomicInt(&asr->want_begin) && !session->begin_requested_ns)
        session->begin_requested_ns = SDL_GetTicksNS();
    if (SDL_GetAtomicInt(&asr->want_abort)) {
        SDL_SetAtomicInt(&asr->want_abort, 0);
        SDL_SetAtomicInt(&asr->want_begin, 0);
        SDL_SetAtomicInt(&asr->want_end, 0);
        session_abandon_task(session);
        return;
    }
    if (SDL_GetAtomicInt(&asr->want_begin)) {
        if (session->phase == ASR_PHASE_CONNECTED && session->task_used &&
            !session->fresh_reconnect) {
            /* Servers drop idle sockets without notice; take a fresh
               connection for each utterance instead of queuing onto a
               possibly dead one. */
            session_drop_connection(session);
            session->reconnect_at_ns = 0;
            session->fresh_reconnect = true;
        } else if (session->phase == ASR_PHASE_CONNECTED) {
            session->fresh_reconnect = false;
            SDL_SetAtomicInt(&asr->want_begin, 0);
            session_start_task(session);
        }
        /* Otherwise keep the flag pending until a task slot is free. */
    }
    if (SDL_GetAtomicInt(&asr->want_end)) {
        SDL_SetAtomicInt(&asr->want_end, 0);
        session_finish_task(session);
    }
}

static void session_check_deadlines(AsrSession *session) {
    if (!session->deadline_ns || SDL_GetTicksNS() < session->deadline_ns)
        return;
    if (session->phase == ASR_PHASE_STARTING)
        session_report_error(session, "ASR start timeout");
    else if (session->phase == ASR_PHASE_FINISHING)
        session_report_error(session, session->result_seen
            ? "ASR finish timeout" : "ASR no result");
    session_abandon_task(session);
}

/* An utterance waiting in the ring outranks politeness: while the user is
   speaking, retry the socket almost at once instead of letting the 4 s buffer
   overflow with the beginning of the sentence already lost. */
static Uint64 reconnect_wait_ns(const BongoCatAsr *asr) {
    return SDL_GetAtomicInt((SDL_AtomicInt *)&asr->want_begin)
        ? ASR_RETRY_WAIT_NS : ASR_RECONNECT_NS;
}

static void session_drop_connection(AsrSession *session) {
    if (session->client) {
        session->established = false;
        bongo_cat_ws_close(session->client);
        session->client = NULL;
    }
    session->finish_pending = false;
    session->result_seen = false;
    session->fresh_reconnect = false;
    session->task_used = false;
    session->task_id[0] = '\0';
    session->deadline_ns = 0;
    session->phase = ASR_PHASE_DISCONNECTED;
    session->reconnect_at_ns = SDL_GetTicksNS()
        + reconnect_wait_ns(session->asr);
}

static int worker_run(void *userdata) {
    AsrSession session = {0};
    BongoCatAsr *asr = userdata;
    session.asr = asr;
    session.phase = ASR_PHASE_DISCONNECTED;
    Uint64 handshake_deadline_ns = 0;
    char auth_header[ASR_API_KEY_CAP + 32];
    snprintf(auth_header, sizeof(auth_header), "Authorization: bearer %s",
        asr->api_key);
    const char *headers[] = {auth_header, NULL};
    while (!SDL_GetAtomicInt(&asr->quit)) {
        if (!session.client) {
            if (SDL_GetAtomicInt(&asr->want_begin))
                session.reconnect_at_ns = 0;
            /* An utterance already waiting cuts the backoff short: a sentence
               must not expire in the listening window because this socket chose
               to rest politely. */
            if (SDL_GetTicksNS() < session.reconnect_at_ns) {
                SDL_Delay(50);
                continue;
            }
            session.client = bongo_cat_ws_connect(ASR_URL, headers,
                session_on_text, session_on_binary, session_on_state,
                &session);
            if (!session.client) {
                SDL_LogWarn(BONGO_CAT_LOG_LIFECYCLE,
                    "ASR connect failed: %s", bongo_cat_ws_last_error(NULL));
                session.reconnect_at_ns = SDL_GetTicksNS()
                    + reconnect_wait_ns(asr);
                continue;
            }
            session.established = false;
            session.phase = ASR_PHASE_DISCONNECTED;
            handshake_deadline_ns = SDL_GetTicksNS()
                + ASR_HANDSHAKE_TIMEOUT_NS;
        }
        if (session.established && !bongo_cat_ws_connected(session.client)) {
            /* The peer or transport dropped an established connection. */
            session_drop_connection(&session);
            if (SDL_GetAtomicInt(&asr->want_begin))
                session.reconnect_at_ns = 0;
            continue;
        }
        if (!session.established &&
            SDL_GetTicksNS() >= handshake_deadline_ns) {
            SDL_LogWarn(BONGO_CAT_LOG_LIFECYCLE, "ASR handshake timeout");
            session_drop_connection(&session);
            continue;
        }
        if (bongo_cat_ws_connected(session.client)) {
            session_handle_requests(&session);
            if (session.phase == ASR_PHASE_RUNNING)
                session_send_remaining(&session);
            session_check_deadlines(&session);
        }
        const bool was_established = session.established;
        if (!bongo_cat_ws_run(session.client, 20)) {
            session_drop_connection(&session);
            /* Retry at once only when this client had actually connected
               before (a dying socket race); a server that refuses every
               connection must keep the polite backoff. */
            if (was_established && SDL_GetAtomicInt(&asr->want_begin))
                session.reconnect_at_ns = 0;
            continue;
        }
    }
    session.established = false; /* Stop the state callback from reporting. */
    if (session.client) bongo_cat_ws_close(session.client);
    session.client = NULL;
    SDL_memset(auth_header, 0, sizeof(auth_header));
    return 0;
}

BongoCatAsr *bongo_cat_asr_create(BongoCatVoice *voice,
    const BongoCatDashscopeAsrConfig *config) {
    if (!voice || !config || !config->model || !config->model[0]) return NULL;
    if (!bongo_cat_ws_backend_available()) return NULL;
    BongoCatAsr *asr = calloc(1, sizeof(*asr));
    if (!asr) return NULL;
    asr->voice = voice;
    SDL_strlcpy(asr->model, config->model, sizeof(asr->model));
    SDL_strlcpy(asr->format, config->format && config->format[0]
        ? config->format : "pcm", sizeof(asr->format));
    asr->sample_rate = config->sample_rate > 0 ? config->sample_rate : 16000;
    asr->disfluency_removal_enabled = config->disfluency_removal_enabled;
    asr->semantic_punctuation_enabled = config->semantic_punctuation_enabled;
    SDL_SetAtomicInt(&asr->quit, 0);
    SDL_SetAtomicInt(&asr->running, 0);
    return asr;
}

bool bongo_cat_asr_start(BongoCatAsr *asr, const char *api_key) {
    if (!asr || !api_key || !api_key[0]) return false;
    if (SDL_GetAtomicInt(&asr->running)) return true;
    if (strlen(api_key) >= sizeof(asr->api_key)) return false;
    SDL_strlcpy(asr->api_key, api_key, sizeof(asr->api_key));
    SDL_SetAtomicInt(&asr->quit, 0);
    asr->thread = SDL_CreateThread(worker_run, "bongo_asr", asr);
    if (!asr->thread) {
        asr->api_key[0] = '\0';
        return false;
    }
    SDL_SetAtomicInt(&asr->running, 1);
    return true;
}

void bongo_cat_asr_notify(BongoCatAsr *asr, BongoCatAsrNotify notify) {
    if (!asr) return;
    switch (notify) {
        case BONGO_CAT_ASR_NOTIFY_BEGIN:
            SDL_SetAtomicInt(&asr->want_begin, 1);
            break;
        case BONGO_CAT_ASR_NOTIFY_END:
            SDL_SetAtomicInt(&asr->want_end, 1);
            break;
        case BONGO_CAT_ASR_NOTIFY_ABORT:
            SDL_SetAtomicInt(&asr->want_abort, 1);
            break;
    }
}

bool bongo_cat_asr_running(const BongoCatAsr *asr) {
    return asr && SDL_GetAtomicInt(
        (SDL_AtomicInt *)&asr->running) != 0;
}

void bongo_cat_asr_stop(BongoCatAsr *asr) {
    if (!asr || !asr->thread) return;
    SDL_SetAtomicInt(&asr->quit, 1);
    SDL_WaitThread(asr->thread, NULL);
    asr->thread = NULL;
    SDL_SetAtomicInt(&asr->running, 0);
    asr->api_key[0] = '\0';
}

void bongo_cat_asr_destroy(BongoCatAsr *asr) {
    if (!asr) return;
    bongo_cat_asr_stop(asr);
    free(asr);
}
