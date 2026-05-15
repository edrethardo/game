#pragma once

#include "core/types.h"
#include "core/math.h"
#include "game/entity.h"
#include "world/level_gen.h"

static constexpr u32 MAX_SQUADS           = 32;
static constexpr u8  MAX_SQUAD_MEMBERS    = 8;
static constexpr f32 SQUAD_REASSIGN_INTERVAL    = 3.0f;
static constexpr f32 SQUAD_ADJACENT_ALERT_DELAY = 2.0f;

// A squad groups the hostile entities occupying a single dungeon room.
// On alert, roles are assigned (RUSH/FLANK/HOLD/HARASS) and periodically
// reassigned as members die. Adjacent squads receive a delayed alert.
struct Squad {
    u16  roomIdx;                        // index into DungeonResult::rooms
    u16  memberIndices[MAX_SQUAD_MEMBERS]; // indices into EntityPool::entities
    u8   memberCount;
    bool alerted;
    f32  alertTimer;     // > 0 while countdown before adjacent rooms are marked alerted
    f32  reassignTimer;  // counts down to 0 to trigger periodic role reassignment
};

struct SquadPool {
    Squad squads[MAX_SQUADS];
    u32   squadCount;
};

namespace SquadSystem {
    // Rebuild squads from current dungeon layout. Called once per floor.
    void rebuild(SquadPool& pool, const DungeonResult& dungeon, EntityPool& entities);

    // Per-frame update: alert propagation and periodic role reassignment.
    void update(SquadPool& pool, const DungeonResult& dungeon,
                EntityPool& entities, Vec3 playerPos, f32 dt);

    // Alert the squad containing this entity. Called when an enemy detects the player.
    // dungeon is needed for adjacency-based propagation.
    void alertSquad(SquadPool& pool, u16 entityIndex, EntityPool& entities,
                    const DungeonResult& dungeon);

    // Remove dead member and force role reassignment next tick.
    void onMemberDeath(SquadPool& pool, u16 entityIndex, EntityPool& entities);
}
