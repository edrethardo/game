#include "renderer/debug_draw.h"
#include "renderer/shader.h"
#include "core/log.h"

#include <glad/glad.h>
#include <cstdlib>
#include <cstring>

struct DebugVertex {
    Vec3 pos;
    Vec3 color;
};

static constexpr u32 MAX_DEBUG_LINES = 8192;

static DebugVertex s_verts[MAX_DEBUG_LINES * 2];
static u32  s_lineCount = 0;
static bool s_enabled   = false;

static u32    s_vao = 0;
static u32    s_vbo = 0;
static Shader s_shader;

void DebugDraw::init() {
    s_shader = ShaderSystem::load("assets/shaders/debug.vert", "assets/shaders/debug.frag");
    if (!s_shader.program) {
        LOG_WARN("DebugDraw: debug shaders not found, debug rendering disabled");
        return;
    }

    glGenVertexArrays(1, &s_vao);
    glBindVertexArray(s_vao);

    glGenBuffers(1, &s_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(s_verts), nullptr, GL_DYNAMIC_DRAW);

    // position
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(DebugVertex), (void*)0);
    glEnableVertexAttribArray(0);
    // color
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(DebugVertex), (void*)(sizeof(Vec3)));
    glEnableVertexAttribArray(1);

    glBindVertexArray(0);

    s_enabled = false;
    LOG_INFO("DebugDraw initialized");
}

void DebugDraw::shutdown() {
    if (s_vao) { glDeleteVertexArrays(1, &s_vao); s_vao = 0; }
    if (s_vbo) { glDeleteBuffers(1, &s_vbo); s_vbo = 0; }
    ShaderSystem::destroy(s_shader);
}

void DebugDraw::setEnabled(bool enabled) { s_enabled = enabled; }
bool DebugDraw::isEnabled()              { return s_enabled; }

void DebugDraw::clear() {
    s_lineCount = 0;
}

void DebugDraw::line(Vec3 start, Vec3 end, Vec3 color) {
    if (!s_enabled || s_lineCount >= MAX_DEBUG_LINES) return;
    s_verts[s_lineCount * 2 + 0] = {start, color};
    s_verts[s_lineCount * 2 + 1] = {end,   color};
    s_lineCount++;
}

void DebugDraw::box(const AABB& b, Vec3 color) {
    Vec3 mn = b.min, mx = b.max;
    // Bottom face
    line({mn.x,mn.y,mn.z},{mx.x,mn.y,mn.z},color);
    line({mx.x,mn.y,mn.z},{mx.x,mn.y,mx.z},color);
    line({mx.x,mn.y,mx.z},{mn.x,mn.y,mx.z},color);
    line({mn.x,mn.y,mx.z},{mn.x,mn.y,mn.z},color);
    // Top face
    line({mn.x,mx.y,mn.z},{mx.x,mx.y,mn.z},color);
    line({mx.x,mx.y,mn.z},{mx.x,mx.y,mx.z},color);
    line({mx.x,mx.y,mx.z},{mn.x,mx.y,mx.z},color);
    line({mn.x,mx.y,mx.z},{mn.x,mx.y,mn.z},color);
    // Vertical edges
    line({mn.x,mn.y,mn.z},{mn.x,mx.y,mn.z},color);
    line({mx.x,mn.y,mn.z},{mx.x,mx.y,mn.z},color);
    line({mx.x,mn.y,mx.z},{mx.x,mx.y,mx.z},color);
    line({mn.x,mn.y,mx.z},{mn.x,mx.y,mx.z},color);
}

void DebugDraw::ray(Vec3 origin, Vec3 dir, f32 len, Vec3 color) {
    line(origin, origin + normalize(dir) * len, color);
}

void DebugDraw::cross(Vec3 pos, f32 size, Vec3 color) {
    f32 h = size * 0.5f;
    line(pos - Vec3{h,0,0}, pos + Vec3{h,0,0}, color);
    line(pos - Vec3{0,h,0}, pos + Vec3{0,h,0}, color);
    line(pos - Vec3{0,0,h}, pos + Vec3{0,0,h}, color);
}

void DebugDraw::flush(const Mat4& viewProjection) {
    if (!s_enabled || !s_vao || !s_shader.program || s_lineCount == 0) return;

    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, s_lineCount * 2 * sizeof(DebugVertex), s_verts);

    glUseProgram(s_shader.program);
    s32 locVP = glGetUniformLocation(s_shader.program, "u_vp");
    if (locVP >= 0) glUniformMatrix4fv(locVP, 1, GL_FALSE, viewProjection.ptr());

    glBindVertexArray(s_vao);
    glDrawArrays(GL_LINES, 0, s_lineCount * 2);
    glBindVertexArray(0);
}
