#pragma once
// Internal header for enemy_ai subsystem.
// Shared helpers and types used across the split .cpp files.
// Never include from outside game/enemy_ai*.cpp — public API is enemy_ai.h.

#include "core/types.h"
#include "game/entity.h"
#include "game/enemy_ai.h"   // MAX_AI_TARGETS (sizes the watch set) + inViewCone
#include "game/player.h"
#include "game/projectile.h"
#include "game/squad.h"
#include "world/level_grid.h"
#include "world/level_gen.h"
#include "world/pathfinder.h"
#include "game/combat.h"
#include "game/boss_def.h"
#include "game/game_constants.h"
#include "world/combat_query.h"
#include "world/raycast.h"
#include "world/collision.h"

// ---------------------------------------------------------------------------
// Per-entity loop action return type
// ---------------------------------------------------------------------------
// Each extracted block that originally contained a loop-level `continue` or
// `break` returns an AIStep value instead.
//   NextEntity  == original `continue`  (skip to next entity)
//   BreakLoop   == original outer `break` (exit entity loop entirely)
//   Continue    == normal fall-through (keep processing this entity)
enum class AIStep : u8 { Continue, NextEntity, BreakLoop };

// ---------------------------------------------------------------------------
// File-scope statics (defined in enemy_ai.cpp, shared with extracted files)
// ---------------------------------------------------------------------------
// Frame counter for staggering expensive per-entity work
extern u32 s_frameTick;
// Pre-computed friendly group center and count (for archer kiting logic)
extern Vec3 s_friendlyGroupCenter;
extern u32  s_friendlyGroupCount;
// Drone spawn callback (set by Engine so Swarm Queen can spawn mini drones)
extern void(*s_droneSpawnCb)(Vec3 pos, u8 type);
// Boss personality table (set by Engine during init)
extern const BossDefTable* s_bossDefTable;
// Skeleton minion visuals (resolved by Engine at init) for boss summon abilities
extern u8 s_skeletonMeshId;
extern u8 s_skeletonMatId;

// ---------------------------------------------------------------------------
// Dormant-disguise watch set (weeping-angel wake rule)
// ---------------------------------------------------------------------------
// Every player view the DORMANT wake rule must consider: the primary plus the extras
// (split-screen partners, or on SERVER the remote-player views — seedRemoteView mirrors
// yaw/pitch/eyeHeight/smokeTimer, which is everything the checks below read). Rebuilt at
// the top of every EnemyAI::update. The DORMANT state can't take these as parameters —
// updateHostileStates predates extras and threading them through its 20-arg signature
// buys nothing — so they ride file-scope like s_frameTick and friends.
static constexpr u32 MAX_WATCH_PLAYERS = 1 + EnemyAI::MAX_AI_TARGETS;
// Half-angle of the "being watched" cone, as a cosine. 0.5 (~60°) is deliberately WIDE:
// anything the player can plausibly still see on screen counts as watched, so a wake can
// only ever read as "the moment I looked away" — never "it moved while I could see it".
static constexpr f32 WATCH_CONE_COS = 0.5f;
extern const Player* s_watchPlayers[MAX_WATCH_PLAYERS];
extern u32 s_watchPlayerCount;

// True if any living player has `point` inside their view cone WITH line of sight —
// staring at the wall in front of a hidden statue is not watching it. (enemy_ai.cpp)
bool anyPlayerWatching(Vec3 point, const LevelGrid& grid);
// True if any living, un-smoked player is within `range` (XZ) of `pos`. Smoke-stealthed
// players don't provoke a dormant ambusher, mirroring the old per-target smokeTimer gate.
// (enemy_ai.cpp)
bool anyPlayerWithin(Vec3 pos, f32 range);

// ---------------------------------------------------------------------------
// Grid/movement helpers (defined in enemy_ai.cpp)
// ---------------------------------------------------------------------------

// Returns true if the AABB at `centre`/`halfExtents` overlaps any solid cell.
bool entityOverlapsGrid(Vec3 centre, Vec3 halfExtents, const LevelGrid& grid);

// Grid collision / navigation radius for an entity, CAPPED so an oversized body
// (a boss — the Butcher is 0.8 m half-width) still fits the 1 m cell grid: it
// collides, paths, and unwedges as a ~0.9 m body and slides along walls instead
// of jamming in corners, while its full halfExtents stay intact for hit detection
// and rendering. Normal enemies/NPCs (<= the cap) are unaffected — navRadius is a
// no-op for them. A body wider than ~1 cell can't be exactly grid-navigated, so
// the visual model is allowed to clip wall faces slightly; never getting stuck is
// the right trade for a chunky melee boss.
static constexpr f32 ENTITY_NAV_RADIUS_CAP = 0.45f;
inline f32 navRadius(const Entity& e) {
    return e.halfExtents.x < ENTITY_NAV_RADIUS_CAP ? e.halfExtents.x : ENTITY_NAV_RADIUS_CAP;
}
inline Vec3 navExtents(const Entity& e) {
    f32 r = navRadius(e);
    return { r, e.halfExtents.y, r };
}

// Snaps a ground entity's Y to the floor height of its current grid cell.
void snapEntityToFloor(Entity& e, const LevelGrid& grid);

// Returns true if entity AABB overlaps the player AABB in the XZ plane.
bool entityOverlapsPlayer(const Vec3& entPos, const Vec3& halfExt,
                          const Vec3& playerPos, f32 playerHW);

// Axis-separated slide movement for entities (flying aware).
void entityMoveAndSlide(Entity& e, const LevelGrid& grid, f32 dt,
                        const Vec3& playerPos, f32 playerHW);

// ---------------------------------------------------------------------------
// LOS helpers (defined in enemy_ai.cpp)
// ---------------------------------------------------------------------------

// True if entity has line-of-sight to the player.
bool hasLOS(const Entity& e, const Player& player, const LevelGrid& grid);

// True if there is clear LOS between two world points.
bool hasLOSToPoint(Vec3 from, Vec3 to, const LevelGrid& grid);

// True if a body of the given radius can walk in a STRAIGHT line from->to without
// any part of its width clipping a wall. Casts the centre ray plus two rays
// offset perpendicular by `radius`, so an enemy only commits to a direct charge
// when its whole AABB actually fits — otherwise the caller falls back to A*.
// This is what stops wide enemies from steering into corners they can "see"
// the player through with a thin ray.
bool hasWidthLOS(Vec3 from, Vec3 to, f32 radius, const LevelGrid& grid);

// ---------------------------------------------------------------------------
// Extracted per-entity blocks (each declared here, defined in its own .cpp)
// ---------------------------------------------------------------------------

// updateLegacyBossAbilities: handles boss-type entities — delegates to BossAI
// personality system, tracks LOS-duration aggro, and fires the per-floor boss
// ability on cooldown. No loop-level early exits — always returns Continue.
// (enemy_ai_boss.cpp)
void updateLegacyBossAbilities(Entity& e, u32 i,
                                EntityPool& pool, ProjectilePool& projectiles,
                                Player& player, Player* targetPlayer,
                                const LevelGrid& grid, f32 dt,
                                f32 dist, Vec3 playerEye);

// applyRoleModifiers: handles archetype special abilities (SUMMONER/HEALER/
// CHARGER/RANGED_CASTER/BOMBER/SHIELD_BEARER) and the far-enemy stagger early-
// exit. Returns NextEntity if the stagger `continue` fires, BreakLoop if the
// suicide-bomber `break` fires, Continue otherwise.
// (enemy_ai_roles.cpp)
AIStep applyRoleModifiers(Entity& e, u32 i,
                           EntityPool& pool,
                           Player& player, Player* targetPlayer,
                           const LevelGrid& grid, f32 dt,
                           f32 dist, Vec3 playerEye);

// updateFriendlyNPC: full AI for friendly NPC party members (Paladin/Cleric/
// Archer/Mage/Rogue) and drones. Finds enemy targets, moves, attacks, handles
// group cohesion, separation, and speech. Always returns NextEntity because the
// original block ended with `continue;` (friendlies skip the hostile AI path).
//
// Anchored on the owning player by POSITION (anchorPos / anchorEye), not a Player&,
// so remote-cast minions (a co-op peer's drones, whose Player struct doesn't exist
// on this host) still get full AI. anchorPlayer is the real Player* when the owner
// is local (host / split-screen lane) and nullptr for a remote owner; only the
// Cleric heal-the-player path needs it (writes player.health). Passing nullptr there
// just skips healing the (non-local) owner — drones/other classes are unaffected.
// (enemy_ai_friendly.cpp)
AIStep updateFriendlyNPC(Entity& e, u32 i,
                          EntityPool& pool, ProjectilePool& projectiles,
                          Vec3 anchorPos, Player* anchorPlayer,
                          const LevelGrid& grid, f32 dt,
                          Vec3 playerEye);

// updateHostileStates: the switch(e.aiState) FSM for hostile entities.
// Handles IDLE/CHASE/FLYBY/ATTACK/DORMANT/FLANK/RETREAT/AMBUSH/STRAFE/
// SURROUND/DEAD. All `break` statements inside are switch-internal (exit the
// switch, not the entity loop), so this function returns void.
// (enemy_ai_states.cpp)
void updateHostileStates(Entity& e, u32 i,
                          EntityPool& pool, ProjectilePool& projectiles,
                          Player& player, Player* targetPlayer,
                          const LevelGrid& grid, f32 dt,
                          Vec3 targetPos, f32 targetDist,
                          Vec3 targetVel, bool targetIsNPC,
                          Vec3 dirToTarget, bool isBat,
                          f32 effectiveSpeed, bool shouldCheckLOS,
                          f32 dist,
                          SquadPool* squads, const DungeonResult* dungeon,
                          bool spawnCalm);  // true = suppress IDLE->CHASE auto-detection
