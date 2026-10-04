#include "tts_worker.h"
#include "tts_envelope.h"
#include "dashscope_json.h"
#include "voice_internal.h"
#include "ws_client.h"
#include "bongo_cat/log.h"
#include "bongo_cat/path.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TTS_URL "wss://dashscope.aliyuncs.com/api-ws/v1/inference"
#define TTS_API_KEY_CAP 512
#define TTS_JSON_CAP 4096
#define TTS_TEXT_CAP 2048
/* Long enough for a generous answer; the bubble truncates earlier anyway. */
#define TTS_TEXT_MAX_CODEPOINTS 600
#define TTS_PCM_CAP (10u * 1024u * 1024u)
#define TTS_HANDSHAKE_TIMEOUT_NS (10ull * 1000000000ull)
#define TTS_START_TIMEOUT_NS (8ull * 1000000000ull)
#define TTS_FINISH_TIMEOUT_NS (30ull * 1000000000ull)

struct BongoCatTts {
    BongoCatVoice *voice;
    char model[64];
    char voice_name[64];
    int sample_rate;
    char api_key[TTS_API_KEY_CAP];
    SDL_Thread *thread;
    /* Guards the queued request: text, epoch and want_speak change together. */
    SDL_Mutex *job_mutex;
    char pending_text[TTS_TEXT_CAP];
    /* Voice turn the queued text answers; posted back with READY/ERROR. */
    SDL_AtomicInt epoch;
    SDL_AtomicInt want_speak;
    SDL_AtomicInt want_cancel;
    SDL_AtomicInt quit;
    SDL_AtomicInt running;
};

typedef struct TtsJob {
    BongoCatTts *tts;
    uint8_t *pcm;
    size_t pcm_size, pcm_cap;
    bool started;
    bool finished;
    bool failed;
    char error[352];
} TtsJob;

static void job_fail(TtsJob *job, const char *message) {
    if (job->failed) return;
    job->failed = true;
    snprintf(job->error, sizeof(job->error), "%s", message);
}

static void job_append(TtsJob *job, const void *data, size_t length) {
    if (job->failed || !length) return;
    if (length > TTS_PCM_CAP - job->pcm_size) {
        job_fail(job, "TTS audio exceeded size limit");
        return;
    }
    size_t cap = job->pcm_cap ? job->pcm_cap : 64u * 1024u;
    while (cap < job->pcm_size + length) cap *= 2;
    if (cap > TTS_PCM_CAP) cap = TTS_PCM_CAP;
    uint8_t *grown = realloc(job->pcm, cap);
    if (!grown) {
        job_fail(job, "TTS out of memory");
        return;
    }
    job->pcm = grown;
    job->pcm_cap = cap;
    memcpy(job->pcm + job->pcm_size, data, length);
    job->pcm_size += length;
}

static void tts_on_text(void *userdata, const char *text, size_t length) {
    TtsJob *job = userdata;
    BongoCatDashscopeEvent event;
    if (!bongo_cat_dashscope_parse_event(text, length, &event)) return;
    switch (event.kind) {
        case BONGO_CAT_DS_EVENT_TASK_STARTED:
            job->started = true;
            break;
        case BONGO_CAT_DS_EVENT_TASK_FINISHED:
            job->finished = true;
            break;
        case BONGO_CAT_DS_EVENT_TASK_FAILED: {
            char message[sizeof(job->error)];
            if (event.error_message[0])
                snprintf(message, sizeof(message), "TTS %s: %s",
                    event.error_code, event.error_message);
            else
                snprintf(message, sizeof(message), "TTS task failed (%s)",
                    event.error_code);
            job_fail(job, message);
            break;
        }
        default:
            break;
    }
}

static void tts_on_binary(void *userdata, const void *data, size_t length) {
    job_append((TtsJob *)userdata, data, length);
}

static void tts_on_state(void *userdata, bool connected, const char *error) {
    (void)connected;
    /* Servers close the socket right after task-finished; that is a normal
       end of the job, not a failure. */
    if (error && error[0] && !((TtsJob *)userdata)->finished)
        job_fail((TtsJob *)userdata, error);
}

static bool write_u32_le(FILE *file, uint32_t value) {
    unsigned char bytes[4] = {(unsigned char)(value & 0xffu),
        (unsigned char)((value >> 8) & 0xffu),
        (unsigned char)((value >> 16) & 0xffu),
        (unsigned char)((value >> 24) & 0xffu)};
    return fwrite(bytes, 1, 4, file) == 4;
}

static bool write_u16_le(FILE *file, uint16_t value) {
    unsigned char bytes[2] = {(unsigned char)(value & 0xffu),
        (unsigned char)((value >> 8) & 0xffu)};
    return fwrite(bytes, 1, 2, file) == 2;
}

static bool write_wav(const char *path, const uint8_t *pcm, size_t pcm_size,
    int sample_rate) {
    FILE *file = fopen(path, "wb");
    if (!file) return false;
    uint32_t byte_rate = (uint32_t)sample_rate * 2u;
    bool ok = fwrite("RIFF", 1, 4, file) == 4 &&
        write_u32_le(file, (uint32_t)(36u + pcm_size)) &&
        fwrite("WAVEfmt ", 1, 8, file) == 8 &&
        write_u32_le(file, 16) && write_u16_le(file, 1) &&
        write_u16_le(file, 1) && write_u32_le(file, (uint32_t)sample_rate) &&
        write_u32_le(file, byte_rate) && write_u16_le(file, 2) &&
        write_u16_le(file, 16) &&
        fwrite("data", 1, 4, file) == 4 &&
        write_u32_le(file, (uint32_t)pcm_size) &&
        (pcm_size == 0 || fwrite(pcm, 1, pcm_size, file) == pcm_size);
    if (fclose(file) != 0) ok = false;
    if (!ok) remove(path);
    return ok;
}

/* Truncates to max_codepoints codepoints on a UTF-8 boundary. */
static void clamp_text(char *text, size_t capacity, size_t max_codepoints) {
    size_t codepoints = 0;
    size_t i = 0, keep = 0;
    while (i < capacity && text[i]) {
        unsigned char byte = (unsigned char)text[i];
        size_t width = byte < 0x80u ? 1u : byte >= 0xf0u ? 4u :
            byte >= 0xe0u ? 3u : byte >= 0xc0u ? 2u : 1u;
        if (i + width > capacity) break;
        if (++codepoints > max_codepoints) break;
        i += width;
        keep = i;
    }
    text[keep] = '\0';
}

static bool job_publish(BongoCatTts *tts, TtsJob *job, unsigned epoch,
    const char *text) {
    BongoCatVoice *voice = tts->voice;
    size_t count = bongo_cat_tts_envelope_count(job->pcm_size / 2u,
        tts->sample_rate);
    float *envelope = NULL;
    if (count) {
        envelope = malloc(count * sizeof(*envelope));
        if (!envelope) {
            job_fail(job, "TTS envelope allocation failed");
        } else {
            bongo_cat_tts_envelope_build((const int16_t *)job->pcm,
                job->pcm_size / 2u, tts->sample_rate, envelope, count);
        }
    }
    if (job->failed) {
        free(envelope);
        return false;
    }
    char path[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_path_join(path, sizeof(path),
        tts->voice->app->config_root, "tts-last.wav")) {
        free(envelope);
        job_fail(job, "TTS audio path unavailable");
        return false;
    }
    if (!write_wav(path, job->pcm, job->pcm_size, tts->sample_rate)) {
        free(envelope);
        job_fail(job, "TTS could not write audio file");
        return false;
    }
    SDL_LockMutex(voice->mutex);
    free(voice->tts_envelope);
    voice->tts_envelope = envelope;
    voice->tts_envelope_count = envelope ? count : 0;
    voice->tts_sample_rate = tts->sample_rate;
    snprintf(voice->tts_path, sizeof(voice->tts_path), "%s", path);
    SDL_UnlockMutex(voice->mutex);
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
        "TTS ready: %zu bytes of audio: %s", job->pcm_size, text);
    bongo_cat_voice_post_turn(voice, BONGO_CAT_VOICE_EV_TTS_READY, epoch);
    return true;
}

static void run_task(BongoCatTts *tts, const char *request_text,
    unsigned epoch) {
    char text[TTS_TEXT_CAP];
    SDL_strlcpy(text, request_text, sizeof(text));
    clamp_text(text, sizeof(text), TTS_TEXT_MAX_CODEPOINTS);
    TtsJob job = {0};
    job.tts = tts;
    char auth_header[TTS_API_KEY_CAP + 32];
    snprintf(auth_header, sizeof(auth_header), "Authorization: bearer %s",
        tts->api_key);
    const char *headers[] = {auth_header, NULL};
    BongoCatWsClient *client = bongo_cat_ws_connect(TTS_URL, headers,
        tts_on_text, tts_on_binary, tts_on_state, &job);
    SDL_memset(auth_header, 0, sizeof(auth_header));
    if (!client) {
        SDL_LogWarn(BONGO_CAT_LOG_LIFECYCLE, "TTS connect failed: %s",
            bongo_cat_ws_last_error(NULL));
        /* Without this event the service waits out its synthesis watchdog in
           silence, which reads as the pet refusing to answer. */
        bongo_cat_voice_post_turn(tts->voice, BONGO_CAT_VOICE_EV_TTS_ERROR,
            epoch);
        return;
    }
    BongoCatDashscopeTtsConfig config = {tts->model, tts->voice_name, "pcm",
        tts->sample_rate};
    char task_id[BONGO_CAT_DS_TASK_ID_CAP];
    char json[TTS_JSON_CAP];
    bongo_cat_dashscope_new_task_id(task_id, sizeof(task_id));
    Uint64 deadline = SDL_GetTicksNS() + TTS_HANDSHAKE_TIMEOUT_NS;
    bool sent_run = false, sent_text = false, sent_finish = false;
    while (!job.failed && !job.finished &&
        !SDL_GetAtomicInt(&tts->want_cancel) &&
        !SDL_GetAtomicInt(&tts->quit)) {
        if (bongo_cat_ws_connected(client)) {
            if (!sent_run) {
                if (!bongo_cat_dashscope_build_tts_run_task(json,
                    sizeof(json), task_id, &config) ||
                    !bongo_cat_ws_send_text(client, json)) {
                    job_fail(&job, "TTS request send failed");
                    break;
                }
                sent_run = true;
                deadline = SDL_GetTicksNS() + TTS_START_TIMEOUT_NS;
            } else if (job.started && !sent_text) {
                if (!bongo_cat_dashscope_build_tts_continue_task(json,
                    sizeof(json), task_id, text) ||
                    !bongo_cat_ws_send_text(client, json)) {
                    job_fail(&job, "TTS text send failed");
                    break;
                }
                sent_text = true;
            } else if (sent_text && !sent_finish) {
                if (!bongo_cat_dashscope_build_finish_task(json,
                    sizeof(json), task_id) ||
                    !bongo_cat_ws_send_text(client, json)) {
                    job_fail(&job, "TTS finish send failed");
                    break;
                }
                sent_finish = true;
                deadline = SDL_GetTicksNS() + TTS_FINISH_TIMEOUT_NS;
            } else if (!sent_finish && !job.started &&
                SDL_GetTicksNS() >= deadline) {
                job_fail(&job, "TTS start timeout");
                break;
            } else if (sent_finish && SDL_GetTicksNS() >= deadline) {
                job_fail(&job, "TTS receive timeout");
                break;
            }
        } else if (SDL_GetTicksNS() >= deadline) {
            job_fail(&job, "TTS connect timeout");
            break;
        }
        if (!bongo_cat_ws_run(client, 20)) break;
    }
    if (job.failed && !SDL_GetAtomicInt(&tts->want_cancel) &&
        !SDL_GetAtomicInt(&tts->quit))
        SDL_LogWarn(BONGO_CAT_LOG_LIFECYCLE, "%s", job.error);
    if (!job.failed && !job.finished &&
        !SDL_GetAtomicInt(&tts->want_cancel) &&
        !SDL_GetAtomicInt(&tts->quit))
        SDL_LogWarn(BONGO_CAT_LOG_LIFECYCLE, "TTS connection closed early");
    bongo_cat_ws_close(client);
    if (!SDL_GetAtomicInt(&tts->want_cancel) &&
        !SDL_GetAtomicInt(&tts->quit)) {
        bool ok = job.finished && job.pcm_size
            && job_publish(tts, &job, epoch, text);
        if (!ok) {
            if (job.finished && !job.pcm_size)
                SDL_LogWarn(BONGO_CAT_LOG_LIFECYCLE,
                    "TTS returned no audio bytes");
            bongo_cat_voice_post_turn(tts->voice,
                BONGO_CAT_VOICE_EV_TTS_ERROR, epoch);
        }
    }
    free(job.pcm);
}

static int worker_run(void *userdata) {
    BongoCatTts *tts = userdata;
    while (!SDL_GetAtomicInt(&tts->quit)) {
        char text[TTS_TEXT_CAP];
        unsigned epoch = 0;
        SDL_LockMutex(tts->job_mutex);
        bool ready = SDL_GetAtomicInt(&tts->want_speak) != 0;
        if (ready) {
            SDL_snprintf(text, sizeof(text), "%s", tts->pending_text);
            epoch = (unsigned)SDL_GetAtomicInt(&tts->epoch);
            SDL_SetAtomicInt(&tts->want_speak, 0);
            /* A cancel belongs to the request it interrupted. Kept armed past
               an idle worker it would abort the next answer before a single
               frame was sent, and that answer would never be reported back. */
            SDL_SetAtomicInt(&tts->want_cancel, 0);
        }
        SDL_UnlockMutex(tts->job_mutex);
        if (!ready) {
            SDL_Delay(20);
            continue;
        }
        run_task(tts, text, epoch);
    }
    return 0;
}

BongoCatTts *bongo_cat_tts_create(BongoCatVoice *voice,
    const BongoCatTtsConfig *config) {
    if (!voice || !config || !config->model || !config->model[0] ||
        !config->voice || !config->voice[0] || config->sample_rate <= 0)
        return NULL;
    if (!bongo_cat_ws_backend_available()) return NULL;
    BongoCatTts *tts = calloc(1, sizeof(*tts));
    if (!tts) return NULL;
    tts->voice = voice;
    SDL_strlcpy(tts->model, config->model, sizeof(tts->model));
    SDL_strlcpy(tts->voice_name, config->voice, sizeof(tts->voice_name));
    tts->sample_rate = config->sample_rate;
    tts->job_mutex = SDL_CreateMutex();
    if (!tts->job_mutex) {
        free(tts);
        return NULL;
    }
    return tts;
}

bool bongo_cat_tts_start(BongoCatTts *tts, const char *api_key) {
    if (!tts || !api_key || !api_key[0]) return false;
    if (SDL_GetAtomicInt(&tts->running)) return true;
    if (strlen(api_key) >= sizeof(tts->api_key)) return false;
    SDL_strlcpy(tts->api_key, api_key, sizeof(tts->api_key));
    tts->thread = SDL_CreateThread(worker_run, "bongo_tts", tts);
    if (!tts->thread) {
        tts->api_key[0] = '\0';
        return false;
    }
    SDL_SetAtomicInt(&tts->running, 1);
    return true;
}

bool bongo_cat_tts_speak(BongoCatTts *tts, const char *text,
    unsigned epoch) {
    if (!tts || !text || !text[0]) return false;
    /* The voice only ever wants the last word, so a queued or in-flight answer
       is replaced rather than refused: refusing left it silent with no event
       for the service to report. */
    SDL_LockMutex(tts->job_mutex);
    SDL_strlcpy(tts->pending_text, text, sizeof(tts->pending_text));
    SDL_SetAtomicInt(&tts->epoch, (int)epoch);
    SDL_SetAtomicInt(&tts->want_speak, 1);
    SDL_UnlockMutex(tts->job_mutex);
    return true;
}

void bongo_cat_tts_cancel(BongoCatTts *tts) {
    if (!tts) return;
    SDL_SetAtomicInt(&tts->want_cancel, 1);
}

bool bongo_cat_tts_running(const BongoCatTts *tts) {
    return tts && SDL_GetAtomicInt(
        (SDL_AtomicInt *)&tts->running) != 0;
}

void bongo_cat_tts_stop(BongoCatTts *tts) {
    if (!tts || !tts->thread) return;
    SDL_SetAtomicInt(&tts->want_cancel, 1);
    SDL_SetAtomicInt(&tts->quit, 1);
    SDL_WaitThread(tts->thread, NULL);
    tts->thread = NULL;
    SDL_SetAtomicInt(&tts->running, 0);
    SDL_SetAtomicInt(&tts->want_speak, 0);
    tts->api_key[0] = '\0';
}

void bongo_cat_tts_destroy(BongoCatTts *tts) {
    if (!tts) return;
    bongo_cat_tts_stop(tts);
    SDL_DestroyMutex(tts->job_mutex);
    free(tts);
}
