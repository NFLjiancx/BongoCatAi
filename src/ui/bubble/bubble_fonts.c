#include "bubble_internal.h"
#include "ui_font.h"
#include "ui_font_atlas_internal.h"

#include <SDL3/SDL_opengl.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void mark(BongoCatBubble *b, nk_rune rune) {
    if (rune < 32 || rune >= 0x110000) return;
    uint8_t *slot = b->used_runes + (rune >> 3);
    uint8_t bit = (uint8_t)(1u << (rune & 7));
    if ((*slot & bit) == 0) {
        *slot |= bit;
        b->runes_new = true;
    }
}

static void collect_text(BongoCatBubble *b, const char *s) {
    if (!s) return;
    int length = (int)strlen(s);
    while (length > 0) {
        nk_rune rune;
        int n = nk_utf_decode(s, &rune, length);
        if (n <= 0) break;
        mark(b, rune);
        s += n;
        length -= n;
    }
}

bool bubble_fonts_collect(BongoCatBubble *b) {
    if (!b->used_runes) return false;
    collect_text(b, "\xe2\x80\xa6"); /* Ellipsis used by line truncation. */
    for (int i = 0; i < BONGO_CAT_VOICE_STATE_COUNT; ++i)
        collect_text(b, b->labels.state_names[i]);
    collect_text(b, b->labels.cancel);
    collect_text(b, b->labels.close);
    collect_text(b, b->labels.utterances);
    collect_text(b, b->status.text);
    collect_text(b, b->status.error);
    return true;
}

static nk_rune *ranges_from_bitmap(const uint8_t *used) {
    size_t count = 0;
    for (nk_rune i = 32; i < 0x110000 && count < BUBBLE_FONT_CAP; ++i)
        if (used[i >> 3] & (uint8_t)(1u << (i & 7)))
            ++count;
    if (!count) return NULL;
    nk_rune *result = calloc(count * 2 + 1, sizeof(*result));
    if (!result) return NULL;
    size_t out = 0;
    for (nk_rune i = 32; i < 0x110000 && out < count * 2; ++i) {
        if (!(used[i >> 3] & (uint8_t)(1u << (i & 7)))) continue;
        result[out++] = i;
        while (i + 1 < 0x110000 && out < count * 2 &&
            (used[(i + 1) >> 3] & (uint8_t)(1u << ((i + 1) & 7))))
            ++i;
        result[out++] = i;
    }
    return result;
}

bool bubble_fonts_rebuild(BongoCatBubble *b) {
    BubblePaint *p = &b->paint;
    if (!b->used_runes) return false;
    if (p->atlas.permanent.alloc) nk_font_atlas_clear(&p->atlas);
    if (p->font_texture) {
        glDeleteTextures(1, &p->font_texture);
        p->font_texture = 0;
    }
    for (int i = 0; i < 4; ++i) {
        free(p->ranges[i]);
        p->ranges[i] = NULL;
    }
    p->ranges[0] = ranges_from_bitmap(b->used_runes);
    if (!p->ranges[0] || !bongo_cat_ui_font_split_ranges(p->ranges[0],
        &p->ranges[1], &p->ranges[2], &p->ranges[3])) {
        /* split_ranges frees partial allocations on failure. */
        p->ranges[1] = p->ranges[2] = p->ranges[3] = NULL;
        return false;
    }
    char paths[3][BONGO_CAT_PATH_CAP];
    const char *names[] = {
        bongo_cat_ui_system_font(paths[0], sizeof(paths[0]), false),
        bongo_cat_ui_system_font(paths[1], sizeof(paths[1]), true),
        bongo_cat_ui_system_korean_font(paths[2], sizeof(paths[2]))};
    UIFontSource sources[3] = {0};
    for (int i = 0; i < 3; ++i)
        if (names[i]) bongo_cat_ui_font_source_load(&sources[i], names[i]);
    nk_font_atlas_init_default(&p->atlas);
    nk_font_atlas_begin(&p->atlas);
    float height = 24.0f * fmaxf(1.0f, fminf(2.0f,
        b->scale * b->raster_scale));
    p->font = bongo_cat_ui_font_add_family(&p->atlas, &sources[0], &sources[1],
        &sources[2], height, p->ranges[0], p->ranges[1], p->ranges[2],
        p->ranges[3]);
    int width = 0, rows = 0, maximum = 0;
    const void *pixels = p->font
        ? nk_font_atlas_bake(&p->atlas, &width, &rows, NK_FONT_ATLAS_ALPHA8)
        : NULL;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maximum);
    bool ok = pixels && width > 0 && rows > 0 && width <= maximum
        && rows <= maximum;
    if (ok) {
        glGenTextures(1, &p->font_texture);
        glBindTexture(GL_TEXTURE_2D, p->font_texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_ONE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_ONE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_ONE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_RED);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width, rows, 0, GL_RED,
            GL_UNSIGNED_BYTE, pixels);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        ok = p->font_texture && glGetError() == GL_NO_ERROR;
        if (ok) {
            p->font_width = width;
            p->font_height = rows;
        }
        nk_font_atlas_end(&p->atlas, nk_handle_id((int)p->font_texture), NULL);
    }
    for (int i = 0; i < 3; ++i) {
        bongo_cat_ui_font_detach_source(&p->atlas, &sources[i]);
        bongo_cat_ui_font_source_release(&sources[i]);
    }
    nk_font_atlas_cleanup(&p->atlas);
    if (ok) b->runes_new = false;
    b->fonts_ready = ok;
    return ok;
}

static float glyph_scale(BongoCatBubble *b, float size) {
    return size / b->paint.font->info.height;
}

float bubble_text_width(BongoCatBubble *b, const char *text, float size) {
    if (!b->paint.font || !text) return 0.0f;
    float scale = glyph_scale(b, size);
    float total = 0.0f;
    int length = (int)strlen(text);
    while (length > 0) {
        nk_rune rune;
        int n = nk_utf_decode(text, &rune, length);
        if (n <= 0) break;
        const struct nk_font_glyph *g = nk_font_find_glyph(b->paint.font,
            rune);
        if (g) total += g->xadvance * scale;
        text += n;
        length -= n;
    }
    return total;
}

void bubble_text(BongoCatBubble *b, const char *text, float x, float y,
    float size, uint32_t color) {
    struct nk_font *font = b->paint.font;
    if (!font || !text) return;
    float scale = glyph_scale(b, size);
    float pen = x;
    float top = y - font->info.height * scale / 2.0f;
    int length = (int)strlen(text);
    while (length > 0) {
        nk_rune rune;
        int n = nk_utf_decode(text, &rune, length);
        if (n <= 0) break;
        const struct nk_font_glyph *g = nk_font_find_glyph(font, rune);
        if (g) {
            if (g->w > 0 && g->h > 0)
                bubble_quad(b, b->paint.font_texture, pen + g->x0 * scale,
                    top + g->y0 * scale, g->w * scale, g->h * scale,
                    g->u0, g->v0, g->u1, g->v1, color);
            pen += g->xadvance * scale;
        }
        text += n;
        length -= n;
    }
}

/* Greedy width-based line breaking; CJK needs no word boundaries. Only the
   last max_lines lines are kept (in reading order) so growing transcripts
   scroll toward the newest text. Returns the number of lines produced. */
int bongo_cat_bubble_wrap(BongoCatBubble *b, const char *text, float width,
    float size, char lines[][BUBBLE_LINE_CAP], int max_lines) {
    if (!b->paint.font || !text || !text[0] || width <= 0.0f ||
        max_lines <= 0) return 0;
    if (max_lines > 16) max_lines = 16;
    float scale = glyph_scale(b, size);
    char current[BUBBLE_LINE_CAP];
    size_t length = 0;
    float line_width = 0.0f;
    int seen = 0;
    const char *cursor = text;
    while (*cursor) {
        nk_rune rune;
        int n = nk_utf_decode(cursor, &rune, (int)strlen(cursor));
        if (n <= 0) break;
        cursor += n;
        if (rune == '\n') {
            if (length) {
                current[length] = '\0';
                snprintf(lines[seen % max_lines], BUBBLE_LINE_CAP, "%s",
                    current);
                ++seen;
                length = 0;
                line_width = 0.0f;
            }
            continue;
        }
        const struct nk_font_glyph *g = nk_font_find_glyph(b->paint.font,
            rune);
        float advance = g ? g->xadvance * scale : 0.0f;
        if (line_width + advance > width && length > 0) {
            current[length] = '\0';
            snprintf(lines[seen % max_lines], BUBBLE_LINE_CAP, "%s", current);
            ++seen;
            length = 0;
            line_width = 0.0f;
        }
        if (length + (size_t)n < BUBBLE_LINE_CAP) {
            memcpy(current + length, cursor - n, (size_t)n);
            length += (size_t)n;
        }
        line_width += advance;
    }
    if (length) {
        current[length] = '\0';
        snprintf(lines[seen % max_lines], BUBBLE_LINE_CAP, "%s", current);
        ++seen;
    }
    if (!seen) return 0;
    int produced = seen < max_lines ? seen : max_lines;
    /* Rotate the ring so the oldest kept line comes first. While the ring has
       not wrapped yet, the oldest kept line is slot 0. */
    int first = seen < max_lines ? 0 : seen % max_lines;
    char ordered[16][BUBBLE_LINE_CAP];
    for (int i = 0; i < produced; ++i)
        snprintf(ordered[i], BUBBLE_LINE_CAP, "%s",
            lines[(first + i) % max_lines]);
    for (int i = 0; i < produced; ++i)
        snprintf(lines[i], BUBBLE_LINE_CAP, "%s", ordered[i]);
    return produced;
}
