#include "renderer/shader.h"
#include "core/log.h"
#include "core/math.h"

#include <glad/glad.h>
#include <cstdio>
#include <cstdlib>

static char* readFile(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        LOG_ERROR("Could not open shader file: %s", path);
        return nullptr;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char* buf = static_cast<char*>(std::malloc(size + 1));
    fread(buf, 1, size, f);
    buf[size] = '\0';
    fclose(f);
    return buf;
}

static u32 compileShader(GLenum type, const char* source, const char* path) {
    u32 shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char log[1024];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        LOG_ERROR("Shader compile error (%s):\n%s", path, log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

Shader ShaderSystem::load(const char* vertPath, const char* fragPath) {
    Shader shader = {};

    char* vertSrc = readFile(vertPath);
    char* fragSrc = readFile(fragPath);
    if (!vertSrc || !fragSrc) {
        std::free(vertSrc);
        std::free(fragSrc);
        return shader;
    }

#ifdef __SWITCH__
    // Switch mesa uses compatibility profile — strip "core" from #version directive
    auto stripCore = [](char* src) {
        char* p = std::strstr(src, "#version 330 core");
        if (p) std::memcpy(p, "#version 330     ", 17); // same length, spaces replace "core"
    };
    stripCore(vertSrc);
    stripCore(fragSrc);
#endif

    u32 vert = compileShader(GL_VERTEX_SHADER, vertSrc, vertPath);
    u32 frag = compileShader(GL_FRAGMENT_SHADER, fragSrc, fragPath);
    std::free(vertSrc);
    std::free(fragSrc);

    if (!vert || !frag) {
        if (vert) glDeleteShader(vert);
        if (frag) glDeleteShader(frag);
        return shader;
    }

    shader.program = glCreateProgram();
    glAttachShader(shader.program, vert);
    glAttachShader(shader.program, frag);
    glLinkProgram(shader.program);

    GLint success;
    glGetProgramiv(shader.program, GL_LINK_STATUS, &success);
    if (!success) {
        char log[1024];
        glGetProgramInfoLog(shader.program, sizeof(log), nullptr, log);
        LOG_ERROR("Shader link error:\n%s", log);
        glDeleteProgram(shader.program);
        shader.program = 0;
    }

    glDeleteShader(vert);
    glDeleteShader(frag);

    if (shader.program) {
        // Cache uniform locations
        shader.loc_mvp          = glGetUniformLocation(shader.program, "u_mvp");
        shader.loc_model        = glGetUniformLocation(shader.program, "u_model");
        shader.loc_lightDir     = glGetUniformLocation(shader.program, "u_lightDir");
        shader.loc_lightColor   = glGetUniformLocation(shader.program, "u_lightColor");
        shader.loc_ambientColor = glGetUniformLocation(shader.program, "u_ambientColor");
        shader.loc_texture0     = glGetUniformLocation(shader.program, "u_texture0");
        shader.loc_color        = glGetUniformLocation(shader.program, "u_color");
        shader.loc_vp           = glGetUniformLocation(shader.program, "u_vp");

        // Point light uniforms — queried by index since arrays don't have a single location
        for (int i = 0; i < 4; i++) {
            char posName[32], colName[32];
            snprintf(posName, sizeof(posName), "u_pointLightPos[%d]", i);
            snprintf(colName, sizeof(colName), "u_pointLightColor[%d]", i);
            shader.loc_pointLightPos[i]   = glGetUniformLocation(shader.program, posName);
            shader.loc_pointLightColor[i] = glGetUniformLocation(shader.program, colName);
        }
        shader.loc_pointLightCount = glGetUniformLocation(shader.program, "u_pointLightCount");

        LOG_INFO("Shader loaded: %s + %s (program=%u)", vertPath, fragPath, shader.program);
    }

    return shader;
}

void ShaderSystem::destroy(Shader& shader) {
    if (shader.program) {
        glDeleteProgram(shader.program);
        shader.program = 0;
    }
}

void ShaderSystem::bind(const Shader& shader) {
    glUseProgram(shader.program);
}

void ShaderSystem::setMat4(s32 location, const Mat4& m) {
    if (location >= 0) glUniformMatrix4fv(location, 1, GL_FALSE, m.ptr());
}

void ShaderSystem::setVec3(s32 location, const Vec3& v) {
    if (location >= 0) glUniform3f(location, v.x, v.y, v.z);
}

void ShaderSystem::setVec4(s32 location, const Vec4& v) {
    if (location >= 0) glUniform4f(location, v.x, v.y, v.z, v.w);
}

void ShaderSystem::setFloat(s32 location, f32 v) {
    if (location >= 0) glUniform1f(location, v);
}

void ShaderSystem::setInt(s32 location, s32 v) {
    if (location >= 0) glUniform1i(location, v);
}
