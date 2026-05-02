#include "renderer/item_icons.h"
#include "game/item.h"
#include "game/weapon.h"
#include "renderer/shader.h"
#include "core/math.h"
#include "core/log.h"

#include <glad/glad.h>
#include <cstring>

// ---- 16x16 icon silhouettes (u16 per row, MSB = leftmost pixel) ----
// Each icon is 16 rows of 16-bit bitmasks.
// Bit 15 (MSB) = leftmost pixel (x=0), bit 0 = rightmost pixel (x=15).

static const u16 s_iconData[][16] = {
    // 0: NONE — small diamond (generic/empty slot indicator)
    {
        0x0000, 0x0000, 0x0000, 0x0000,
        0x0180, 0x03C0, 0x07E0, 0x0FF0,
        0x0FF0, 0x07E0, 0x03C0, 0x0180,
        0x0000, 0x0000, 0x0000, 0x0000,
    },
    // 1: SWORD — vertical blade with crossguard and handle
    {
        0x0180, 0x0180, 0x0180, 0x0180,
        0x01C0, 0x01C0, 0x03C0, 0x03C0,
        0x0180, 0x0FF0, 0x0FF0, 0x0180,
        0x0180, 0x0180, 0x03C0, 0x07E0,
    },
    // 2: DAGGER — short blade with guard
    {
        0x0000, 0x0000, 0x0180, 0x0180,
        0x0180, 0x01C0, 0x01C0, 0x0180,
        0x0FF0, 0x0FF0, 0x0180, 0x0180,
        0x0180, 0x03C0, 0x0000, 0x0000,
    },
    // 3: AXE — wedge head on stick
    {
        0x0000, 0x0F00, 0x1F80, 0x3FC0,
        0x3FC0, 0x1F80, 0x0F80, 0x0180,
        0x0180, 0x0180, 0x0180, 0x0180,
        0x0180, 0x0180, 0x03C0, 0x03C0,
    },
    // 4: PISTOL — L-shaped gun profile
    {
        0x0000, 0x0000, 0x0000, 0x7FC0,
        0x7FE0, 0x7FE0, 0x01E0, 0x01E0,
        0x03E0, 0x07C0, 0x0FC0, 0x0FC0,
        0x0F80, 0x0F00, 0x0000, 0x0000,
    },
    // 5: SMG — longer body with pistol grip
    {
        0x0000, 0x0000, 0xFFC0, 0xFFE0,
        0xFFE0, 0xFFE0, 0x03E0, 0x03E0,
        0x07E0, 0x0FC0, 0x0FC0, 0x0F80,
        0x0F80, 0x0F00, 0x0000, 0x0000,
    },
    // 6: CARBINE — full rifle profile
    {
        0x0000, 0xFFF0, 0xFFF8, 0xFFF8,
        0x00F8, 0x00F0, 0x01F0, 0x03E0,
        0x07C0, 0x0F80, 0x0F80, 0x0F00,
        0x0F00, 0x0E00, 0x0000, 0x0000,
    },
    // 7: REVOLVER — rounded cylinder + barrel
    {
        0x0000, 0x0000, 0x7F00, 0x7F80,
        0x7FC0, 0x7FC0, 0x07C0, 0x0FC0,
        0x1F80, 0x1F80, 0x3F00, 0x3F00,
        0x3E00, 0x0000, 0x0000, 0x0000,
    },
    // 8: BOW — curved arc with string (left arc = bow limb, right column = string)
    {
        0x0080, 0x0180, 0x0280, 0x0480,
        0x0880, 0x1080, 0x2080, 0x4080,
        0x4080, 0x2080, 0x1080, 0x0880,
        0x0480, 0x0280, 0x0180, 0x0080,
    },
    // 9: CROSSBOW — T-shape (horizontal stock + vertical tiller)
    {
        0x0000, 0xFFFF, 0xFFFF, 0x8181,
        0xC3C3, 0x6666, 0x0180, 0x0180,
        0x0180, 0x0180, 0x0180, 0x0180,
        0x03C0, 0x07E0, 0x0000, 0x0000,
    },
    // 10: THROWING_KNIFE — small diagonal blade (top-right to bottom-left)
    {
        0x0000, 0x0003, 0x0006, 0x000C,
        0x0018, 0x0030, 0x0060, 0x00C0,
        0x0180, 0x0380, 0x0700, 0x0E00,
        0x0C00, 0x1800, 0x1000, 0x0000,
    },
    // 11: MOLOTOV — bottle shape with flame at top
    {
        0x0100, 0x0380, 0x0280, 0x0100,
        0x0180, 0x0180, 0x0180, 0x03C0,
        0x07E0, 0x07E0, 0x07E0, 0x07E0,
        0x07E0, 0x07E0, 0x03C0, 0x03C0,
    },
    // 12: WAND — thin stick with glowing crystal tip
    {
        0x0180, 0x03C0, 0x03C0, 0x0180,
        0x0080, 0x0080, 0x0080, 0x0080,
        0x0080, 0x0080, 0x0080, 0x0080,
        0x0080, 0x0080, 0x00C0, 0x0000,
    },
    // 13: SHIELD (offhand) — classic kite shield silhouette
    {
        0x0000, 0x3FFC, 0x7FFE, 0x7FFE,
        0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
        0x7FFE, 0x7FFE, 0x3FFC, 0x1FF8,
        0x0FF0, 0x07E0, 0x03C0, 0x0180,
    },
    // 13: HELMET — dome/great-helm shape
    {
        0x0000, 0x0000, 0x07E0, 0x0FF0,
        0x1FF8, 0x3FFC, 0x3FFC, 0x7FFE,
        0x7FFE, 0x7FFE, 0xFFFF, 0xFFFF,
        0xFFFF, 0x0000, 0x0000, 0x0000,
    },
    // 14: ARMOR — breastplate/chest silhouette
    {
        0x0000, 0xE00E, 0xF01E, 0x783C,
        0x3FF8, 0x1FF0, 0x1FF0, 0x1FF0,
        0x1FF0, 0x1FF0, 0x1FF0, 0x1FF0,
        0x0FE0, 0x0FE0, 0x07C0, 0x0000,
    },
    // 15: BOOTS — side-profile boot shape
    {
        0x0000, 0x0000, 0x0F00, 0x0F00,
        0x0F00, 0x0F00, 0x0F00, 0x0F00,
        0x0F00, 0x0F00, 0x0F80, 0x0FC0,
        0x0FE0, 0x1FF0, 0x1FF0, 0x0000,
    },
    // 16: RING — circle outline
    {
        0x0000, 0x0000, 0x0000, 0x03C0,
        0x07E0, 0x0E70, 0x0C30, 0x1818,
        0x1818, 0x0C30, 0x0E70, 0x07E0,
        0x03C0, 0x0000, 0x0000, 0x0000,
    },
};

static constexpr u32 ICON_COUNT = sizeof(s_iconData) / sizeof(s_iconData[0]);
static constexpr u32 ATLAS_W    = ICON_ATLAS_COLS * ICON_SIZE;  // 256px wide
static constexpr u32 ATLAS_H    = ICON_ATLAS_ROWS * ICON_SIZE;  // 32px tall

// GPU resources
static u32    s_atlasTexture = 0;
static Shader s_unlitShader;
static u32    s_vao = 0;
static u32    s_vbo = 0;

void ItemIconSystem::init() {
    // Build atlas RGBA pixel data (white silhouettes with alpha mask)
    u8 atlasData[ATLAS_W * ATLAS_H * 4] = {};

    for (u32 idx = 0; idx < ICON_COUNT; idx++) {
        // Each icon occupies one cell; row/col determine its atlas position
        u32 col   = idx % ICON_ATLAS_COLS;
        u32 row   = idx / ICON_ATLAS_COLS;
        u32 baseX = col * ICON_SIZE;
        u32 baseY = row * ICON_SIZE;

        for (u32 py = 0; py < ICON_SIZE; py++) {
            u16 rowBits = s_iconData[idx][py];
            for (u32 px = 0; px < ICON_SIZE; px++) {
                // MSB (bit 15) = leftmost pixel; shift right by (15-px) to test
                bool on = (rowBits >> (15 - px)) & 1;
                u32 atlasX = baseX + px;
                u32 atlasY = baseY + py;
                u32 i      = (atlasY * ATLAS_W + atlasX) * 4;
                atlasData[i + 0] = 255;        // R — always white, tint applied in shader
                atlasData[i + 1] = 255;        // G
                atlasData[i + 2] = 255;        // B
                atlasData[i + 3] = on ? 255 : 0;  // A — silhouette mask
            }
        }
    }

    // Upload atlas to GPU
    glGenTextures(1, &s_atlasTexture);
    glBindTexture(GL_TEXTURE_2D, s_atlasTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, ATLAS_W, ATLAS_H, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, atlasData);
    // GL_NEAREST preserves hard pixel-art edges; mipmapping would blur them
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Load shared unlit shader (same as font system)
    s_unlitShader = ShaderSystem::load("assets/shaders/unlit.vert", "assets/shaders/unlit.frag");

    // VAO/VBO for a single icon quad (6 verts: 2 triangles, interleaved pos3+uv2)
    glGenVertexArrays(1, &s_vao);
    glBindVertexArray(s_vao);

    glGenBuffers(1, &s_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    // Allocate for 6 verts * 5 floats (xyz + uv); updated each draw call
    glBufferData(GL_ARRAY_BUFFER, 6 * 5 * sizeof(f32), nullptr, GL_DYNAMIC_DRAW);

    // Position attribute at layout location 0 (x, y, z)
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(f32), (void*)0);
    glEnableVertexAttribArray(0);
    // UV attribute at layout location 2 — matches unlit shader layout (0=pos, 1=normal, 2=uv)
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(f32), (void*)(3 * sizeof(f32)));
    glEnableVertexAttribArray(2);

    glBindVertexArray(0);

    LOG_INFO("ItemIconSystem: atlas %ux%u, %u icons", ATLAS_W, ATLAS_H, ICON_COUNT);
}

void ItemIconSystem::shutdown() {
    if (s_atlasTexture) { glDeleteTextures(1, &s_atlasTexture); s_atlasTexture = 0; }
    if (s_vao)          { glDeleteVertexArrays(1, &s_vao);      s_vao = 0;          }
    if (s_vbo)          { glDeleteBuffers(1, &s_vbo);           s_vbo = 0;          }
    ShaderSystem::destroy(s_unlitShader);
}

// Map WeaponSubtype enum value to atlas icon index (row 0 icons, indices 0-11)
static u32 subtypeToIconIndex(WeaponSubtype st) {
    switch (st) {
        case WeaponSubtype::NONE:           return 0;
        case WeaponSubtype::SWORD:          return 1;
        case WeaponSubtype::DAGGER:         return 2;
        case WeaponSubtype::AXE:            return 3;
        case WeaponSubtype::PISTOL:         return 4;
        case WeaponSubtype::SMG:            return 5;
        case WeaponSubtype::CARBINE:        return 6;
        case WeaponSubtype::REVOLVER:       return 7;
        case WeaponSubtype::BOW:            return 8;
        case WeaponSubtype::CROSSBOW:       return 9;
        case WeaponSubtype::THROWING_KNIFE: return 10;
        case WeaponSubtype::MOLOTOV:        return 11;
        case WeaponSubtype::WAND:           return 12;
        default:                            return 0;
    }
}

// Map ItemSlot enum to atlas icon index for non-weapon equipment (indices 12-16)
static u32 slotToIconIndex(ItemSlot slot) {
    switch (slot) {
        case ItemSlot::WEAPON:  return 1;   // sword icon as generic weapon fallback
        case ItemSlot::OFFHAND: return 13;  // shield (shifted +1 for wand icon)
        case ItemSlot::HELMET:  return 14;
        case ItemSlot::ARMOR:   return 15;
        case ItemSlot::BOOTS:   return 16;
        case ItemSlot::RING:    return 17;
        default:                return 0;
    }
}

void ItemIconSystem::drawIcon(u32 screenWidth, u32 screenHeight,
                               f32 x, f32 y, f32 size,
                               const ItemDef& def, Rarity rarity) {
    if (!s_vao || !s_atlasTexture) return;

    // Weapon items use their specific subtype icon; other equipment uses slot icon
    u32 iconIdx;
    if (def.slot == ItemSlot::WEAPON && def.weaponSubtype != WeaponSubtype::NONE) {
        iconIdx = subtypeToIconIndex(def.weaponSubtype);
    } else {
        iconIdx = slotToIconIndex(def.slot);
    }

    // Compute UV rect for this icon within the atlas
    u32 col = iconIdx % ICON_ATLAS_COLS;
    u32 row = iconIdx / ICON_ATLAS_COLS;
    f32 u0  = static_cast<f32>(col * ICON_SIZE)           / static_cast<f32>(ATLAS_W);
    f32 v0  = static_cast<f32>(row * ICON_SIZE)           / static_cast<f32>(ATLAS_H);
    f32 u1  = static_cast<f32>((col + 1) * ICON_SIZE)     / static_cast<f32>(ATLAS_W);
    f32 v1  = static_cast<f32>((row + 1) * ICON_SIZE)     / static_cast<f32>(ATLAS_H);

    // Column-major orthographic projection mapping screen pixels to NDC.
    // Matches the convention used by FontSystem::drawText.
    f32 w = static_cast<f32>(screenWidth);
    f32 h = static_cast<f32>(screenHeight);
    Mat4 ortho = Mat4::identity();
    ortho.m[0]  =  2.0f / w;
    ortho.m[5]  =  2.0f / h;
    ortho.m[10] = -1.0f;
    ortho.m[12] = -1.0f;
    ortho.m[13] = -1.0f;

    // Build quad as two CCW triangles (pos3 + uv2 per vertex).
    // v0/v1 are flipped relative to screen Y: atlas row 0 is at top, but
    // screen Y=0 is bottom, so v0 maps to the top of the quad (y+size).
    f32 verts[] = {
        // Triangle 1: bottom-left, bottom-right, top-right
        x,        y,        0.0f,  u0, v1,
        x + size, y,        0.0f,  u1, v1,
        x + size, y + size, 0.0f,  u1, v0,
        // Triangle 2: bottom-left, top-right, top-left
        x,        y,        0.0f,  u0, v1,
        x + size, y + size, 0.0f,  u1, v0,
        x,        y + size, 0.0f,  u0, v0,
    };

    // Rarity tint color applied as u_color uniform (white texture * tint color)
    Vec3 rc   = rarityColor(rarity);
    Vec4 tint = {rc.x, rc.y, rc.z, 1.0f};

    // Render with alpha blending, no depth writes (2D overlay)
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram(s_unlitShader.program);

    if (s_unlitShader.loc_mvp >= 0)
        glUniformMatrix4fv(s_unlitShader.loc_mvp, 1, GL_FALSE, ortho.ptr());

    if (s_unlitShader.loc_color >= 0)
        glUniform4f(s_unlitShader.loc_color, tint.x, tint.y, tint.z, tint.w);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_atlasTexture);
    if (s_unlitShader.loc_texture0 >= 0)
        glUniform1i(s_unlitShader.loc_texture0, 0);

    // Stream updated quad verts, then draw
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);

    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
}
