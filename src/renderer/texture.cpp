#include "renderer/texture.h"
#include "core/log.h"

#include <glad/glad.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"

Texture TextureSystem::load(const char* path) {
    stbi_set_flip_vertically_on_load(1);
    s32 w, h, channels;
    u8* pixels = stbi_load(path, &w, &h, &channels, 4);
    if (!pixels) {
        LOG_WARN("Could not load texture: %s — using white fallback", path);
        return createWhite();
    }
    Texture tex = create(pixels, w, h, 4);
    stbi_image_free(pixels);
    LOG_INFO("Texture loaded: %s (%dx%d)", path, w, h);
    return tex;
}

Texture TextureSystem::create(const u8* pixels, s32 width, s32 height, s32 channels) {
    Texture tex;
    tex.width  = width;
    tex.height = height;

    GLenum fmt = (channels == 4) ? GL_RGBA : GL_RGB;

    glGenTextures(1, &tex.handle);
    glBindTexture(GL_TEXTURE_2D, tex.handle);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, fmt, GL_UNSIGNED_BYTE, pixels);
    glGenerateMipmap(GL_TEXTURE_2D);

    // Nearest-neighbour for Barony pixel style
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
}

Texture TextureSystem::createWhite() {
    u8 pixel[4] = {255, 255, 255, 255};
    return create(pixel, 1, 1, 4);
}

void TextureSystem::destroy(Texture& tex) {
    if (tex.handle) {
        glDeleteTextures(1, &tex.handle);
        tex = {};
    }
}

void TextureSystem::bind(const Texture& tex, u32 unit) {
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, tex.handle);
}
