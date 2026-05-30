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
}
