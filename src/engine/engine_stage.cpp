// engine_stage.cpp — trailer STAGING doors (WB-266/267: the Cinematic and Game trailers).
//
// A stage is a world entered purely to be filmed: deterministic geometry, no quests, no
// progression, built for --record and the cinematic camera. The first stage is the CHAKRAM ROOM —
// the trailers' signature shot: a sealed stone room with dozens of Infinity Chakrams in flight at
// once. The weapon is real (items.json `infiniteFlight`): the disc never expires and ricochets
// forever, so the room only ever gets busier.
//
// SP-only dev doors, like --vhall/--lava: never reachable in normal play, no save or wire change.
// A guest cannot follow a stage (the seed broadcast would build a dungeon), so these are not
// entered while hosting a session — the launch path runs long before any join.
#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "engine/engine.h"
#include "renderer/material.h"
#include "world/level_gen.h"
#include "world/level_mesh.h"
#include "renderer/minimap.h"
#include "game/projectile.h"
#include "core/log.h"

#include <cmath>   // cos/sin — the disc scatter

namespace {
// Deterministic direction scatter — records must be reproducible, so no rand().
// Plain LCG; only ever used for stage dressing, never simulation.
struct StageRng {
    u32 s;
    explicit StageRng(u32 seed) : s(seed ? seed : 1) {}
    u32 next() { s = s * 1664525u + 1013904223u; return s; }
    f32 frac() { return static_cast<f32>(next() >> 8) / 16777216.0f; }   // [0,1)
};
} // namespace

// The room: a 28x28 sealed box, open sky (the mesher builds no lid without CELL_CEILING, and
// daylight is what makes the discs read at speed), stone walls high enough that every ricochet
// stays in frame, and four pillars so the flight paths cross instead of settling into laps.
Vec3 Engine::buildChakramRoom() {
    constexpr u32 W = 28, D = 28;
    LevelGridSystem::init(m_level.grid, W, D, 1.0f);

    const u8 stone = MaterialSystem::getIdByName("stone_wall");
    const u8 floor = MaterialSystem::getIdByName("stone_floor");
    for (u32 z = 0; z < D; z++) {
        for (u32 x = 0; x < W; x++) {
            GridCell& c = LevelGridSystem::getCell(m_level.grid, x, z);
            const bool border = (x == 0 || z == 0 || x == W - 1 || z == D - 1);
            if (border) {
                c.flags = CELL_SOLID;
                c.wallMaterialId = stone;
            } else {
                c.flags = CELL_FLOOR;
                c.floorHeight = 0;
                // 16 qu = 4 m of wall: a disc flying at head height ricochets well below the rim,
                // so the whole bounce stays inside a ground-level camera's frame.
                c.ceilingHeight = 16;
                c.floorMaterialId = floor;
                c.wallMaterialId  = stone;
            }
        }
    }
    // Four pillars, symmetric about the centre: ricochet geometry. Without them the discs settle
    // into wall-to-wall laps; with them the paths cross mid-room, which is the whole shot.
    auto pillar = [&](u32 px, u32 pz) {
        for (u32 z = pz; z < pz + 2; z++)
            for (u32 x = px; x < px + 2; x++) {
                GridCell& c = LevelGridSystem::getCell(m_level.grid, x, z);
                c.flags = CELL_SOLID;
                c.wallMaterialId = stone;
            }
    };
    pillar(8, 8); pillar(18, 8); pillar(8, 18); pillar(18, 18);

    const Vec3 center = { static_cast<f32>(W) * 0.5f, 0.0f, static_cast<f32>(D) * 0.5f };
    m_level.sectionCount = LevelMeshSystem::buildAll(m_level.grid,
                             0xC4A6BA11u,   // constant seed — deterministic tile shading, same take twice
                             m_level.sections, MAX_LEVEL_SECTIONS);
    LevelGridSystem::buildClearanceField(m_level.grid);
    Minimap::init(m_level.grid.width, m_level.grid.depth);
    LevelGridSystem::buildFlowField(m_level.grid, center);
    return center;
}

// --chakram-room <N>: enter the stage and fill the air. Mirrors enterTown's entry ritual —
// every step here has shipped a bug when some entry point forgot it (see engine_world.cpp).
void Engine::enterChakramRoom(u32 discs) {
    worldResetPools();
    const Vec3 center = buildChakramRoom();
    worldClearLevelFlags();
    // Outdoors-lit: the stage has no ceiling, and the dungeon themes light ~8 m around the player
    // and leave the rest BLACK — the first take of this room came back as a night scene. The flag
    // routes the render through the town/arena daylight branch (sun + high ambient + sky clear).
    m_level.inStage = true;

    // South of centre, facing the room (yaw 0 faces -Z): the player IS the gameplay-trailer
    // camera, standing inside the storm; the cinematic take orbits from outside the player.
    const Vec3 base = {center.x, 0.0f, center.z + 8.0f};
    worldPlaceLocalPlayers(base, 0.0f);
    worldSeedHostSlot();
    worldSeatNetPlayers(base);

    // The discs. Real Infinity Chakram semantics, straight from the weapon-fire path
    // (engine_combat.cpp): PROJ_BOUNCE + PROJ_INFINITE_BOUNCE, lifetime 0 (age counts up),
    // fromPlayer=true — so they hit enemies if any are staged in, and in an empty room they
    // fly until the take ends. Spawned BELOW the per-owner Infinity cap (64) so a player
    // throwing more on top behaves exactly as in play: the oldest disc retires.
    if (discs > 60) discs = 60;
    const u8 mesh = findMeshByName("infinity_chakram");
    StageRng rng(0xC4A6BA11u);   // fixed seed: the same take twice is the same take
    u32 spawned = 0;
    for (u32 i = 0; i < discs; i++) {
        // Ring positions between the pillars, heights scattered through the camera band.
        const f32 ang = rng.frac() * 6.2831853f;
        const f32 rad = 3.0f + rng.frac() * 8.0f;
        const Vec3 pos = { center.x + std::cos(ang) * rad,
                           0.8f + rng.frac() * 1.6f,
                           center.z + std::sin(ang) * rad };
        // Flat flight, tangential-ish scatter so the first seconds already cross paths.
        const f32 dirA = ang + 1.5707963f + (rng.frac() - 0.5f) * 1.2f;
        const Vec3 dir = { std::cos(dirA), 0.0f, std::sin(dirA) };

        const u16 idx = ProjectileSystem::spawn(m_projectiles, pos, dir,
                                                /*speed*/ 16.0f, /*damage*/ 70.0f,
                                                /*radius*/ 0.12f, /*lifetime*/ 0.0f,
                                                /*fromPlayer*/ true,
                                                PROJ_BOUNCE | PROJ_INFINITE_BOUNCE);
        if (idx == 0xFFFF) break;   // pool full — never silently half-arm the stage
        Projectile& p = m_projectiles.projectiles[idx];
        if (mesh > 0) p.meshId = mesh;
        p.bouncesLeft = 3;          // unused under INFINITE_BOUNCE, but never left uninitialised
        p.ownerSlot   = 0;          // the host player owns the storm
        spawned++;
    }

    worldFinishEntry(/*sentinelFloor=*/1, /*peaceful=*/true);
    LOG_INFO("Launch: --chakram-room armed — %u Infinity Chakrams in flight (of %u asked)",
             spawned, discs);
}
