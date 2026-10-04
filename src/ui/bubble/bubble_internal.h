#ifndef BONGO_CAT_BUBBLE_INTERNAL_H
#define BONGO_CAT_BUBBLE_INTERNAL_H

#include "bubble_window.h"
#include "bongo_cat/gl_api.h"
#include "nuklear_config.h"

#include <SDL3/SDL.h>

#define BUBBLE_WIDTH 380
#define BUBBLE_HEIGHT 176
#define BUBBLE_FONT_CAP 2500
#define BUBBLE_RUNE_SPACE (0x110000u / 8u)
#define BUBBLE_LINGER_NS (6ull * 1000000000ull)
#define BUBBLE_PAINT_MIN_NS (40ull * 1000000ull)
#define BUBBLE_LAYOUT_CHECK_NS (200ull * 1000000ull)
#define BUBBLE_RAISE_NS (1000ull * 1000000ull)
/* Following the pet mid-drag is a moving target the compositor cannot keep up
   with, so the bubble waits for the pet to land, then glides over. */
#define BUBBLE_FOLLOW_NS (220ull * 1000000ull)
#define BUBBLE_GLIDE_MIN_NS (120ull * 1000000ull)
#define BUBBLE_GLIDE_MAX_NS (280ull * 1000000ull)
#define BUBBLE_BODY_LINES 4
#define BUBBLE_LINE_CAP 512

typedef struct BubbleVertex {
    float x, y, u, v;
    uint8_t r, g, b, a;
} BubbleVertex;

typedef struct BubbleBatch { GLuint texture; int first, count; } BubbleBatch;

typedef struct BubblePaint {
    BongoCatGL gl;
    GLuint program, vao, vbo, white, font_texture;
    GLint projection, sampler;
    BubbleVertex *vertices;
    size_t count, capacity;
    BubbleBatch batches[128];
    int batch_count;
    struct nk_font_atlas atlas;
    struct nk_font *font;
    nk_rune *ranges[4];
    int font_width, font_height;
    bool failed;
} BubblePaint;

typedef struct BongoCatBubble {
    SDL_Window *window;
    SDL_GLContext context;
    SDL_WindowID window_id;
    bool popup;          /* Wayland keeps the bubble as an owner popup. */
    bool visible;
    bool user_closed;    /* [Close] hides until the next LISTENING onset. */
    bool want_cancel;
    bool dirty;
    bool fonts_ready;
    Uint64 create_retry_ns;
    int width, height, pixel_width, pixel_height;
    float scale, raster_scale;
    int owner_x, owner_y, owner_w, owner_h;
    Uint64 follow_due_ns; /* When the pet stopped moving; 0 while dragging. */
    int glide_from_x, glide_from_y, glide_to_x, glide_to_y;
    Uint64 glide_start_ns;
    Uint64 glide_dur_ns; /* 0 once the bubble has landed. */
    Uint64 hide_after_ns;
    Uint64 paint_due_ns;
    Uint64 layout_check_ns;
    Uint64 raise_due_ns; /* Re-asserts the above-state on X11. */
    BongoCatVoiceStatus status;
    BongoCatBubbleLabels labels;
    float cancel_rect[4], close_rect[4]; /* Logical window-space hit rects. */
    BubblePaint paint;
    uint8_t *used_runes; /* Accumulated codepoint bitmap across rebuilds. */
    bool runes_new;
} BongoCatBubble;

void bubble_quad(BongoCatBubble *b, GLuint texture, float x, float y,
    float w, float h, float u0, float v0, float u1, float v1, uint32_t color);
void bubble_rect(BongoCatBubble *b, float x, float y, float w, float h,
    uint32_t color);
void bubble_round_rect(BongoCatBubble *b, float x, float y, float w, float h,
    float radius, uint32_t color);
bool bubble_paint_init(BongoCatBubble *b);
bool bubble_paint_frame(BongoCatBubble *b);
void bubble_paint_free(BongoCatBubble *b);
void bongo_cat_bubble_scene(BongoCatBubble *b);

/* Atlas pipeline (bubble_fonts.c). */
bool bubble_fonts_collect(BongoCatBubble *b); /* Marks runes_new on additions. */
bool bubble_fonts_rebuild(BongoCatBubble *b);
float bubble_text_width(BongoCatBubble *b, const char *text, float size);
void bubble_text(BongoCatBubble *b, const char *text, float x, float y,
    float size, uint32_t color);
int bongo_cat_bubble_wrap(BongoCatBubble *b, const char *text, float width,
    float size, char lines[][BUBBLE_LINE_CAP], int max_lines);

#endif
