// Squad coordinator: groups hostile entities by dungeon room and assigns
// tactical roles (RUSH/FLANK/HOLD/HARASS) when a squad is alerted. Handles
// delayed alert propagation to adjacent squads and periodic role reassignment
// as members die. Called from engine update after EnemyAI::update. See
// CLAUDE.md "How to Add Things" and entity.h SquadRole for the full design.

#include "game/squad.h"
#include <cstring>  // memset

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

// Returns the room index whose grid-cell bounds contain the entity's XZ world
// position, or -1 if no room matches. Rooms are stored in grid-cell units;
// entity positions are in metres. The level grid uses 1 m cells (cellSize=1),
// so a world-unit X maps directly to cell column, and Z to cell row.
static int findRoomForEntity(const Entity& e, const DungeonResult& dungeon) {
    // Convert world position to integer grid coords (floor, 1 m cells)
    s32 gx = static_cast<s32>(e.position.x);
    s32 gz = static_cast<s32>(e.position.z);

    for (u32 r = 0; r < dungeon.roomCount; ++r) {
        const DungeonRoom& room = dungeon.rooms[r];
        // Room occupies columns [room.x, room.x + room.w) and
        // rows [room.z, room.z + room.d)
        if (gx >= static_cast<s32>(room.x) &&
            gx <  static_cast<s32>(room.x + room.w) &&
            gz >= static_cast<s32>(room.z) &&
            gz <  static_cast<s32>(room.z + room.d))
        {
            return static_cast<int>(r);
        }
    }
    return -1;
}

// Assign squad roles to all living members based on type and attack range.
// playerPos is used for future spatial decisions; currently classification is
// purely stat-based (avoids per-frame distance math during reassignment).
static void assignRoles(Squad& squad, EntityPool& entities, Vec3 /*playerPos*/) {
    // Count how many melee rushers we've assigned so far so the first two
    // melee members get RUSH and the rest get FLANK.
    u8 meleeRushCount = 0;

    for (u8 i = 0; i < squad.memberCount; ++i) {
        u16 idx = squad.memberIndices[i];
        Entity& e = entities.entities[idx];

        // Skip dead or inactive slots — they'll be removed by onMemberDeath
        if (!(e.flags & ENT_ACTIVE) || (e.flags & ENT_DEAD)) continue;

        if (e.flags & ENT_FLYING) {
            e.squadRole = SquadRole::ROLE_HARASS;
        } else if (e.attackRange > 5.0f) {
            // Long-range fighters hold position and fire from a distance
            e.squadRole = SquadRole::ROLE_HOLD;
        } else {
            // Short-range melee: first two rush, remainder circle to flank
            if (meleeRushCount < 2) {
                e.squadRole = SquadRole::ROLE_RUSH;
                ++meleeRushCount;
            } else {
                e.squadRole = SquadRole::ROLE_FLANK;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// SquadSystem
// ---------------------------------------------------------------------------

void SquadSystem::rebuild(SquadPool& pool, const DungeonResult& dungeon,
                          EntityPool& entities)
{
    // Clear pool and size to number of rooms (one squad per room)
    memset(&pool, 0, sizeof(pool));
    pool.squadCount = dungeon.roomCount < MAX_SQUADS
                    ? dungeon.roomCount
                    : MAX_SQUADS;

    for (u32 r = 0; r < pool.squadCount; ++r) {
        pool.squads[r].roomIdx      = static_cast<u16>(r);
        pool.squads[r].memberCount  = 0;
        pool.squads[r].alerted      = false;
        pool.squads[r].alertTimer   = 0.0f;
        pool.squads[r].reassignTimer = 0.0f;
    }

    // Assign each active hostile entity to the room it currently occupies
    for (u32 i = 0; i < entities.activeCount; ++i) {
        u32 idx = entities.activeList[i];
        Entity& e = entities.entities[idx];

        if (!(e.flags & ENT_ACTIVE))   continue;
        if (e.flags & ENT_DEAD)        continue;
        if (e.flags & ENT_FRIENDLY)    continue;  // only hostile enemies join squads

        int roomIdx = findRoomForEntity(e, dungeon);
        if (roomIdx < 0 || static_cast<u32>(roomIdx) >= pool.squadCount) {
            // Entity outside any room — leave unassigned
            e.squadId = 0xFFFF;
            continue;
        }

        Squad& squad = pool.squads[roomIdx];
        if (squad.memberCount >= MAX_SQUAD_MEMBERS) continue;  // squad full

        squad.memberIndices[squad.memberCount++] = static_cast<u16>(idx);
        e.squadId = static_cast<u16>(roomIdx);
    }
}

// Forward declaration — propagateAdjacentAlert is called from both update()
// (cascade on delayed alert) and alertSquad (initial propagation).
static void propagateAdjacentAlert(SquadPool& pool, u16 sid,
                                    const DungeonResult& dungeon);

void SquadSystem::update(SquadPool& pool, const DungeonResult& dungeon,
                         EntityPool& entities, Vec3 playerPos, f32 dt)
{
    for (u32 s = 0; s < pool.squadCount; ++s) {
        Squad& squad = pool.squads[s];

        if (!squad.alerted) {
            // Countdown until this squad becomes alerted from adjacency wave
            if (squad.alertTimer > 0.0f) {
                squad.alertTimer -= dt;
                if (squad.alertTimer <= 0.0f) {
                    squad.alertTimer = 0.0f;
                    squad.alerted    = true;
                    assignRoles(squad, entities, playerPos);

                    // Cascade: newly alerted squad also alerts its own neighbors,
                    // creating a wave that propagates room-by-room through corridors
                    propagateAdjacentAlert(pool, static_cast<u16>(s), dungeon);
                }
            }
            continue;
        }

        // Alerted squad: tick reassign interval and refresh roles periodically
        // so roles update as members die or new role logic changes
        if (squad.reassignTimer > 0.0f) {
            squad.reassignTimer -= dt;
            if (squad.reassignTimer <= 0.0f) {
                squad.reassignTimer = SQUAD_REASSIGN_INTERVAL;
                assignRoles(squad, entities, playerPos);
            }
        }
    }
}

void SquadSystem::alertSquad(SquadPool& pool, u16 entityIndex,
                              EntityPool& entities,
                              const DungeonResult& dungeon)
{
    // Find the squad that owns this entity
    Entity& trigger = entities.entities[entityIndex];
    u16 sid = trigger.squadId;

    if (sid >= pool.squadCount) return;  // unassigned entity

    Squad& own = pool.squads[sid];
    if (own.alerted) return;  // already alerted — nothing to do

    // Immediately alert the entity's own squad and assign initial roles.
    // We don't have a playerPos here, so roles will be reassigned on the next
    // update tick with a real position via the reassignTimer.
    own.alerted      = true;
    own.alertTimer   = 0.0f;
    own.reassignTimer = SQUAD_REASSIGN_INTERVAL;
    assignRoles(own, entities, trigger.lastSeenPos);

    // Propagate delayed alert only to adjacent rooms (not all squads),
    // creating a realistic wave-propagation effect through connected corridors.
    propagateAdjacentAlert(pool, sid, dungeon);
}

// Alert adjacent rooms of squad sid with a delay, skipping already-alerted squads.
static void propagateAdjacentAlert(SquadPool& pool, u16 sid,
                                    const DungeonResult& dungeon)
{
    if (sid >= dungeon.roomCount) return;
    const DungeonRoom& alertRoom = dungeon.rooms[sid];

    for (u8 a = 0; a < alertRoom.adjacentCount; a++) {
        u16 adjRoomIdx = alertRoom.adjacentRooms[a];
        if (adjRoomIdx >= pool.squadCount) continue;
        Squad& neighbor = pool.squads[adjRoomIdx];
        if (neighbor.alerted) continue;
        // First alerter wins — don't reset a shorter timer already running
        if (neighbor.alertTimer <= 0.0f) {
            neighbor.alertTimer = SQUAD_ADJACENT_ALERT_DELAY;
        }
    }
}

void SquadSystem::onMemberDeath(SquadPool& pool, u16 entityIndex,
                                EntityPool& entities)
{
    Entity& dead = entities.entities[entityIndex];
    u16 sid = dead.squadId;

    if (sid >= pool.squadCount) return;

    Squad& squad = pool.squads[sid];

    // Swap-remove the dead member to keep memberIndices packed
    for (u8 i = 0; i < squad.memberCount; ++i) {
        if (squad.memberIndices[i] == entityIndex) {
            squad.memberIndices[i] = squad.memberIndices[squad.memberCount - 1];
            --squad.memberCount;
            break;
        }
    }

    // Mark entity as no longer belonging to a squad
    dead.squadId   = 0xFFFF;
    dead.squadRole = SquadRole::ROLE_NONE;

    // Force immediate role reassignment so surviving members adapt
    squad.reassignTimer = 0.0f;
}
