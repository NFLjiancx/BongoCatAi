#include "bubble_internal.h"
#include "ui_present.h"

#include <SDL3/SDL_opengl.h>
#include <math.h>
#include <stddef.h>
#include <stdlib.h>

static const char *vertex_shader =
    "#version 330 core\n"
    "layout(location=0) in vec2 Position; layout(location=1) in vec2 UV;"
    "layout(location=2) in vec4 Color; uniform mat4 Projection;"
    "out vec2 uv; out vec4 color;"
    "void main(){uv=UV;color=Color;gl_Position=Projection*vec4(Position,0,1);}";
static const char *fragment_shader =
    "#version 330 core\n"
    "in vec2 uv; in vec4 color; uniform sampler2D Texture; out vec4 pixel;"
    "void main(){pixel=color*texture(Texture,uv);}";

static void vertex(BongoCatBubble *b, GLuint texture, float x, float y,
    float u, float v, uint32_t color) {
    BubblePaint *p = &b->paint;
    if (p->failed) return;
    if (p->count == p->capacity) {
        size_t capacity = p->capacity ? p->capacity * 2 : 8192;
        if (capacity > 131072) { p->failed = true; return; }
        void *memory = realloc(p->vertices, capacity * sizeof(*p->vertices));
        if (!memory) { p->failed = true; return; }
        p->vertices = memory;
        p->capacity = capacity;
    }
    if (!p->batch_count || p->batches[p->batch_count - 1].texture != texture) {
        if (p->batch_count == 128) { p->failed = true; return; }
        p->batches[p->batch_count++] =
            (BubbleBatch){texture, (int)p->count, 0};
    }
    p->vertices[p->count++] = (BubbleVertex){x, y, u, v,
        (uint8_t)(color >> 16), (uint8_t)(color >> 8), (uint8_t)color,
        (uint8_t)(color >> 24)};
    p->batches[p->batch_count - 1].count++;
}

void bubble_quad(BongoCatBubble *b, GLuint texture, float x, float y,
    float w, float h, float u0, float v0, float u1, float v1, uint32_t color) {
    vertex(b, texture, x, y, u0, v0, color);
    vertex(b, texture, x + w, y, u1, v0, color);
    vertex(b, texture, x + w, y + h, u1, v1, color);
    vertex(b, texture, x, y, u0, v0, color);
    vertex(b, texture, x + w, y + h, u1, v1, color);
    vertex(b, texture, x, y + h, u0, v1, color);
}

void bubble_rect(BongoCatBubble *b, float x, float y, float w, float h,
    uint32_t color) {
    bubble_quad(b, b->paint.white, x, y, w, h, 0.0f, 0.0f, 1.0f, 1.0f, color);
}

static void corner(BongoCatBubble *b, float cx, float cy, float radius,
    float start, uint32_t color) {
    const int segments = 6;
    for (int i = 0; i < segments; ++i) {
        float a0 = start + (float)i * (3.14159265f / 2.0f) / segments;
        float a1 = start + (float)(i + 1) * (3.14159265f / 2.0f) / segments;
        float x0 = cx + radius * cosf(a0), y0 = cy + radius * sinf(a0);
        float x1 = cx + radius * cosf(a1), y1 = cy + radius * sinf(a1);
        vertex(b, b->paint.white, cx, cy, 0.0f, 0.0f, color);
        vertex(b, b->paint.white, x0, y0, 0.0f, 0.0f, color);
        vertex(b, b->paint.white, x1, y1, 0.0f, 0.0f, color);
    }
}

void bubble_round_rect(BongoCatBubble *b, float x, float y, float w, float h,
    float radius, uint32_t color) {
    float r = radius;
    if (r > w / 2.0f) r = w / 2.0f;
    if (r > h / 2.0f) r = h / 2.0f;
    if (r < 0.5f) {
        bubble_rect(b, x, y, w, h, color);
        return;
    }
    bubble_rect(b, x + r, y, w - 2.0f * r, h, color);
    bubble_rect(b, x, y + r, r, h - 2.0f * r, color);
    bubble_rect(b, x + w - r, y + r, r, h - 2.0f * r, color);
    corner(b, x + w - r, y + r, r, -3.14159265f / 2.0f, color);
    corner(b, x + w - r, y + h - r, r, 0.0f, color);
    corner(b, x + r, y + h - r, r, 3.14159265f / 2.0f, color);
    corner(b, x + r, y + r, r, 3.14159265f, color);
}

bool bubble_paint_init(BongoCatBubble *b) {
    BubblePaint *p = &b->paint;
    BongoCatError error = {0};
    if (!bongo_cat_gl_load(&p->gl, &error))
        return SDL_SetError("Bubble GL loader: %s", error.message);
    bongo_cat_gl_clear_errors();
    p->program = bongo_cat_gl_program(&p->gl, vertex_shader, fragment_shader,
        &error);
    if (!p->program) {
        SDL_SetError("%s", error.message);
        return false;
    }
    p->projection = p->gl.uniform_location(p->program, "Projection");
    p->sampler = p->gl.uniform_location(p->program, "Texture");
    p->gl.gen_vertex_arrays(1, &p->vao);
    p->gl.gen_buffers(1, &p->vbo);
    p->gl.bind_vertex_array(p->vao);
    p->gl.bind_buffer(GL_ARRAY_BUFFER, p->vbo);
    p->gl.enable_attribute(0);
    p->gl.enable_attribute(1);
    p->gl.enable_attribute(2);
    p->gl.attribute_pointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(BubbleVertex),
        (void *)offsetof(BubbleVertex, x));
    p->gl.attribute_pointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(BubbleVertex),
        (void *)offsetof(BubbleVertex, u));
    p->gl.attribute_pointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE,
        sizeof(BubbleVertex), (void *)offsetof(BubbleVertex, r));
    glGenTextures(1, &p->white);
    glBindTexture(GL_TEXTURE_2D, p->white);
    const uint8_t white[] = {255, 255, 255, 255};
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA,
        GL_UNSIGNED_BYTE, white);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    if (!bubble_fonts_rebuild(b))
        return SDL_SetError("Bubble font atlas could not be created");
    return glGetError() == GL_NO_ERROR ||
        SDL_SetError("Bubble GL initialization error: 0x%x",
            (unsigned)glGetError());
}

bool bubble_paint_frame(BongoCatBubble *b) {
    BubblePaint *p = &b->paint;
    p->count = 0;
    p->batch_count = 0;
    p->failed = false;
    b->dirty = false;
    bongo_cat_bubble_scene(b);
    if (p->failed) return SDL_SetError("Bubble geometry budget exceeded");
    glViewport(0, 0, b->pixel_width, b->pixel_height);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND);
    p->gl.blend_equation(GL_FUNC_ADD);
    p->gl.blend_func_separate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE,
        GL_ONE_MINUS_SRC_ALPHA);
    float projection[16] = {2.0f / (float)b->width, 0, 0, 0,
        0, -2.0f / (float)b->height, 0, 0, 0, 0, -1, 0,
        -1, 1, 0, 1};
    p->gl.use_program(p->program);
    p->gl.uniform_matrix_4fv(p->projection, 1, GL_FALSE, projection);
    p->gl.uniform_1i(p->sampler, 0);
    p->gl.active_texture(GL_TEXTURE0);
    p->gl.bind_vertex_array(p->vao);
    p->gl.bind_buffer(GL_ARRAY_BUFFER, p->vbo);
    p->gl.buffer_data(GL_ARRAY_BUFFER,
        (GLsizeiptr)(p->count * sizeof(*p->vertices)), p->vertices,
        GL_STREAM_DRAW);
    for (int i = 0; i < p->batch_count; ++i) {
        glBindTexture(GL_TEXTURE_2D, p->batches[i].texture);
        glDrawArrays(GL_TRIANGLES, p->batches[i].first, p->batches[i].count);
    }
    if (glGetError() != GL_NO_ERROR)
        return SDL_SetError("Bubble OpenGL draw failed");
    return bongo_cat_ui_present(b->window);
}

void bubble_paint_free(BongoCatBubble *b) {
    BubblePaint *p = &b->paint;
    if (p->font_texture) glDeleteTextures(1, &p->font_texture);
    if (p->atlas.permanent.alloc) nk_font_atlas_clear(&p->atlas);
    p->font = NULL;
    p->font_texture = 0;
    if (p->white) glDeleteTextures(1, &p->white);
    if (p->vbo) p->gl.delete_buffers(1, &p->vbo);
    if (p->vao) p->gl.delete_vertex_arrays(1, &p->vao);
    if (p->program) p->gl.delete_program(p->program);
    free(p->vertices);
    p->vertices = NULL;
    p->capacity = 0;
    p->count = 0;
    p->white = p->vbo = p->vao = p->program = 0;
    b->fonts_ready = false;
}
