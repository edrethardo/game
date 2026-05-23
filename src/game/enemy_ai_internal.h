#pragma once
// Internal header for enemy_ai subsystem.
// Shared helpers and types used across the split .cpp files.
// Never include from outside game/enemy_ai*.cpp — public API is enemy_ai.h.

#include "core/types.h"
#include "game/entity.h"
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

// ---------------------------------------------------------------------------
// Grid/movement helpers (defined in enemy_ai.cpp)
// ---------------------------------------------------------------------------

// Returns true if the AABB at `centre`/`halfExtents` overlaps any solid cell.
bool entityOverlapsGrid(Vec3 centre, Vec3 halfExtents, const LevelGrid& grid);

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
