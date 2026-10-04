#include "preferences_state.h"
#include "preferences_widgets.h"
#include "preferences_widgets_internal.h"
#include "preferences_notice.h"
#include "preferences_text_edit.h"
#include "bongo_cat/ai_key_store.h"
#include "bongo_cat/i18n.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

enum {
    AI_FIELD_KEY = 0,
    AI_FIELD_ASR,
    AI_FIELD_TTS_MODEL,
    AI_FIELD_TTS_VOICE,
    AI_FIELD_ASSISTANT_MODEL
};

static const char *tr(BongoCatPreferences *value, const char *key,
    const char *fallback) {
    return bongo_cat_i18n_get(value->app->i18n, key, fallback);
}

static void ai_edit_begin(BongoCatPreferences *value, int field,
    const char *id, const char *current, struct nk_rect bounds) {
    bongo_cat_preferences_shortcut_cancel(value);
    value->ai_edit_field = field;
    bongo_cat_preferences_text_session_begin(&value->ai_edit, id, current,
        bounds);
    SDL_StartTextInput(value->window);
    value->render_dirty = true;
}

static bool ai_edit_target(BongoCatApp *app, int field, char **target,
    size_t *capacity) {
    switch (field) {
    case AI_FIELD_ASR: *target = app->settings.ai.asr_model;
        *capacity = sizeof(app->settings.ai.asr_model); break;
    case AI_FIELD_TTS_MODEL: *target = app->settings.ai.tts_model;
        *capacity = sizeof(app->settings.ai.tts_model); break;
    case AI_FIELD_TTS_VOICE: *target = app->settings.ai.tts_voice;
        *capacity = sizeof(app->settings.ai.tts_voice); break;
    case AI_FIELD_ASSISTANT_MODEL: *target = app->settings.ai.assistant_model;
        *capacity = sizeof(app->settings.ai.assistant_model); break;
    default: return false;
    }
    return true;
}

void bongo_cat_preferences_ai_edit_finish(BongoCatPreferences *value,
    bool save) {
    if (!value || !bongo_cat_preferences_text_session_active(&value->ai_edit))
        return;
    BongoCatPreferencesTextSession *session = &value->ai_edit;
    int field = value->ai_edit_field;
    bool reload = false;
    if (save) {
        bongo_cat_text_edit_trim(session->text);
        if (field == AI_FIELD_KEY) {
            if (bongo_cat_ai_key_store(value->app->config_root, session->text))
                value->app->voice_reload_requested = true;
        } else {
            char *target = NULL;
            size_t capacity = 0;
            if (ai_edit_target(value->app, field, &target, &capacity)) {
                reload = strcmp(target, session->text) != 0;
                snprintf(target, capacity, "%s", session->text);
                bongo_cat_settings_validate(&value->app->settings);
                if (reload) value->app->voice_reload_requested = true;
            }
        }
    }
    memset(session, 0, sizeof(*session));
    value->ai_edit_field = -1;
    SDL_StopTextInput(value->window);
    if (reload) bongo_cat_preferences_reload_fonts(value);
    value->render_dirty = true;
}

bool bongo_cat_preferences_ai_edit_event(BongoCatPreferences *value,
    const SDL_Event *event) {
    if (!value) return false;
    BongoCatPreferencesTextSessionEvent result =
        bongo_cat_preferences_text_session_event(&value->ai_edit,
            event, value->ui.layout_scale, value->ui.label_font, 5.0f);
    if (result.finish) bongo_cat_preferences_ai_edit_finish(value, result.save);
    if (result.handled) value->render_dirty = true;
    return result.handled;
}

static void ai_field_row(BongoCatPreferences *value,
    struct nk_context *context, int field, const char *id,
    BongoCatPrefIcon icon, const char *title, const char *detail,
    const char *shown_stored, const char *edit_seed) {
    FormStyle saved;
    int lines = bongo_cat_pref_detail_lines(context, detail);
    bongo_cat_pref_row_icon(context, icon);
    if (!bongo_cat_pref_form_begin(context, id, lines, &saved)) return;
    bongo_cat_pref_form_title_sized(context, title, 220.0f);
    struct nk_rect bounds;
    if (nk_widget(&bounds, context) == NK_WIDGET_INVALID) {
        nk_layout_row_end(context);
        bongo_cat_pref_description(context, detail, lines);
        bongo_cat_pref_form_end(context, &saved);
        return;
    }
    BongoCatUIPalette p = bongo_cat_ui_palette(bongo_cat_ui_dark(context));
    struct nk_command_buffer *canvas = nk_window_get_canvas(context);
    BongoCatPreferencesTextSession *session = &value->ai_edit;
    bool editing = value->ai_edit_field == field &&
        bongo_cat_preferences_text_session_active(session);
    if (editing) session->bounds = bounds;
    const char *shown = editing ? session->text : shown_stored;
    nk_fill_rect(canvas, bounds, 10, editing ? p.hover : p.field);
    nk_stroke_rect(canvas, bounds, 10, 1,
        editing ? p.pink : p.border_subtle);
    const struct nk_user_font *font = value->ui.label_font;
    struct nk_rect text = nk_rect(bounds.x + 5,
        bounds.y + (bounds.h - font->height) * .5f,
        NK_MAX(1.0f, bounds.w - 10), font->height);
    if (editing && session->select_all)
        nk_fill_rect(canvas, text, 4, p.selection);
    nk_draw_text(canvas, text, shown, nk_strlen(shown), font,
        nk_rgba(0, 0, 0, 0), editing ? p.pink : p.text);
    if (editing) {
        float caret = font->width(font->userdata, font->height, shown,
            (int)session->cursor);
        caret = NK_MIN(caret, bounds.w - 12);
        if (!session->select_all)
            nk_stroke_line(canvas, bounds.x + 5 + caret, bounds.y + 3,
                bounds.x + 5 + caret, bounds.y + bounds.h - 3, 1, p.pink);
    } else {
        bool hover = nk_input_is_mouse_hovering_rect(&context->input, bounds);
        bool clicked = hover && nk_input_is_mouse_click_in_rect(
            &context->input, NK_BUTTON_LEFT, bounds);
        if (hover) bongo_cat_ui_cursor_hover_rect(context, bounds,
            BONGO_CAT_UI_CURSOR_TEXT);
        if (clicked) ai_edit_begin(value, field, id, edit_seed, bounds);
    }
    nk_layout_row_end(context);
    bongo_cat_pref_description(context, detail, lines);
    bongo_cat_pref_form_end(context, &saved);
}

void bongo_cat_preferences_page_ai(BongoCatPreferences *value,
    struct nk_context *context) {
    BongoCatApp *app = value->app;
    BongoCatAiPreferences *ai = &app->settings.ai;
    bongo_cat_pref_section_icon(context, tr(value,
        "pages.preference.ai.title", "AI"),
        BONGO_CAT_PREF_ICON_SECTION_APPLICATION);
    bool consent = ai->privacy_consent;
    bongo_cat_pref_row_icon(context, BONGO_CAT_PREF_ICON_ADMINISTRATOR);
    if (bongo_cat_pref_toggle(context, "ai-consent", tr(value,
        "pages.preference.ai.labels.consent", "Voice Privacy Consent"), tr(value,
        "pages.preference.ai.hints.consent",
        "Voice data is sent to external AI services."), &consent)) {
        ai->privacy_consent = consent;
        if (!consent) ai->enabled = false;
    }
    bool enabled = ai->enabled;
    bongo_cat_pref_row_icon(context, BONGO_CAT_PREF_ICON_AUTOSTART);
    if (bongo_cat_pref_toggle(context, "ai-assistant", tr(value,
        "pages.preference.ai.labels.assistant", "Voice Assistant"), tr(value,
        "pages.preference.ai.hints.assistant",
        "Enable the voice assistant pipeline."), &enabled)) {
        if (enabled && !ai->privacy_consent) {
            bongo_cat_preferences_notice_show(app, tr(value,
                "pages.preference.ai.hints.needConsent",
                "Accept the privacy notice first."), true);
        } else {
            ai->enabled = enabled;
            bongo_cat_preferences_notice_show(app, tr(value,
                "pages.preference.ai.hints.restart",
                "Restart required."), false);
        }
    }
    bool key_present = bongo_cat_ai_key_present(app->config_root);
    ai_field_row(value, context, AI_FIELD_KEY, "ai-api-key",
        BONGO_CAT_PREF_ICON_LANGUAGE, tr(value,
            "pages.preference.ai.labels.apiKey", "API Key"), tr(value,
            "pages.preference.ai.hints.keyHint",
            "Stored in the configuration folder."),
        key_present ? tr(value, "pages.preference.ai.hints.keySet",
            "Key saved") : tr(value, "pages.preference.ai.hints.keyMissing",
            "No key"), "");
    ai_field_row(value, context, AI_FIELD_ASR, "ai-asr-model",
        BONGO_CAT_PREF_ICON_SECTION_MODEL, tr(value,
            "pages.preference.ai.labels.asrModel", "Speech Recognition"),
        tr(value, "pages.preference.ai.hints.asrModel",
            "Speech recognition model."), ai->asr_model, ai->asr_model);
    ai_field_row(value, context, AI_FIELD_TTS_MODEL, "ai-tts-model",
        BONGO_CAT_PREF_ICON_SECTION_MODEL, tr(value,
            "pages.preference.ai.labels.ttsModel", "Speech Synthesis Model"),
        tr(value, "pages.preference.ai.hints.ttsModel",
            "Text to speech model."), ai->tts_model, ai->tts_model);
    ai_field_row(value, context, AI_FIELD_TTS_VOICE, "ai-tts-voice",
        BONGO_CAT_PREF_ICON_SECTION_MODEL, tr(value,
            "pages.preference.ai.labels.ttsVoice", "Speech Voice"),
        tr(value, "pages.preference.ai.hints.ttsVoice",
            "Text to speech voice."), ai->tts_voice, ai->tts_voice);
    ai_field_row(value, context, AI_FIELD_ASSISTANT_MODEL, "ai-model",
        BONGO_CAT_PREF_ICON_SECTION_MODEL, tr(value,
            "pages.preference.ai.labels.model", "Assistant Model"),
        tr(value, "pages.preference.ai.hints.model",
            "Chat model used by the assistant."), ai->assistant_model,
        ai->assistant_model);
}
