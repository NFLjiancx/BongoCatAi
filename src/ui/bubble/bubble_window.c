#include "bubble_internal.h"
#include "ui_backend.h"
#include "bongo_cat/log.h"
#include "bongo_cat/platform.h"

#include <SDL3/SDL_opengl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Vertex colors are packed as AARRGGBB (see bubble_paint.c vertex()). */
#define BUBBLE_COLOR_BG 0xD616182Eu
#define BUBBLE_COLOR_TEXT 0xFFE8ECF5u
#define BUBBLE_COLOR_SUB 0xFFA8B0BEu
#define BUBBLE_COLOR_ACCENT 0xFF68BCFFu
#define BUBBLE_COLOR_ERROR 0xFFFF7A7Au
#define BUBBLE_COLOR_BUTTON 0x1CFFFFFFu
#define BUBBLE_COLOR_TRACK 0x24FFFFFFu

static bool labels_changed(BongoCatBubble *b,
    const BongoCatBubbleLabels *labels) {
    bool changed = memcmp(b->labels.state_names, labels->state_names,
        sizeof(labels->state_names)) != 0 ||
        b->labels.cancel != labels->cancel ||
        b->labels.close != labels->close ||
        b->labels.utterances != labels->utterances;
    memcpy(b->labels.state_names, labels->state_names,
        sizeof(b->labels.state_names));
    b->labels.cancel = labels->cancel;
    b->labels.close = labels->close;
    b->labels.utterances = labels->utterances;
    return changed;
}

static bool status_changed(BongoCatBubble *b, const BongoCatVoiceStatus *s) {
    int level = (int)(fmaxf(0.0f, fminf(1.0f, s->level)) * 64.0f);
    int previous = (int)(fmaxf(0.0f, fminf(1.0f, b->status.level)) * 64.0f);
    bool changed = b->status.state != s->state || level != previous ||
        b->status.utterances != s->utterances ||
        strcmp(b->status.text, s->text) != 0 ||
        strcmp(b->status.error, s->error) != 0;
    b->status = *s;
    return changed;
}

/* Where the bubble belongs next to the pet, flipping to the left when there is
   no room on the right. */
static bool bubble_target(BongoCatBubble *b, int ox, int oy, int ow, int oh,
    int *out_x, int *out_y) {
    SDL_Point center = {ox + ow / 2, oy + oh / 2};
    SDL_Rect bounds;
    if (!SDL_GetDisplayUsableBounds(SDL_GetDisplayForPoint(&center),
        &bounds)) return false;
    int gap = 12;
    int x = ox + ow + gap;
    if (x + b->width > bounds.x + bounds.w) x = ox - b->width - gap;
    if (x < bounds.x) x = bounds.x + gap;
    *out_x = x;
    *out_y = SDL_clamp(oy, bounds.y,
        SDL_max(bounds.y, bounds.y + bounds.h - b->height));
    return true;
}

static void bubble_jump(BongoCatBubble *b, int ox, int oy, int ow, int oh) {
    int x = 0, y = 0;
    if (!bubble_target(b, ox, oy, ow, oh, &x, &y)) return;
    b->glide_dur_ns = 0;
    SDL_SetWindowPosition(b->window, x, y);
}

static void bubble_follow_now(BongoCatBubble *b, SDL_Window *owner) {
    int ox = 0, oy = 0, ow = 0, oh = 0;
    if (!owner || !SDL_GetWindowPosition(owner, &ox, &oy) ||
        !SDL_GetWindowSize(owner, &ow, &oh)) return;
    if (b->popup) return; /* The Wayland compositor anchors the popup. */
    b->owner_x = ox;
    b->owner_y = oy;
    b->owner_w = ow;
    b->owner_h = oh;
    b->follow_due_ns = 0;
    bubble_jump(b, ox, oy, ow, oh);
}

/* Chasing a drag two windows at a time is what turns the drag into visible
   stutter, so the bubble waits for the pet to land, then slides to its place. */
static void bubble_glide_start(BongoCatBubble *b, int ox, int oy, int ow,
    int oh, Uint64 now_ns) {
    int to_x = 0, to_y = 0, from_x = 0, from_y = 0;
    if (!bubble_target(b, ox, oy, ow, oh, &to_x, &to_y)) return;
    if (!SDL_GetWindowPosition(b->window, &from_x, &from_y)) return;
    int distance = SDL_abs(to_x - from_x) + SDL_abs(to_y - from_y);
    if (distance < 2) return;
    b->glide_from_x = from_x;
    b->glide_from_y = from_y;
    b->glide_to_x = to_x;
    b->glide_to_y = to_y;
    b->glide_start_ns = now_ns;
    b->glide_dur_ns = SDL_min(BUBBLE_GLIDE_MAX_NS, BUBBLE_GLIDE_MIN_NS
        + (Uint64)distance * (BUBBLE_GLIDE_MAX_NS - BUBBLE_GLIDE_MIN_NS)
        / 480ull);
}

static void bubble_glide_step(BongoCatBubble *b, Uint64 now_ns) {
    if (!b->glide_dur_ns) return;
    Uint64 elapsed = now_ns - b->glide_start_ns;
    if (elapsed >= b->glide_dur_ns) {
        b->glide_dur_ns = 0;
        SDL_SetWindowPosition(b->window, b->glide_to_x, b->glide_to_y);
        return;
    }
    /* Ease-out: leave fast, arrive soft, so the slide reads as settling rather
       than as a second window being dragged. */
    float t = (float)elapsed / (float)b->glide_dur_ns;
    t = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
    SDL_SetWindowPosition(b->window,
        (int)lroundf((float)b->glide_from_x
            + t * (float)(b->glide_to_x - b->glide_from_x)),
        (int)lroundf((float)b->glide_from_y
            + t * (float)(b->glide_to_y - b->glide_from_y)));
}

static void bubble_position(BongoCatBubble *b, SDL_Window *owner,
    Uint64 now_ns) {
    int ox = 0, oy = 0, ow = 0, oh = 0;
    if (!owner || !SDL_GetWindowPosition(owner, &ox, &oy) ||
        !SDL_GetWindowSize(owner, &ow, &oh)) return;
    if (b->popup) return; /* The Wayland compositor anchors the popup. */
    if (ox != b->owner_x || oy != b->owner_y || ow != b->owner_w ||
        oh != b->owner_h) {
        b->owner_x = ox;
        b->owner_y = oy;
        b->owner_w = ow;
        b->owner_h = oh;
        b->glide_dur_ns = 0;
        b->follow_due_ns = now_ns + BUBBLE_FOLLOW_NS;
        return;
    }
    if (!b->follow_due_ns || now_ns < b->follow_due_ns) return;
    b->follow_due_ns = 0;
    bubble_glide_start(b, ox, oy, ow, oh, now_ns);
}

static bool bubble_create_window(BongoCatBubble *b, SDL_Window *owner) {
    float layout = 1.0f;
    bongo_cat_ui_query_window_scale(owner, &layout, NULL);
    if (layout < 0.5f) layout = 1.0f;
    int width = (int)((float)BUBBLE_WIDTH * layout);
    int height = (int)((float)BUBBLE_HEIGHT * layout);
    if (width < 1 || height < 1) return false;
    SDL_WindowFlags flags = SDL_WINDOW_OPENGL | SDL_WINDOW_TRANSPARENT |
        SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN;
    const char *driver = SDL_GetCurrentVideoDriver();
    b->popup = driver && SDL_strcmp(driver, "wayland") == 0;
    SDL_GLContext previous_context = SDL_GL_GetCurrentContext();
    SDL_Window *previous_window = SDL_GL_GetCurrentWindow();
    int ox = 0, oy = 0, ow = 0, oh = 0;
    SDL_GetWindowPosition(owner, &ox, &oy);
    SDL_GetWindowSize(owner, &ow, &oh);
    if (b->popup) {
        b->window = SDL_CreatePopupWindow(owner, ow + 12, 0, width, height,
            flags);
    } else {
        /* Not focusable: mapping a plain utility window makes the compositor
           treat it as a program asking for the keyboard, which both steals
           focus from the user and flags the taskbar entry. Mouse hits, which
           the [Cancel] button needs, do not require focus. */
        b->window = SDL_CreateWindow("BongoCat - Voice", width, height,
            flags | SDL_WINDOW_BORDERLESS | SDL_WINDOW_UTILITY |
            SDL_WINDOW_ALWAYS_ON_TOP | SDL_WINDOW_NOT_FOCUSABLE);
    }
    if (!b->window) return false;
    b->window_id = SDL_GetWindowID(b->window);
    int sharing = 0;
    SDL_GL_GetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, &sharing);
    if (!SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0)) {
        SDL_DestroyWindow(b->window);
        b->window = NULL;
        return false;
    }
    b->context = SDL_GL_CreateContext(b->window);
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, sharing);
    if (!b->context || !SDL_GL_MakeCurrent(b->window, b->context)) {
        if (b->context) SDL_GL_DestroyContext(b->context);
        SDL_DestroyWindow(b->window);
        b->window = NULL;
        b->context = NULL;
        if (previous_window && previous_context)
            SDL_GL_MakeCurrent(previous_window, previous_context);
        return false;
    }
    SDL_GL_SetSwapInterval(0);
    b->width = BUBBLE_WIDTH;
    b->height = BUBBLE_HEIGHT;
    b->scale = (float)width / (float)BUBBLE_WIDTH;
    int pixel_width = width, pixel_height = height;
    SDL_GetWindowSizeInPixels(b->window, &pixel_width, &pixel_height);
    b->pixel_width = pixel_width > 0 ? pixel_width : width;
    b->pixel_height = pixel_height > 0 ? pixel_height : height;
    b->raster_scale = (float)b->pixel_width / (float)width;
    bool ok = bubble_paint_init(b);
    if (previous_window && previous_context)
        SDL_GL_MakeCurrent(previous_window, previous_context);
    else SDL_GL_MakeCurrent(NULL, NULL);
    if (!ok) return false;
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
        "Voice bubble window created: driver=%s popup=%d scale=%.2f "
        "raster=%.2f", driver ? driver : "unknown", b->popup, b->scale,
        b->raster_scale);
    return true;
}

static void bubble_show(BongoCatBubble *b, SDL_Window *owner,
    Uint64 now_ns) {
    if (!SDL_ShowWindow(b->window)) return;
    b->raise_due_ns = now_ns + BUBBLE_RAISE_NS;
    /* Mutter only honours the pin once the window is mapped, and it drops the
       above-state again when the window is unmapped for the next hide. */
    if (!b->popup) bongo_cat_platform_pin_above(b->window);
    b->visible = true;
    bubble_follow_now(b, owner);
    b->dirty = true;
}

static void bubble_hide(BongoCatBubble *b) {
    if (!b->visible) return;
    SDL_HideWindow(b->window);
    b->visible = false;
}

static void bubble_paint(BongoCatBubble *b) {
    SDL_Window *previous_window = SDL_GL_GetCurrentWindow();
    SDL_GLContext previous_context = SDL_GL_GetCurrentContext();
    if (!SDL_GL_MakeCurrent(b->window, b->context)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
            "Voice bubble context activation failed: %s", SDL_GetError());
        return;
    }
    if (b->runes_new) bubble_fonts_rebuild(b);
    if (!bubble_paint_frame(b))
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
            "Voice bubble render failed: %s", SDL_GetError());
    if (previous_window && previous_context)
        SDL_GL_MakeCurrent(previous_window, previous_context);
    else SDL_GL_MakeCurrent(NULL, NULL);
}

void bongo_cat_bubble_scene(BongoCatBubble *b) {
    const BongoCatVoiceStatus *s = &b->status;
    bubble_round_rect(b, 0, 0, (float)BUBBLE_WIDTH, (float)BUBBLE_HEIGHT,
        14.0f, BUBBLE_COLOR_BG);
    BongoCatVoiceState state = s->state < BONGO_CAT_VOICE_STATE_COUNT
        ? s->state : BONGO_CAT_VOICE_STATE_OFF;
    const char *name = b->labels.state_names[state];
    if (!name || !name[0]) name = b->labels.state_names[0];
    uint32_t name_color = state == BONGO_CAT_VOICE_STATE_ERROR_HOLD
        ? BUBBLE_COLOR_ERROR : BUBBLE_COLOR_TEXT;
    if (name) bubble_text(b, name, 16.0f, 27.0f, 20.0f, name_color);
    float right = (float)BUBBLE_WIDTH - 16.0f;
    float close_width = b->labels.close
        ? bubble_text_width(b, b->labels.close, 16.0f) + 22.0f : 64.0f;
    float cancel_width = b->labels.cancel
        ? bubble_text_width(b, b->labels.cancel, 16.0f) + 22.0f : 64.0f;
    float close_x = right - close_width;
    float cancel_x = close_x - 8.0f - cancel_width;
    b->close_rect[0] = close_x;
    b->close_rect[1] = 13.0f;
    b->close_rect[2] = close_width;
    b->close_rect[3] = 27.0f;
    b->cancel_rect[0] = cancel_x;
    b->cancel_rect[1] = 13.0f;
    b->cancel_rect[2] = cancel_width;
    b->cancel_rect[3] = 27.0f;
    struct {
        const float *rect;
        const char *label;
    } buttons[2] = {{b->cancel_rect, b->labels.cancel},
        {b->close_rect, b->labels.close}};
    for (int i = 0; i < 2; ++i) {
        if (!buttons[i].label) continue;
        bubble_round_rect(b, buttons[i].rect[0], buttons[i].rect[1],
            buttons[i].rect[2], buttons[i].rect[3], 8.0f, BUBBLE_COLOR_BUTTON);
        float text_width = bubble_text_width(b, buttons[i].label, 16.0f);
        bubble_text(b, buttons[i].label,
            buttons[i].rect[0] + (buttons[i].rect[2] - text_width) / 2.0f,
            buttons[i].rect[1] + buttons[i].rect[3] / 2.0f, 16.0f,
            BUBBLE_COLOR_SUB);
    }
    float level = s->level;
    if (level < 0.0f) level = 0.0f;
    if (level > 1.0f) level = 1.0f;
    bubble_round_rect(b, 16.0f, 48.0f, 348.0f, 5.0f, 2.5f, BUBBLE_COLOR_TRACK);
    if (level > 0.02f)
        bubble_round_rect(b, 16.0f, 48.0f, 348.0f * level, 5.0f, 2.5f,
            BUBBLE_COLOR_ACCENT);
    char lines[BUBBLE_BODY_LINES][BUBBLE_LINE_CAP];
    int count = 0;
    const char *body = s->error[0] ? s->error : s->text;
    uint32_t body_color = s->error[0] ? BUBBLE_COLOR_ERROR
        : BUBBLE_COLOR_SUB;
    char counter[BUBBLE_LINE_CAP];
    if (!body[0] && s->utterances >= 0 && b->labels.utterances) {
        snprintf(counter, sizeof(counter), "%s %d", b->labels.utterances,
            s->utterances);
        body = counter;
    }
    if (body[0])
        count = bongo_cat_bubble_wrap(b, body, 348.0f, 20.0f, lines,
            BUBBLE_BODY_LINES);
    for (int i = 0; i < count; ++i)
        bubble_text(b, lines[i], 16.0f, 70.0f + (float)i * 26.0f, 20.0f,
            body_color);
}

BongoCatBubble *bongo_cat_bubble_create(void) {
    BongoCatBubble *b = calloc(1, sizeof(*b));
    if (!b) return NULL;
    b->used_runes = calloc(BUBBLE_RUNE_SPACE, 1);
    if (!b->used_runes) {
        free(b);
        return NULL;
    }
    for (int i = 32; i < 127; ++i)
        b->used_runes[i >> 3] |= (uint8_t)(1u << (i & 7));
    b->owner_x = b->owner_y = -1;
    return b;
}

void bongo_cat_bubble_destroy(BongoCatBubble *b) {
    if (!b) return;
    if (b->window && b->context) {
        SDL_Window *previous_window = SDL_GL_GetCurrentWindow();
        SDL_GLContext previous_context = SDL_GL_GetCurrentContext();
        if (SDL_GetWindowFromID(b->window_id) &&
            SDL_GL_MakeCurrent(b->window, b->context)) {
            bubble_paint_free(b);
            if (previous_window && previous_context)
                SDL_GL_MakeCurrent(previous_window, previous_context);
        } else if (b->paint.atlas.permanent.alloc) {
            nk_font_atlas_clear(&b->paint.atlas);
            free(b->paint.vertices);
            b->paint.vertices = NULL;
        }
        if (SDL_GetWindowFromID(b->window_id)) {
            if (b->context) SDL_GL_DestroyContext(b->context);
            SDL_DestroyWindow(b->window);
        }
    }
    for (int i = 0; i < 4; ++i) free(b->paint.ranges[i]);
    free(b->used_runes);
    free(b);
}

bool bongo_cat_bubble_event(BongoCatBubble *b, const SDL_Event *event) {
    if (!b || !b->window || !event) return false;
    if (SDL_GetWindowFromEvent(event) != b->window) return false;
    if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
        event->button.button == SDL_BUTTON_LEFT && b->visible) {
        float x = event->button.x, y = event->button.y;
        if (b->close_rect[2] > 0.0f && x >= b->close_rect[0] &&
            x <= b->close_rect[0] + b->close_rect[2] &&
            y >= b->close_rect[1] && y <= b->close_rect[1] +
            b->close_rect[3]) b->user_closed = true;
        else if (b->cancel_rect[2] > 0.0f && x >= b->cancel_rect[0] &&
            x <= b->cancel_rect[0] + b->cancel_rect[2] &&
            y >= b->cancel_rect[1] && y <= b->cancel_rect[1] +
            b->cancel_rect[3]) b->want_cancel = true;
    }
    return true; /* Consume every event addressed to the bubble. */
}

bool bongo_cat_bubble_take_cancel_request(BongoCatBubble *b) {
    if (!b || !b->want_cancel) return false;
    b->want_cancel = false;
    return true;
}

void bongo_cat_bubble_update(BongoCatBubble *b, SDL_Window *owner,
    const BongoCatVoiceStatus *status, const BongoCatBubbleLabels *labels,
    Uint64 now_ns) {
    if (!b || !status || !labels) return;
    /* A gate the room noise opened has nothing to show, and showing it is how a
       loud room keeps the bubble up forever: listening and recognizing only
       count once the recognizer has actually returned words. */
    bool listening = status->state == BONGO_CAT_VOICE_STATE_LISTENING;
    bool waiting = listening ||
        status->state == BONGO_CAT_VOICE_STATE_RECOGNIZING;
    if (listening) b->user_closed = false;
    bool engaging = status->state >= BONGO_CAT_VOICE_STATE_LISTENING &&
        status->state <= BONGO_CAT_VOICE_STATE_ERROR_HOLD &&
        (!waiting || status->text[0] != '\0');
    if (engaging) b->hide_after_ns = now_ns + BUBBLE_LINGER_NS;
    bool owner_visible = owner &&
        (SDL_GetWindowFlags(owner) & SDL_WINDOW_HIDDEN) == 0 &&
        (SDL_GetWindowFlags(owner) & SDL_WINDOW_MINIMIZED) == 0;
    bool linger = b->visible && !engaging && now_ns < b->hide_after_ns;
    bool want = (engaging || linger) && !b->user_closed && owner_visible;
    if (want && !b->window && now_ns >= b->create_retry_ns) {
        if (!bubble_create_window(b, owner)) {
            b->create_retry_ns = now_ns + 30ull * 1000000000ull;
            SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
                "Voice bubble window could not be created: %s",
                SDL_GetError());
        }
    }
    if (!want) {
        if (b->window) bubble_hide(b);
        return;
    }
    if (!b->window) return; /* Creation deferred until the retry deadline. */
    if (labels_changed(b, labels)) b->dirty = true;
    if (status_changed(b, status)) b->dirty = true;
    if (b->dirty || !b->visible) bubble_fonts_collect(b);
    if (!b->visible) {
        bubble_show(b, owner, now_ns);
        return; /* Paint below with the fresh content. */
    }
    if (!b->popup && now_ns >= b->raise_due_ns) {
        /* A borderless utility window loses its above-state whenever another
           window is raised or focus moves, so keep pinning it like the pet. */
        b->raise_due_ns = now_ns + BUBBLE_RAISE_NS;
        bongo_cat_platform_pin_above(b->window);
    }
    bubble_glide_step(b, now_ns);
    if (now_ns >= b->layout_check_ns) {
        b->layout_check_ns = now_ns + BUBBLE_LAYOUT_CHECK_NS;
        bubble_position(b, owner, now_ns);
        int pixel_width = 0, pixel_height = 0;
        if (SDL_GetWindowSizeInPixels(b->window, &pixel_width,
            &pixel_height) && pixel_width > 0 &&
            pixel_width != b->pixel_width) {
            b->pixel_width = pixel_width;
            b->pixel_height = pixel_height;
            b->raster_scale = (float)pixel_width /
                ((float)BUBBLE_WIDTH * b->scale);
            b->runes_new = true; /* Rebake at the new pixel density. */
            b->dirty = true;
        }
    }
    if (!b->dirty || now_ns < b->paint_due_ns) return;
    b->paint_due_ns = now_ns + BUBBLE_PAINT_MIN_NS;
    bubble_paint(b);
}
