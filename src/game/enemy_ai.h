#pragma once

#include "core/types.h"
#include "game/entity.h"
#include "game/projectile.h"
#include "game/squad.h"
#include "world/level_grid.h"
#include "world/level_gen.h"
#include "world/pathfinder.h"

struct Player;
struct BossDefTable;
struct NetPlayer;

namespace EnemyAI {
    // Update all enemy AI: FSM transitions, movement, attacks.
    static constexpr u32 MAX_AI_TARGETS = 4;
    // `netPlayers`/`netPlayerCount` are used by friendly-NPC tether code (N4) so
    // remote-cast minions follow their caster's NetPlayer instead of the host. Pass
    // m_players + MAX_PLAYERS on the SERVER tick; leave null for SP/split-screen.
    void update(EntityPool& pool, const LevelGrid& grid,
                Player& player, ProjectilePool& projectiles, f32 dt,
                SquadPool* squads = nullptr,
                Player** extraPlayers = nullptr, u32 extraPlayerCount = 0,
                const DungeonResult* dungeon = nullptr,
                bool spawnCalm = false,    // true = floor-start calm: no auto-aggro, friendly NPCs hold
                const NetPlayer* netPlayers = nullptr, u32 netPlayerCount = 0);

    // Set drone spawn callback for Swarm Queen auto-spawning
    void setDroneSpawnCallback(void(*cb)(Vec3 pos, u8 type));

    // Set boss def table for personality-driven boss AI delegation
    void setBossDefTable(const BossDefTable* table);

    // Set resolved skeleton mesh/material IDs for boss skeleton-summon abilities
    void setSkeletonVisuals(u8 meshId, u8 matId);

    // Spring a dormant disguised ambusher (mimic "chest" / stone gargoyle) into combat: the
    // ONE wake routine, so the chomp/wake presentation can't drift between the DORMANT state's
    // weeping-angel trigger and the Engine's E-interact path (CL_INTERACT_ENTITY lands here
    // too). No-op unless the entity is actually DORMANT. Server/SP only — on guests aiState
    // is replicated, never locally decided.
    void wakeAmbusher(Entity& e);

    // Pure view-cone half of the weeping-angel wake rule ("dormant ambushers stir only while
    // unobserved"). Forward derives from yaw/pitch with the SAME convention as the server's
    // aim vector (engine.cpp: yaw 0 faces -Z, positive pitch looks up); Player.forward itself
    // is deliberately NOT trusted — remote views (seedRemoteView) never seed it. Inline and
    // parameter-pure so tests/game/test_dormant_watch.cpp can pin the convention without
    // linking the AI. The caller pairs this with an LOS check: a cone hit through a wall is
    // not watching.
    inline bool inViewCone(Vec3 eye, f32 yaw, f32 pitch, Vec3 point, f32 cosHalfAngle) {
        Vec3 to = point - eye;
        f32 len = length(to);
        if (len < 0.5f) return true;   // effectively standing inside it — always "seen"
        Vec3 fwd = { -sinf(yaw) * cosf(pitch), sinf(pitch), -cosf(yaw) * cosf(pitch) };
        return dot(fwd, to * (1.0f / len)) >= cosHalfAngle;
    }
}
