#include "voice_internal.h"
#include "asr_worker.h"
#include "tts_worker.h"
#include "bongo_cat/log.h"

#include <string.h>

/* Builds the three DashScope-backed workers from the settings on disk and the
   resolved API key. Runs at startup and again whenever the AI page saves new
   values, so a key or model change needs no restart. Missing pieces are simply
   left NULL and the states that need them stay unavailable. */
void bongo_cat_voice_spawn_workers(BongoCatVoice *voice) {
    BongoCatApp *app = voice->app;
    char api_key[512];
    api_key[0] = '\0';
    if (bongo_cat_voice_resolve_api_key(voice, api_key, sizeof(api_key))) {
        const char *asr_model = app->settings.ai.asr_model[0]
            ? app->settings.ai.asr_model : BONGO_CAT_AI_DEFAULT_ASR_MODEL;
        BongoCatDashscopeAsrConfig config = {asr_model, "pcm",
            16000, NULL, 0, false, false};
        voice->asr = bongo_cat_asr_create(voice, &config);
        if (voice->asr && !bongo_cat_asr_start(voice->asr, api_key)) {
            bongo_cat_asr_destroy(voice->asr);
            voice->asr = NULL;
        }
        const char *env_model = SDL_getenv("BONGO_CAT_TTS_MODEL");
        const char *env_voice = SDL_getenv("BONGO_CAT_TTS_VOICE");
        const char *tts_model = env_model && env_model[0] ? env_model :
            app->settings.ai.tts_model[0] ? app->settings.ai.tts_model
                : BONGO_CAT_AI_DEFAULT_TTS_MODEL;
        const char *tts_voice = env_voice && env_voice[0] ? env_voice :
            app->settings.ai.tts_voice[0] ? app->settings.ai.tts_voice
                : BONGO_CAT_AI_DEFAULT_TTS_VOICE;
        BongoCatTtsConfig tts_config = {tts_model, tts_voice, 24000};
        voice->tts = bongo_cat_tts_create(voice, &tts_config);
        if (voice->tts && !bongo_cat_tts_start(voice->tts, api_key)) {
            bongo_cat_tts_destroy(voice->tts);
            voice->tts = NULL;
        }
    }
    SDL_memset(api_key, 0, sizeof(api_key));
    const char *env_model = SDL_getenv("BONGO_CAT_ASSISTANT_MODEL");
    const char *assistant_model = env_model && env_model[0] ? env_model :
        app->settings.ai.assistant_model;
    voice->assistant = bongo_cat_assistant_create(voice, assistant_model);
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
        "Voice assistant initialized: asr=%s agent=%s tts=%s",
        voice->asr ? "on" : "off", voice->assistant ? "on" : "off",
        voice->tts ? "on" : "off");
}

/* Cancels and joins every worker; safe when none were started. */
void bongo_cat_voice_kill_workers(BongoCatVoice *voice) {
    if (voice->tts) {
        bongo_cat_tts_stop(voice->tts);
        bongo_cat_tts_destroy(voice->tts);
        voice->tts = NULL;
    }
    if (voice->assistant) {
        bongo_cat_assistant_stop(voice->assistant);
        bongo_cat_assistant_destroy(voice->assistant);
        voice->assistant = NULL;
    }
    if (voice->asr) {
        bongo_cat_asr_stop(voice->asr);
        bongo_cat_asr_destroy(voice->asr);
        voice->asr = NULL;
    }
}
