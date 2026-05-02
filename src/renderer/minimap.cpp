#include "renderer/minimap.h"
#include "renderer/shader.h"
#include "core/log.h"
#include <glad/glad.h>
#include <cstring>
#include <cmath>

// ---------------------------------------------------------------------------
// Static state
// ---------------------------------------------------------------------------

// Visited cell tracking (fog-of-war)
static bool s_visited[MAX_MINIMAP_CELLS] = {};
static u32  s_gridW = 0;
static u32  s_gridD = 0;

// GPU resources
static u32    s_minimapTex = 0;  // texture showing dungeon cells
static u32    s_whiteTex   = 0;  // 1x1 white pixel for drawing the player marker
static u32    s_minimapVAO = 0;
static u32    s_minimapVBO = 0;
static Shader s_minimapShader;
static bool   s_dirty = true;   // texture needs rebuilding

// RGBA pixel buffer (one pixel per cell)
static u8 s_pixelData[MAX_MINIMAP_CELLS * 4] = {};

// ---------------------------------------------------------------------------
// Vertex layout
// ---------------------------------------------------------------------------
struct MinimapVertex {
    Vec3 pos;
    Vec2 uv;
};

// ---------------------------------------------------------------------------
// Minimap::init
// ---------------------------------------------------------------------------
void Minimap::init(u32 gridWidth, u32 gridDepth) {
    s_gridW = gridWidth;
    s_gridD = gridDepth;
    std::memset(s_visited,   0, sizeof(s_visited));
    std::memset(s_pixelData, 0, sizeof(s_pixelData));
    s_dirty = true;

    // Dungeon texture (one texel = one cell)
    if (!s_minimapTex) {
        glGenTextures(1, &s_minimapTex);
        glBindTexture(GL_TEXTURE_2D, s_minimapTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    // 1x1 white pixel texture for solid-color draws (player dot / arrow)
    if (!s_whiteTex) {
        u8 white[4] = {255, 255, 255, 255};
        glGenTextures(1, &s_whiteTex);
        glBindTexture(GL_TEXTURE_2D, s_whiteTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }

    // VAO / VBO — dynamic buffer big enough for 12 vertices (reused for each draw call)
    if (!s_minimapVAO) {
        glGenVertexArrays(1, &s_minimapVAO);
        glBindVertexArray(s_minimapVAO);

        glGenBuffers(1, &s_minimapVBO);
        glBindBuffer(GL_ARRAY_BUFFER, s_minimapVBO);
        glBufferData(GL_ARRAY_BUFFER, 12 * sizeof(MinimapVertex), nullptr, GL_DYNAMIC_DRAW);

        // aPos  (location 0)
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(MinimapVertex),
                              (void*)0);
        glEnableVertexAttribArray(0);

        // aUV   (location 2 — matches unlit.vert layout)
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(MinimapVertex),
                              (void*)sizeof(Vec3));
        glEnableVertexAttribArray(2);

        glBindVertexArray(0);

        s_minimapShader = ShaderSystem::load("assets/shaders/unlit.vert",
                                             "assets/shaders/unlit.frag");
    }

    LOG_INFO("Minimap: initialized (%ux%u)", gridWidth, gridDepth);
}

// ---------------------------------------------------------------------------
// Minimap::shutdown
// ---------------------------------------------------------------------------
void Minimap::shutdown() {
    if (s_minimapTex) { glDeleteTextures(1, &s_minimapTex); s_minimapTex = 0; }
    if (s_whiteTex)   { glDeleteTextures(1, &s_whiteTex);   s_whiteTex   = 0; }
    if (s_minimapVAO) { glDeleteVertexArrays(1, &s_minimapVAO); s_minimapVAO = 0; }
    if (s_minimapVBO) { glDeleteBuffers(1, &s_minimapVBO);  s_minimapVBO = 0; }
    ShaderSystem::destroy(s_minimapShader);
}

// ---------------------------------------------------------------------------
// Minimap::updateVisited  (fog-of-war reveal)
// ---------------------------------------------------------------------------
void Minimap::updateVisited(const LevelGrid& grid, Vec3 playerPos) {
    u32 px, pz;
    if (!LevelGridSystem::worldToGrid(grid, playerPos, px, pz)) return;

    u32 radius = static_cast<u32>(MINIMAP_REVEAL_RADIUS);
    bool anyNew = false;

    for (u32 dz = 0; dz <= radius * 2; dz++) {
        for (u32 dx = 0; dx <= radius * 2; dx++) {
            s32 gx = static_cast<s32>(px) - static_cast<s32>(radius) + static_cast<s32>(dx);
            s32 gz = static_cast<s32>(pz) - static_cast<s32>(radius) + static_cast<s32>(dz);
            if (gx < 0 || gz < 0) continue;

            u32 ugx = static_cast<u32>(gx);
            u32 ugz = static_cast<u32>(gz);
            if (!LevelGridSystem::isInBounds(grid, ugx, ugz)) continue;

            // Circular reveal — skip corners
            f32 distX = static_cast<f32>(gx) - static_cast<f32>(px);
            f32 distZ = static_cast<f32>(gz) - static_cast<f32>(pz);
            if (distX * distX + distZ * distZ >
                MINIMAP_REVEAL_RADIUS * MINIMAP_REVEAL_RADIUS) continue;

            u32 idx = ugz * s_gridW + ugx;
            if (idx < MAX_MINIMAP_CELLS && !s_visited[idx]) {
                s_visited[idx] = true;
                anyNew = true;
            }
        }
    }

    if (anyNew) s_dirty = true;
}

// ---------------------------------------------------------------------------
// rebuildTexture  (internal — called lazily before draw)
// ---------------------------------------------------------------------------
static void rebuildTexture(const LevelGrid& grid) {
    for (u32 z = 0; z < s_gridD; z++) {
        for (u32 x = 0; x < s_gridW; x++) {
            u32 idx  = z * s_gridW + x;
            if (idx >= MAX_MINIMAP_CELLS) continue;
            u32 pIdx = idx * 4;

            if (!s_visited[idx]) {
                // Unexplored fog: very dark with slight blue tint, semi-transparent
                s_pixelData[pIdx + 0] = 10;
                s_pixelData[pIdx + 1] = 10;
                s_pixelData[pIdx + 2] = 15;
                s_pixelData[pIdx + 3] = 180;
                continue;
            }

            const GridCell& cell = LevelGridSystem::getCell(grid, x, z);

            if (cell.flags & CELL_SOLID) {
                // Wall: dark grey with a blue tint
                s_pixelData[pIdx + 0] = 55;
                s_pixelData[pIdx + 1] = 55;
                s_pixelData[pIdx + 2] = 65;
                s_pixelData[pIdx + 3] = 255;
            } else if (cell.flags & CELL_FLOOR) {
                // Floor: colour by material / height
                f32 floorH = static_cast<f32>(cell.floorHeight) * 0.25f;
                if (cell.wallMaterialId == 3) {
                    // Brick room: warm brown
                    s_pixelData[pIdx + 0] = 130;
                    s_pixelData[pIdx + 1] = 90;
                    s_pixelData[pIdx + 2] = 70;
                } else if (floorH > 0.1f) {
                    // Raised stone floor: lighter
                    s_pixelData[pIdx + 0] = 130;
                    s_pixelData[pIdx + 1] = 125;
                    s_pixelData[pIdx + 2] = 115;
                } else {
                    // Normal stone floor
                    s_pixelData[pIdx + 0] = 110;
                    s_pixelData[pIdx + 1] = 105;
                    s_pixelData[pIdx + 2] = 100;
                }
                s_pixelData[pIdx + 3] = 255;
            } else {
                // Void / ceiling-only cells
                s_pixelData[pIdx + 0] = 5;
                s_pixelData[pIdx + 1] = 5;
                s_pixelData[pIdx + 2] = 8;
                s_pixelData[pIdx + 3] = 200;
            }
        }
    }

    glBindTexture(GL_TEXTURE_2D, s_minimapTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                 static_cast<GLsizei>(s_gridW),
                 static_cast<GLsizei>(s_gridD),
                 0, GL_RGBA, GL_UNSIGNED_BYTE, s_pixelData);

    s_dirty = false;
}

// ---------------------------------------------------------------------------
// Helper: build orthographic matrix (bottom-left origin, matches HUD)
// ---------------------------------------------------------------------------
static Mat4 buildOrtho(f32 w, f32 h) {
    Mat4 o = Mat4::identity();
    o.m[0]  =  2.0f / w;
    o.m[5]  =  2.0f / h;
    o.m[10] = -1.0f;
    o.m[12] = -1.0f;
    o.m[13] = -1.0f;
    return o;
}

// ---------------------------------------------------------------------------
// Minimap::draw
// ---------------------------------------------------------------------------
void Minimap::draw(u32 screenWidth, u32 screenHeight,
                   const LevelGrid& grid, Vec3 playerPos, f32 playerYaw)
{
    if (s_gridW == 0 || s_gridD == 0) return;
    if (s_dirty) rebuildTexture(grid);

    // Screen-space position: top-right corner
    static constexpr f32 MAP_SIZE = 150.0f;
    static constexpr f32 MARGIN   = 10.0f;
    static constexpr f32 BORDER   = 2.0f;

    f32 sw = static_cast<f32>(screenWidth);
    f32 sh = static_cast<f32>(screenHeight);

    f32 mapX = sw - MAP_SIZE - MARGIN;
    f32 mapY = sh - MAP_SIZE - MARGIN;

    // Common GL state for 2D HUD drawing
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram(s_minimapShader.program);

    Mat4 ortho = buildOrtho(sw, sh);
    if (s_minimapShader.loc_mvp >= 0)
        glUniformMatrix4fv(s_minimapShader.loc_mvp, 1, GL_FALSE, ortho.ptr());

    glBindVertexArray(s_minimapVAO);
    glBindBuffer(GL_ARRAY_BUFFER, s_minimapVBO);

    // ------------------------------------------------------------------
    // 1. Dark border background (slightly larger than the map)
    // ------------------------------------------------------------------
    {
        f32 bx0 = mapX - BORDER;
        f32 by0 = mapY - BORDER;
        f32 bx1 = mapX + MAP_SIZE + BORDER;
        f32 by1 = mapY + MAP_SIZE + BORDER;

        MinimapVertex bg[6];
        bg[0] = {{bx0, by0, 0}, {0, 0}};
        bg[1] = {{bx1, by0, 0}, {1, 0}};
        bg[2] = {{bx1, by1, 0}, {1, 1}};
        bg[3] = {{bx0, by0, 0}, {0, 0}};
        bg[4] = {{bx1, by1, 0}, {1, 1}};
        bg[5] = {{bx0, by1, 0}, {0, 1}};

        glBufferSubData(GL_ARRAY_BUFFER, 0, 6 * sizeof(MinimapVertex), bg);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, s_whiteTex);
        if (s_minimapShader.loc_texture0 >= 0)
            glUniform1i(s_minimapShader.loc_texture0, 0);
        if (s_minimapShader.loc_color >= 0)
            glUniform4f(s_minimapShader.loc_color, 0.0f, 0.0f, 0.0f, 0.75f);

        glDrawArrays(GL_TRIANGLES, 0, 6);
    }

    // ------------------------------------------------------------------
    // 2. Dungeon map quad (textured)
    //    UV convention: (0,0) = top-left of texture = grid cell (x=0, z=0)
    //    Screen Y increases upward; grid Z increases "into" the level.
    //    We flip V so that low-Z rows appear at the top of the minimap.
    // ------------------------------------------------------------------
    {
        f32 x0 = mapX, y0 = mapY;
        f32 x1 = mapX + MAP_SIZE, y1 = mapY + MAP_SIZE;

        MinimapVertex mv[6];
        mv[0] = {{x0, y0, 0}, {0, 1}};  // bottom-left  screen  → high-Z row
        mv[1] = {{x1, y0, 0}, {1, 1}};  // bottom-right screen  → high-Z row
        mv[2] = {{x1, y1, 0}, {1, 0}};  // top-right    screen  → low-Z  row
        mv[3] = {{x0, y0, 0}, {0, 1}};
        mv[4] = {{x1, y1, 0}, {1, 0}};
        mv[5] = {{x0, y1, 0}, {0, 0}};  // top-left     screen  → low-Z  row

        glBufferSubData(GL_ARRAY_BUFFER, 0, 6 * sizeof(MinimapVertex), mv);

        glBindTexture(GL_TEXTURE_2D, s_minimapTex);
        if (s_minimapShader.loc_texture0 >= 0)
            glUniform1i(s_minimapShader.loc_texture0, 0);
        if (s_minimapShader.loc_color >= 0)
            glUniform4f(s_minimapShader.loc_color, 1.0f, 1.0f, 1.0f, 0.9f);

        glDrawArrays(GL_TRIANGLES, 0, 6);
    }

    // ------------------------------------------------------------------
    // 3. Player marker: bright-green dot + facing arrow
    // ------------------------------------------------------------------
    u32 px, pz;
    if (LevelGridSystem::worldToGrid(grid, playerPos, px, pz)) {
        f32 normX = (static_cast<f32>(px) + 0.5f) / static_cast<f32>(s_gridW);
        f32 normZ = (static_cast<f32>(pz) + 0.5f) / static_cast<f32>(s_gridD);

        // Flip Z: low-Z → top of minimap (high screen Y)
        f32 dotX = mapX + normX * MAP_SIZE;
        f32 dotY = mapY + (1.0f - normZ) * MAP_SIZE;

        glBindTexture(GL_TEXTURE_2D, s_whiteTex);
        if (s_minimapShader.loc_texture0 >= 0)
            glUniform1i(s_minimapShader.loc_texture0, 0);

        // -- dot --
        {
            static constexpr f32 DOT = 3.0f;
            MinimapVertex dot[6];
            dot[0] = {{dotX - DOT, dotY - DOT, 0}, {0, 0}};
            dot[1] = {{dotX + DOT, dotY - DOT, 0}, {1, 0}};
            dot[2] = {{dotX + DOT, dotY + DOT, 0}, {1, 1}};
            dot[3] = {{dotX - DOT, dotY - DOT, 0}, {0, 0}};
            dot[4] = {{dotX + DOT, dotY + DOT, 0}, {1, 1}};
            dot[5] = {{dotX - DOT, dotY + DOT, 0}, {0, 1}};

            glBufferSubData(GL_ARRAY_BUFFER, 0, 6 * sizeof(MinimapVertex), dot);

            if (s_minimapShader.loc_color >= 0)
                glUniform4f(s_minimapShader.loc_color, 0.2f, 1.0f, 0.3f, 1.0f);

            glDrawArrays(GL_TRIANGLES, 0, 6);
        }

        // -- facing arrow (thin triangle pointing in the player's yaw direction) --
        // playerYaw: 0 = +Z, increases clockwise.
        // On the minimap screen: +X = right, screen +Y = up = world -Z (because Z is flipped).
        // So arrow direction in screen space:
        //   screenDX =  sin(yaw)
        //   screenDY = -(-cos(yaw)) = cos(yaw)   ... because Z is negated when mapping to screen
        // Wait — when Z increases the dot moves DOWN on screen (since we use 1-normZ).
        // Therefore world +Z → screen -Y, so -cos(yaw) is already negated → +cos(yaw).
        {
            static constexpr f32 ARROW_LEN = 7.0f;
            static constexpr f32 ARROW_W   = 1.5f;

            f32 dirSX =  sinf(playerYaw);         // screen X component
            f32 dirSY =  cosf(playerYaw);          // screen Y component (Z flipped)

            // Tip of the arrow
            f32 tipX = dotX + dirSX * ARROW_LEN;
            f32 tipY = dotY + dirSY * ARROW_LEN;

            // Perpendicular for the base of the triangle
            f32 perpX = -dirSY * ARROW_W;
            f32 perpY =  dirSX * ARROW_W;

            MinimapVertex arrow[3];
            arrow[0] = {{dotX + perpX, dotY + perpY, 0}, {0, 0}};
            arrow[1] = {{dotX - perpX, dotY - perpY, 0}, {1, 0}};
            arrow[2] = {{tipX,         tipY,         0}, {0.5f, 1}};

            glBufferSubData(GL_ARRAY_BUFFER, 0, 3 * sizeof(MinimapVertex), arrow);

            // Slightly brighter yellow-green for the direction indicator
            if (s_minimapShader.loc_color >= 0)
                glUniform4f(s_minimapShader.loc_color, 0.8f, 1.0f, 0.2f, 1.0f);

            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
    }

    // Restore GL state
    glBindVertexArray(0);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
}
