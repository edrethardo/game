#include "renderer/minimap.h"
#include "renderer/shader.h"
#include "core/log.h"
#include "game/zone_def.h"   // Zone::isCaveBoundary — picks the cave-mouth arch glyph
#include <glad/glad.h>
#include <cstring>
#include <cmath>

// ---------------------------------------------------------------------------
// Static state
// ---------------------------------------------------------------------------

// Visited cell tracking (fog-of-war)
// 0 = unexplored, 1 = NPC-revealed (dimmer), 2 = player-revealed (full)
static u8   s_visited[MAX_MINIMAP_CELLS] = {};
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

    // VAO / VBO — every batch below re-uploads at offset 0, so this only has to fit the LARGEST
    // single batch, not their sum. That is the 128 entity dots (128 * 6 = 768 verts); the shrine
    // icons (16 * 6 = 96) and everything else are far smaller. Round up to 896.
    static constexpr u32 MINIMAP_MAX_VERTS = 896;
    if (!s_minimapVAO) {
        glGenVertexArrays(1, &s_minimapVAO);
        glBindVertexArray(s_minimapVAO);

        glGenBuffers(1, &s_minimapVBO);
        glBindBuffer(GL_ARRAY_BUFFER, s_minimapVBO);
        glBufferData(GL_ARRAY_BUFFER, MINIMAP_MAX_VERTS * sizeof(MinimapVertex), nullptr, GL_DYNAMIC_DRAW);

        // aPos  (location 0)
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(MinimapVertex),
                              (void*)0);
        glEnableVertexAttribArray(0);

        // aUV   (location 2 — matches unlit.vert layout)
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(MinimapVertex),
                              (void*)sizeof(Vec3));
        glEnableVertexAttribArray(2);

        glBindVertexArray(0);

        s_minimapShader = ShaderSystem::load(ASSET_PATH("assets/shaders/unlit.vert"),
                                             ASSET_PATH("assets/shaders/unlit.frag"));
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
// Helper: reveal cells around a world position at given radius with given visit level
static bool revealAround(const LevelGrid& grid, Vec3 worldPos, f32 radius, u8 visitLevel) {
    u32 cx, cz;
    if (!LevelGridSystem::worldToGrid(grid, worldPos, cx, cz)) return false;

    u32 r = static_cast<u32>(radius);
    bool anyNew = false;

    for (u32 dz = 0; dz <= r * 2; dz++) {
        for (u32 dx = 0; dx <= r * 2; dx++) {
            s32 gx = static_cast<s32>(cx) - static_cast<s32>(r) + static_cast<s32>(dx);
            s32 gz = static_cast<s32>(cz) - static_cast<s32>(r) + static_cast<s32>(dz);
            if (gx < 0 || gz < 0) continue;

            u32 ugx = static_cast<u32>(gx);
            u32 ugz = static_cast<u32>(gz);
            if (!LevelGridSystem::isInBounds(grid, ugx, ugz)) continue;

            f32 distX = static_cast<f32>(gx) - static_cast<f32>(cx);
            f32 distZ = static_cast<f32>(gz) - static_cast<f32>(cz);
            if (distX * distX + distZ * distZ > radius * radius) continue;

            u32 idx = ugz * s_gridW + ugx;
            if (idx < MAX_MINIMAP_CELLS && s_visited[idx] < visitLevel) {
                s_visited[idx] = visitLevel;
                anyNew = true;
            }
        }
    }
    return anyNew;
}

void Minimap::updateVisited(const LevelGrid& grid, Vec3 playerPos,
                             const EntityPool& entities) {
    bool anyNew = false;

    // Player reveals at full visibility (level 2)
    if (revealAround(grid, playerPos, MINIMAP_REVEAL_RADIUS, 2)) anyNew = true;

    // Friendly NPCs reveal at dimmer visibility (level 1) with smaller radius
    for (u32 a = 0; a < entities.activeCount; a++) {
        u32 idx = entities.activeList[a];
        const Entity& e = entities.entities[idx];
        if (!(e.flags & ENT_FRIENDLY)) continue;
        if (e.flags & ENT_DEAD) continue;
        if (revealAround(grid, e.position, NPC_REVEAL_RADIUS, 1)) anyNew = true;
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

            u8 vis = s_visited[idx]; // 0=fog, 1=NPC-revealed (dim), 2=player-revealed

            if (vis == 0) {
                // Full fog of war
                s_pixelData[pIdx + 0] = 10;
                s_pixelData[pIdx + 1] = 10;
                s_pixelData[pIdx + 2] = 15;
                s_pixelData[pIdx + 3] = 180;
                continue;
            }

            // Dimming factor: NPC-revealed cells are shown at 50% brightness
            f32 dim = (vis == 1) ? 0.5f : 1.0f;

            const GridCell& cell = LevelGridSystem::getCell(grid, x, z);

            if (cell.flags & CELL_SOLID) {
                s_pixelData[pIdx + 0] = static_cast<u8>(55 * dim);
                s_pixelData[pIdx + 1] = static_cast<u8>(55 * dim);
                s_pixelData[pIdx + 2] = static_cast<u8>(65 * dim);
                s_pixelData[pIdx + 3] = 255;
            } else if (cell.flags & CELL_LAVA) {
                // Hellforge lava is WALKABLE (it carries CELL_FLOOR), so without its own branch it
                // would paint the same grey as safe stone and the map would route you into a lake.
                // Hot orange, checked BEFORE the floor branch.
                s_pixelData[pIdx + 0] = static_cast<u8>(225 * dim);
                s_pixelData[pIdx + 1] = static_cast<u8>(90  * dim);
                s_pixelData[pIdx + 2] = static_cast<u8>(30  * dim);
                s_pixelData[pIdx + 3] = 255;
            } else if (cell.flags & CELL_FLOOR) {
                f32 floorH = static_cast<f32>(cell.floorHeight) * 0.25f;
                u8 r, g, b;
                if (cell.wallMaterialId == 3) {
                    r = 130; g = 90; b = 70;
                } else if (floorH > 0.1f) {
                    r = 130; g = 125; b = 115;
                } else {
                    r = 110; g = 105; b = 100;
                }
                s_pixelData[pIdx + 0] = static_cast<u8>(r * dim);
                s_pixelData[pIdx + 1] = static_cast<u8>(g * dim);
                s_pixelData[pIdx + 2] = static_cast<u8>(b * dim);
                s_pixelData[pIdx + 3] = 255;
            } else {
                s_pixelData[pIdx + 0] = static_cast<u8>(5 * dim);
                s_pixelData[pIdx + 1] = static_cast<u8>(5 * dim);
                s_pixelData[pIdx + 2] = static_cast<u8>(8 * dim);
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
                   const LevelGrid& grid, Vec3 playerPos, f32 playerYaw,
                   const EntityPool& entities,
                   const Vec3* otherPlayers, const bool* otherActive,
                   u32 otherPlayerCount,
                   const WorldItemPool* worldItems,
                   u8 zoneFloor, u8 cairnMask)
{
    if (s_gridW == 0 || s_gridD == 0) return;
    if (s_dirty) rebuildTexture(grid);

    // Screen-space position: top-right corner, scaled relative to 720p
    f32 uiScale = static_cast<f32>(screenHeight) / 720.0f;
    f32 MAP_SIZE = 150.0f * uiScale;
    f32 MARGIN   = 10.0f * uiScale;
    f32 BORDER   = 2.0f * uiScale;

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
    // 1. Frame + dark background (batched into one upload, two draws)
    // ------------------------------------------------------------------
    {
        static constexpr f32 FRAME = 3.0f;
        f32 fx0 = mapX - BORDER - FRAME;
        f32 fy0 = mapY - BORDER - FRAME;
        f32 fx1 = mapX + MAP_SIZE + BORDER + FRAME;
        f32 fy1 = mapY + MAP_SIZE + BORDER + FRAME;
        f32 bx0 = mapX - BORDER;
        f32 by0 = mapY - BORDER;
        f32 bx1 = mapX + MAP_SIZE + BORDER;
        f32 by1 = mapY + MAP_SIZE + BORDER;

        // Upload both quads (frame + bg) in one call: 12 verts
        MinimapVertex frameBg[12];
        frameBg[0]  = {{fx0, fy0, 0}, {0, 0}};
        frameBg[1]  = {{fx1, fy0, 0}, {1, 0}};
        frameBg[2]  = {{fx1, fy1, 0}, {1, 1}};
        frameBg[3]  = {{fx0, fy0, 0}, {0, 0}};
        frameBg[4]  = {{fx1, fy1, 0}, {1, 1}};
        frameBg[5]  = {{fx0, fy1, 0}, {0, 1}};
        frameBg[6]  = {{bx0, by0, 0}, {0, 0}};
        frameBg[7]  = {{bx1, by0, 0}, {1, 0}};
        frameBg[8]  = {{bx1, by1, 0}, {1, 1}};
        frameBg[9]  = {{bx0, by0, 0}, {0, 0}};
        frameBg[10] = {{bx1, by1, 0}, {1, 1}};
        frameBg[11] = {{bx0, by1, 0}, {0, 1}};

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, s_whiteTex);
        if (s_minimapShader.loc_texture0 >= 0)
            glUniform1i(s_minimapShader.loc_texture0, 0);

        glBufferSubData(GL_ARRAY_BUFFER, 0, 12 * sizeof(MinimapVertex), frameBg);

        // Frame (gold)
        if (s_minimapShader.loc_color >= 0)
            glUniform4f(s_minimapShader.loc_color, 0.55f, 0.45f, 0.2f, 0.9f);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        // Background (dark) — already uploaded at offset 6
        if (s_minimapShader.loc_color >= 0)
            glUniform4f(s_minimapShader.loc_color, 0.0f, 0.0f, 0.0f, 0.85f);
        glDrawArrays(GL_TRIANGLES, 6, 6);
    }

    // ------------------------------------------------------------------
    // 2. Dungeon map quad (textured)
    // ------------------------------------------------------------------
    {
        f32 x0 = mapX, y0 = mapY;
        f32 x1 = mapX + MAP_SIZE, y1 = mapY + MAP_SIZE;

        MinimapVertex mv[6];
        mv[0] = {{x0, y0, 0}, {0, 1}};
        mv[1] = {{x1, y0, 0}, {1, 1}};
        mv[2] = {{x1, y1, 0}, {1, 0}};
        mv[3] = {{x0, y0, 0}, {0, 1}};
        mv[4] = {{x1, y1, 0}, {1, 0}};
        mv[5] = {{x0, y1, 0}, {0, 0}};

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

            // Player forward = (-sin(yaw), 0, -cos(yaw))
            // Minimap: world +X → screen +X, world -Z → screen +Y (Z flipped)
            f32 dirSX = -sinf(playerYaw);         // matches forward.x
            f32 dirSY =  cosf(playerYaw);          // -forward.z → +screen Y

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

    // ------------------------------------------------------------------
    // 4. Friendly NPC dots: batched into a single upload + draw call
    // ------------------------------------------------------------------
    {
        static constexpr f32 NPC_DOT = 2.0f;
        static constexpr u32 MAX_NPC_DOTS = 128;
        MinimapVertex npcBatch[MAX_NPC_DOTS * 6];
        u32 npcVertCount = 0;

        for (u32 a = 0; a < entities.activeCount; a++) {
            u32 eidx = entities.activeList[a];
            const Entity& ent = entities.entities[eidx];
            if (!(ent.flags & ENT_FRIENDLY)) continue;
            if (ent.flags & ENT_DEAD) continue;

            u32 nx, nz;
            if (!LevelGridSystem::worldToGrid(grid, ent.position, nx, nz)) continue;

            f32 normX = (static_cast<f32>(nx) + 0.5f) / static_cast<f32>(s_gridW);
            f32 normZ = (static_cast<f32>(nz) + 0.5f) / static_cast<f32>(s_gridD);

            f32 ndX = mapX + normX * MAP_SIZE;
            f32 ndY = mapY + (1.0f - normZ) * MAP_SIZE;

            if (npcVertCount + 6 > MAX_NPC_DOTS * 6) break;
            npcBatch[npcVertCount++] = {{ndX - NPC_DOT, ndY - NPC_DOT, 0}, {0, 0}};
            npcBatch[npcVertCount++] = {{ndX + NPC_DOT, ndY - NPC_DOT, 0}, {1, 0}};
            npcBatch[npcVertCount++] = {{ndX + NPC_DOT, ndY + NPC_DOT, 0}, {1, 1}};
            npcBatch[npcVertCount++] = {{ndX - NPC_DOT, ndY - NPC_DOT, 0}, {0, 0}};
            npcBatch[npcVertCount++] = {{ndX + NPC_DOT, ndY + NPC_DOT, 0}, {1, 1}};
            npcBatch[npcVertCount++] = {{ndX - NPC_DOT, ndY + NPC_DOT, 0}, {0, 1}};
        }

        if (npcVertCount > 0) {
            glBindTexture(GL_TEXTURE_2D, s_whiteTex);
            if (s_minimapShader.loc_color >= 0)
                glUniform4f(s_minimapShader.loc_color, 0.4f, 0.9f, 0.5f, 0.85f);
            glBufferSubData(GL_ARRAY_BUFFER, 0, npcVertCount * sizeof(MinimapVertex), npcBatch);
            glDrawArrays(GL_TRIANGLES, 0, npcVertCount);
        }
    }

    // ------------------------------------------------------------------
    // 5. Remote co-op player dots (cyan, slightly larger than NPC dots).
    //    Parallel arrays; the caller has already marked the local slot(s) inactive so
    //    the local green marker is never duplicated. Same world→minimap mapping as the
    //    player/NPC markers (flip Z: low-Z → top). Batched into one upload + draw.
    // ------------------------------------------------------------------
    if (otherPlayers && otherActive && otherPlayerCount > 0) {
        static constexpr f32 PLR_DOT      = 2.5f;
        static constexpr u32 MAX_PLR_DOTS = 8;   // MAX_PLAYERS is 4; headroom, stack-safe
        MinimapVertex plrBatch[MAX_PLR_DOTS * 6];
        u32 vc = 0;

        for (u32 i = 0; i < otherPlayerCount && vc + 6 <= MAX_PLR_DOTS * 6; i++) {
            if (!otherActive[i]) continue;

            u32 nx, nz;
            if (!LevelGridSystem::worldToGrid(grid, otherPlayers[i], nx, nz)) continue;

            f32 normX = (static_cast<f32>(nx) + 0.5f) / static_cast<f32>(s_gridW);
            f32 normZ = (static_cast<f32>(nz) + 0.5f) / static_cast<f32>(s_gridD);
            f32 pdX = mapX + normX * MAP_SIZE;
            f32 pdY = mapY + (1.0f - normZ) * MAP_SIZE;

            plrBatch[vc++] = {{pdX - PLR_DOT, pdY - PLR_DOT, 0}, {0, 0}};
            plrBatch[vc++] = {{pdX + PLR_DOT, pdY - PLR_DOT, 0}, {1, 0}};
            plrBatch[vc++] = {{pdX + PLR_DOT, pdY + PLR_DOT, 0}, {1, 1}};
            plrBatch[vc++] = {{pdX - PLR_DOT, pdY - PLR_DOT, 0}, {0, 0}};
            plrBatch[vc++] = {{pdX + PLR_DOT, pdY + PLR_DOT, 0}, {1, 1}};
            plrBatch[vc++] = {{pdX - PLR_DOT, pdY + PLR_DOT, 0}, {0, 1}};
        }

        if (vc > 0) {
            glBindTexture(GL_TEXTURE_2D, s_whiteTex);
            if (s_minimapShader.loc_color >= 0)
                glUniform4f(s_minimapShader.loc_color, 0.3f, 0.7f, 1.0f, 1.0f); // cyan teammates
            glBufferSubData(GL_ARRAY_BUFFER, 0, vc * sizeof(MinimapVertex), plrBatch);
            glDrawArrays(GL_TRIANGLES, 0, vc);
        }
    }

    // ------------------------------------------------------------------
    // 6. Shrine icons — a colour-coded DIAMOND, drawn last so it sits on top.
    //
    //    Shape carries "shrine" and colour carries "which": every other marker on this map is an
    //    axis-aligned square (the player's arrow being the one triangle), so a 45°-rotated diamond
    //    with a white core reads as its own class of thing at 9 px, even for a player who cannot
    //    separate the red one from the green one. The colours are Shrine::colorOf — the same values
    //    the crystal is tinted with in the world, so the map teaches the room and vice versa.
    //
    //    FOG GATE: a shrine is only drawn once its cell has been revealed. Otherwise the minimap
    //    would hand the player a map of things they have not found — the same wallhack the target
    //    bar had. NPC-revealed cells (visit level 1) show it dimmer, matching the map's own
    //    convention that second-hand knowledge is less certain than what you saw yourself.
    //
    //    Deliberately NOT pulsing: this project holds a no-flashing rule (WCAG 2.3.1), and a
    //    steady icon is just as findable.
    // ------------------------------------------------------------------
    if (worldItems) {
        static constexpr f32 SHRINE_R    = 4.5f;   // outer diamond half-extent, px
        static constexpr f32 SHRINE_CORE = 1.6f;   // white centre pip half-extent, px
        static constexpr u32 MAX_SHRINE_ICONS = 16;   // shrines (2/floor) + a zone's waypoint, its gates and its 5 Cairn Stones

        // Emit the two triangles of a diamond centred on (cx, cy).
        //
        // WINDING IS LOAD-BEARING: GL_CULL_FACE is enabled globally and this function never turns it
        // off, so the vertices must run COUNTER-clockwise (right → top → left → bottom) exactly like
        // every other quad in this file. Wound the intuitive way — clockwise from the top — both
        // triangles are back-faces and the icon is silently culled to nothing.
        auto emitDiamond = [](MinimapVertex* v, f32 cx, f32 cy, f32 r) {
            const Vec3 right  = {cx + r, cy,     0};
            const Vec3 top    = {cx,     cy + r, 0};
            const Vec3 left   = {cx - r, cy,     0};
            const Vec3 bottom = {cx,     cy - r, 0};
            v[0] = {right,  {1.0f, 0.5f}};
            v[1] = {top,    {0.5f, 1.0f}};
            v[2] = {left,   {0.0f, 0.5f}};
            v[3] = {right,  {1.0f, 0.5f}};
            v[4] = {left,   {0.0f, 0.5f}};
            v[5] = {bottom, {0.5f, 0.0f}};
        };

        // A CAVE MOUTH gets an ARCH, not a diamond — Diablo 2's automap marks a level entrance with
        // a little gateway glyph, and it works because the shape says "you go THROUGH this" where a
        // dot only says "something is here". Two legs and a lintel: a doorway in silhouette, which
        // is the same thing the model in the world is.
        //
        // Three axis-aligned quads, 18 verts. WINDING IS LOAD-BEARING here exactly as it is for the
        // diamond above — GL_CULL_FACE is on and never disabled — so each quad runs so that its
        // triangles have a positive cross product, matching emitDiamond's right -> top -> left.
        auto emitArch = [](MinimapVertex* v, f32 cx, f32 cy, f32 r) {
            const f32 t    = r * 0.42f;          // leg thickness / lintel depth
            const f32 span = r * 0.30f;          // height at which the legs meet the lintel
            u32 n = 0;
            auto quad = [&](f32 x0, f32 y0, f32 x1, f32 y1) {
                v[n+0] = {{x0, y0, 0}, {0.0f, 0.0f}};
                v[n+1] = {{x1, y0, 0}, {1.0f, 0.0f}};
                v[n+2] = {{x1, y1, 0}, {1.0f, 1.0f}};
                v[n+3] = {{x0, y0, 0}, {0.0f, 0.0f}};
                v[n+4] = {{x1, y1, 0}, {1.0f, 1.0f}};
                v[n+5] = {{x0, y1, 0}, {0.0f, 1.0f}};
                n += 6;
            };
            quad(cx - r,     cy - r,    cx - r + t, cy + span);   // left jamb
            quad(cx + r - t, cy - r,    cx + r,     cy + span);   // right jamb
            quad(cx - r,     cy + span, cx + r,     cy + r);      // lintel
        };
        // THE CAIRN STONES — five dots on a ring, the way the stones stand in the field.
        //
        // Drawn as separate marks rather than an outlined circle on purpose: at this size a thin
        // ring outline turns into a smudged blob, while five distinct dots stay countable and read
        // as ARRANGED — which is the whole point of the landmark, and of its quest.
        // Offsets are a hand-written unit ring (integer-free trig would be overkill for five fixed
        // points, and hard-coding them keeps this a pure lookup with no libm call per frame).
        auto emitRing = [](MinimapVertex* v, f32 cx, f32 cy, f32 r) {
            static const f32 OFF[5][2] = {
                { 0.00f,  1.00f}, { 0.95f,  0.31f}, { 0.59f, -0.81f},
                {-0.59f, -0.81f}, {-0.95f,  0.31f},
            };
            const f32 dot = r * 0.40f;           // each stone
            const f32 ring = r * 0.78f;          // how far out they stand
            u32 n = 0;
            for (u32 i = 0; i < 5; i++) {
                const f32 x = cx + OFF[i][0] * ring, y = cy + OFF[i][1] * ring;
                // Same right -> top -> left order as emitDiamond: GL_CULL_FACE is on and never
                // disabled, so a reversed winding silently draws nothing.
                v[n+0] = {{x + dot, y,       0}, {1.0f, 0.5f}};
                v[n+1] = {{x,       y + dot, 0}, {0.5f, 1.0f}};
                v[n+2] = {{x - dot, y,       0}, {0.0f, 0.5f}};
                v[n+3] = {{x + dot, y,       0}, {1.0f, 0.5f}};
                v[n+4] = {{x - dot, y,       0}, {0.0f, 0.5f}};
                v[n+5] = {{x,       y - dot, 0}, {0.5f, 0.0f}};
                n += 6;
            }
        };
        static constexpr u32 RING_VERTS = 30;

        // THE RIFT — a tall narrow tear, pointed at both ends.
        //
        // Deliberately NOT an arch: an arch is a way through that was always there, and the Hellgate
        // is a wound forced open. Narrow-and-pointed also stays distinct from the diamond at a
        // glance, which matters because both are lit warm.
        auto emitRift = [](MinimapVertex* v, f32 cx, f32 cy, f32 r) {
            const f32 w = r * 0.34f;             // half-width at the waist
            v[0] = {{cx + w, cy,     0}, {1.0f, 0.5f}};
            v[1] = {{cx,     cy + r, 0}, {0.5f, 1.0f}};
            v[2] = {{cx - w, cy,     0}, {0.0f, 0.5f}};
            v[3] = {{cx + w, cy,     0}, {1.0f, 0.5f}};
            v[4] = {{cx - w, cy,     0}, {0.0f, 0.5f}};
            v[5] = {{cx,     cy - r, 0}, {0.5f, 0.0f}};
        };
        static constexpr u32 RIFT_VERTS = 6;

        static constexpr u32 ARCH_VERTS = 18;
        // Sized to the LARGEST glyph, derived rather than typed: the ring is 30 verts and a buffer
        // sized to the arch's 18 would overrun it.
        static constexpr u32 GLYPH_MAX_VERTS =
            (RING_VERTS > ARCH_VERTS ? RING_VERTS : ARCH_VERTS);

        MinimapVertex coreBatch[MAX_SHRINE_ICONS * 6];
        u32 coreVerts = 0;

        glBindTexture(GL_TEXTURE_2D, s_whiteTex);
        if (s_minimapShader.loc_texture0 >= 0)
            glUniform1i(s_minimapShader.loc_texture0, 0);

        u32 drawn = 0;
        for (u32 i = 0; i < MAX_WORLD_ITEMS && drawn < MAX_SHRINE_ICONS; i++) {
            const WorldItem& wi = worldItems->items[i];
            // Shrines, plus the overworld's two permanent FIXTURES. The acts put a zone's POI mouth
            // on a room centre that can be a long way from where you walk in — the Den of Evil's
            // gate generates in a far corner of the Blood Buffer — and with nothing on the map it is
            // simply not findable in open terrain. Reported as "I couldn't find the Den of Evil".
            const bool shrine  = isShrine(wi.item);
            const bool waypnt  = isWaypoint(wi.item);
            const bool zgate   = isZoneGate(wi.item);
            // The CAIRN STONES are furniture too, and more so than most: the quest is to visit all
            // five, so "which have I done" is a question only the map can answer at a glance in a
            // 52-cell field of open country.
            const bool cairn   = isCairnStone(wi.item);
            if (!wi.active || !(shrine || waypnt || zgate || cairn)) continue;

            u32 sx, sz;
            if (!LevelGridSystem::worldToGrid(grid, wi.position, sx, sz)) continue;

            const u32 cellIdx = sz * s_gridW + sx;
            if (cellIdx >= MAX_MINIMAP_CELLS) continue;
            const u8 vis = s_visited[cellIdx];
            // A SHRINE stays hidden until you have been there — it is a reward you find. A FIXTURE
            // does not: a gate and a waypoint are the map's furniture, the things a zone is FOR, and
            // hiding them turns "where is the Den" into a search of open ground with no cue at all.
            // They still dim while unexplored, so the map keeps saying what you have and have not
            // walked, which is the same convention second-hand knowledge already uses here.
            if (vis == 0 && shrine) continue;
            f32 alpha = (vis == 0) ? 0.40f : (vis == 1) ? 0.55f : 1.0f;
            // An ALIGNED stone burns bright and an unaligned one stays dim, whatever the fog says —
            // the state of the circle is the quest, so it must not be readable only where you have
            // already walked.
            const bool cairnLit = cairn && (cairnMask & (1u << wi.item.affixCount)) != 0;
            if (cairn) alpha = cairnLit ? 1.0f : 0.45f;

            f32 normX = (static_cast<f32>(sx) + 0.5f) / static_cast<f32>(s_gridW);
            f32 normZ = (static_cast<f32>(sz) + 0.5f) / static_cast<f32>(s_gridD);
            f32 icoX  = mapX + normX * MAP_SIZE;
            f32 icoY  = mapY + (1.0f - normZ) * MAP_SIZE;   // flip Z, as every other marker does

            // Outer diamond: one draw per shrine, because each carries its own colour uniform.
            // MAX_PER_FLOOR is 2, so this is at most 2 extra draw calls against a 300-500 budget.
            // Colour carries WHICH, shape carries WHAT — the rule this icon block already follows.
            // A gate is the warm orange of a way through; a waypoint the same blue its travel list
            // and its discovery message use, so the three readings agree.
            // A cave mouth is grey stone rather than the warm orange of a portal, matching the model
            // it marks — the destination's terrain decides, via the same predicate the world
            // renderer uses, so the icon and the thing it points at can never disagree.
            // The SAME predicate the world renderer picks the mesh with, so the shape on the map
            // and the thing standing in the field can never disagree about what kind of way this is.
            const Zone::Entrance ekind =
                zgate ? Zone::entranceFor(zoneFloor, static_cast<u8>(wi.item.itemLevel))
                      : Zone::Entrance::STONE;
            // SHAPE carries WHAT, COLOUR carries WHICH — the rule this icon block already followed.
            // Everything you WALK INTO keeps the arch (a cave, a cemetery gate, a stair mouth, a
            // service door) and is told apart by colour; the two that are not doorways at all get
            // their own shapes, because no amount of tinting would make a ring of standing stones
            // or a torn rift read as a door.
            const bool stones = cairn || (zgate && ekind == Zone::Entrance::STONES);
            const bool rift   = zgate && ekind == Zone::Entrance::HELLGATE;
            const bool cave   = zgate && !stones && !rift && ekind != Zone::Entrance::STONE;
            Vec3 c;
            switch (ekind) {
                case Zone::Entrance::CAVE:     c = {0.72f, 0.72f, 0.76f}; break;  // bare rock
                case Zone::Entrance::STONES:   c = {0.78f, 0.74f, 0.62f}; break;  // weathered granite
                case Zone::Entrance::HELLGATE: c = {0.90f, 0.28f, 0.18f}; break;  // ember, the only red
                case Zone::Entrance::GRAVE:    c = {0.55f, 0.62f, 0.55f}; break;  // mossy iron
                case Zone::Entrance::TUBE:     c = {0.30f, 0.55f, 0.95f}; break;  // Underground blue
                case Zone::Entrance::DOOR:     c = {0.62f, 0.66f, 0.60f}; break;  // painted steel
                default:
                    // A stone is pale blue — the same family as the waypoint's blue and the chat
                    // line the quest prints, and deliberately NOT the granite tan of the stone
                    // CIRCLE that leads to TristRAM, which stands in this same field.
                    c = cairn  ? Vec3{0.72f, 0.82f, 1.00f}
                      : zgate  ? Vec3{0.95f, 0.55f, 0.20f}
                      : waypnt ? Vec3{0.55f, 0.85f, 1.00f}
                               : Shrine::colorOf(Shrine::buffOf(wi.item));
                    break;
            }
            MinimapVertex glyph[GLYPH_MAX_VERTS];
            const u32 glyphVerts = cave ? ARCH_VERTS : stones ? RING_VERTS : rift ? RIFT_VERTS : 6u;
            if      (cave)   emitArch(glyph, icoX, icoY, SHRINE_R);
            else if (stones) emitRing(glyph, icoX, icoY, SHRINE_R);
            else if (rift)   emitRift(glyph, icoX, icoY, SHRINE_R);
            else             emitDiamond(glyph, icoX, icoY, SHRINE_R);
            glBufferSubData(GL_ARRAY_BUFFER, 0, glyphVerts * sizeof(MinimapVertex), glyph);
            if (s_minimapShader.loc_color >= 0)
                glUniform4f(s_minimapShader.loc_color, c.x, c.y, c.z, alpha);
            glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(glyphVerts));

            // Centre pip — same white for all, so every shrine's core batches into one draw. The
            // arch deliberately has NO pip: its opening is the read, and a dot in the doorway fills
            // in the one part of the glyph that says "way through".
            // The three ENTRANCE glyphs carry no pip: the arch's opening, the ring's empty middle
            // and the rift's slot are each the read, and a dot dropped in the centre fills in
            // exactly the part that says "way through".
            if (!cave && !stones && !rift) {
                emitDiamond(&coreBatch[coreVerts], icoX, icoY, SHRINE_CORE);
                coreVerts += 6;
            }
            drawn++;
        }

        if (coreVerts > 0) {
            glBufferSubData(GL_ARRAY_BUFFER, 0, coreVerts * sizeof(MinimapVertex), coreBatch);
            if (s_minimapShader.loc_color >= 0)
                glUniform4f(s_minimapShader.loc_color, 1.0f, 1.0f, 1.0f, 0.95f);
            glDrawArrays(GL_TRIANGLES, 0, coreVerts);
        }
    }

    // Restore GL state
    glBindVertexArray(0);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
}
