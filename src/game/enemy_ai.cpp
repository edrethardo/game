// Enemy AI: per-tick FSM (IDLE -> CHASE -> ATTACK; FLYBY for flying enemies)
// driven from EnemyAI::update. Handles LOS checks (staggered by aiCheckIdx for
// budget), grid-axis-separated movement (flying ignores floor/ceiling unless
// blocked), and attack execution against the player. Death is routed through
// Combat::applyDamage; this file does not free entities directly. See
// CLAUDE.md "Data Lifecycles" for the entity handle/death flow.

#include "game/enemy_ai.h"
#include "game/enemy_ai_internal.h"
#include "game/player.h"
#include "net/net_player.h"  // for NetPlayer in N4 friendly-tether resolution
#include "world/level_gen.h"
#include "game/combat.h"
#include "game/projectile.h"
#include "game/game_constants.h"
#include "game/squad.h"
#include "game/boss_ai.h"
#include "game/boss_def.h"
#include "world/raycast.h"
#include "world/combat_query.h"
#include "world/level_grid.h"
#include "world/pathfinder.h"
#include "world/collision.h"
#include <cmath>
#include <cstdlib>

// Queen drone auto-spawn callback — set by Engine so queen can spawn mini drones
void(*s_droneSpawnCb)(Vec3 pos, u8 type) = nullptr;
void EnemyAI::setDroneSpawnCallback(void(*cb)(Vec3, u8)) { s_droneSpawnCb = cb; }

// Boss personality table — set by Engine during init so boss AI can look up BossDefs
const BossDefTable* s_bossDefTable = nullptr;
void EnemyAI::setBossDefTable(const BossDefTable* table) { s_bossDefTable = table; }

// Skeleton minion visuals — resolved once at init (AI code can't resolve asset
// name strings) so boss summon abilities can spawn proper-looking skeletons.
u8 s_skeletonMeshId = 0;
u8 s_skeletonMatId  = 0;
void EnemyAI::setSkeletonVisuals(u8 meshId, u8 matId) { s_skeletonMeshId = meshId; s_skeletonMatId = matId; }

// Frame counter for staggering expensive per-entity work (LOS, AI state).
// Defined here at file scope so extracted sub-files can share it via extern.
u32  s_frameTick           = 0;
// Pre-computed friendly group center/count — updated once per frame so archer
// kiting logic doesn't each scan O(N) per entity.
Vec3 s_friendlyGroupCenter = {0, 0, 0};
u32  s_friendlyGroupCount  = 0;

// ---------------------------------------------------------------------------
// Grid collision for entities (simplified axis-separated slide)
// ---------------------------------------------------------------------------
bool entityOverlapsGrid(Vec3 centre, Vec3 halfExtents,
                        const LevelGrid& grid)
{
    f32 minX = centre.x - halfExtents.x;
    f32 maxX = centre.x + halfExtents.x;
    f32 minZ = centre.z - halfExtents.z;
    f32 maxZ = centre.z + halfExtents.z;

    s32 cx0 = static_cast<s32>(std::floor(minX / grid.cellSize));
    s32 cx1 = static_cast<s32>(std::floor((maxX - 0.0001f) / grid.cellSize));
    s32 cz0 = static_cast<s32>(std::floor(minZ / grid.cellSize));
    s32 cz1 = static_cast<s32>(std::floor((maxZ - 0.0001f) / grid.cellSize));

    for (s32 z = cz0; z <= cz1; z++) {
        for (s32 x = cx0; x <= cx1; x++) {
            if (!LevelGridSystem::isInBounds(grid, (u32)x, (u32)z)) return true;
            if (LevelGridSystem::isSolid(grid, (u32)x, (u32)z)) return true;
        }
    }
    return false;
}

// Snap a ground entity's Y to the floor height of its current grid cell.
// Called after XZ movement to keep entities from floating or sinking.
void snapEntityToFloor(Entity& e, const LevelGrid& grid) {
    u32 gx, gz;
    if (LevelGridSystem::worldToGrid(grid, e.position, gx, gz) &&
        !LevelGridSystem::isSolid(grid, gx, gz)) {
        f32 floorH = LevelGridSystem::getFloorHeight(grid, gx, gz);
        e.position.y = floorH + e.halfExtents.y;
    }
}

// Checks whether an entity AABB overlaps the player AABB in the XZ plane.
bool entityOverlapsPlayer(const Vec3& entPos, const Vec3& halfExt,
                          const Vec3& playerPos, f32 playerHW) {
    f32 eMinX = entPos.x - halfExt.x;
    f32 eMaxX = entPos.x + halfExt.x;
    f32 eMinZ = entPos.z - halfExt.z;
    f32 eMaxZ = entPos.z + halfExt.z;
    f32 pMinX = playerPos.x - playerHW;
    f32 pMaxX = playerPos.x + playerHW;
    f32 pMinZ = playerPos.z - playerHW;
    f32 pMaxZ = playerPos.z + playerHW;
    return eMaxX > pMinX && eMinX < pMaxX &&
           eMaxZ > pMinZ && eMinZ < pMaxZ;
}

void entityMoveAndSlide(Entity& e, const LevelGrid& grid, f32 dt,
                        const Vec3& /*playerPos*/, f32 /*playerHW*/) {
    Vec3 delta = e.velocity * dt;
    // Enemies walk freely toward the player — only walls block them.
    // The player is blocked from walking through enemies via moveAndSlide
    // obstacles, and pushPlayerFromEntities handles any residual overlap.

    // Axis-separated slide: a blocked axis stops while the other keeps moving, so
    // a diagonal into a flat wall slides along it. Track which axes blocked so we
    // can detect the one case this can't escape on its own — a concave corner
    // that blocks BOTH — and nudge out of it below.
    bool blockedX = false, blockedZ = false;

    // X axis — zero velocity on collision to prevent wall penetration over time
    Vec3 tryPos = e.position + Vec3{delta.x, 0, 0};
    if (!entityOverlapsGrid(tryPos, e.halfExtents, grid)) {
        e.position.x = tryPos.x;
    } else {
        e.velocity.x = 0.0f;
        blockedX = true;
    }

    // Z axis
    tryPos = e.position + Vec3{0, 0, delta.z};
    if (!entityOverlapsGrid(tryPos, e.halfExtents, grid)) {
        e.position.z = tryPos.z;
    } else {
        e.velocity.z = 0.0f;
        blockedZ = true;
    }

    // Corner-slip for oversized bodies: a boss whose full AABB jams on BOTH axes
    // is wedged in a corner. Shrink it to its (smaller) nav footprint and retry
    // the same move so it slips out/through instead of grinding to a halt — the
    // "make large enemies slippery at corners" behaviour. Gated on `large` so the
    // footprint only shrinks for bodies bigger than the cell grid can hold, and on
    // both-axes-blocked so big enemies keep full collision against ordinary walls.
    bool large = e.halfExtents.x > ENTITY_NAV_RADIUS_CAP;
    if (large && blockedX && blockedZ && (delta.x * delta.x + delta.z * delta.z) > 1e-6f &&
        !(e.flags & ENT_FLYING)) {
        Vec3 slim = navExtents(e);
        Vec3 sx = e.position + Vec3{delta.x, 0, 0};
        if (!entityOverlapsGrid(sx, slim, grid)) { e.position.x = sx.x; blockedX = false; }
        Vec3 sz = e.position + Vec3{0, 0, delta.z};
        if (!entityOverlapsGrid(sz, slim, grid)) { e.position.z = sz.z; blockedZ = false; }
    }

    // Anti-wedge: both axes blocked while trying to move means we drove into a
    // concave corner and would dead-stop. Slide toward the most open neighbouring
    // cell (clearance-field gradient, tie-broken toward the intended heading) so
    // the entity walks itself out instead of relying on the teleport safety net.
    if (blockedX && blockedZ && (delta.x * delta.x + delta.z * delta.z) > 1e-6f &&
        !(e.flags & ENT_FLYING)) {
        u32 gx, gz;
        if (LevelGridSystem::worldToGrid(grid, e.position, gx, gz)) {
            u8  curClr   = LevelGridSystem::clearanceAt(grid, gx, gz);
            f32 wantLen  = sqrtf(delta.x * delta.x + delta.z * delta.z);
            f32 wantX = delta.x / wantLen, wantZ = delta.z / wantLen;
            const s32 ndx[8] = { 1, -1, 0,  0, 1,  1, -1, -1 };
            const s32 ndz[8] = { 0,  0, 1, -1, 1, -1,  1, -1 };
            f32 bestScore = -1.0f, bestX = 0.0f, bestZ = 0.0f;
            for (u8 d = 0; d < 8; d++) {
                s32 nx = static_cast<s32>(gx) + ndx[d];
                s32 nz = static_cast<s32>(gz) + ndz[d];
                if (nx < 0 || nz < 0 ||
                    !LevelGridSystem::isInBounds(grid, (u32)nx, (u32)nz)) continue;
                if (LevelGridSystem::isSolid(grid, (u32)nx, (u32)nz)) continue;
                u8 clr = LevelGridSystem::clearanceAt(grid, (u32)nx, (u32)nz);
                if (clr < curClr) continue;  // never nudge into a tighter spot
                f32 inv = 1.0f / sqrtf((f32)(ndx[d]*ndx[d] + ndz[d]*ndz[d]));
                f32 dirX = ndx[d] * inv, dirZ = ndz[d] * inv;
                // Reward openness first, then agreement with the intended heading.
                f32 score = clr * 2.0f + (dirX * wantX + dirZ * wantZ);
                if (score > bestScore) { bestScore = score; bestX = dirX; bestZ = dirZ; }
            }
            if (bestScore >= 0.0f) {
                f32 nudge = 2.0f * dt;  // gentle slide-out, ~2 m/s
                Vec3 cand = e.position + Vec3{bestX * nudge, 0, bestZ * nudge};
                // Use the capped nav footprint so an oversized boss can still
                // find a free escape cell (its full AABB never fits a 1-cell gap).
                if (!entityOverlapsGrid(cand, navExtents(e), grid)) {
                    e.position.x = cand.x;
                    e.position.z = cand.z;
                }
            }
        }
    }

    // Y axis (flying only — ground enemies snap to floor)
    if (e.flags & ENT_FLYING) {
        tryPos = e.position + Vec3{0, delta.y, 0};
        if (entityOverlapsGrid(tryPos, e.halfExtents, grid)) {
            e.velocity.y = 0.0f;
        } else {
            e.position.y = tryPos.y;
        }

        // Clamp Y to floor/ceiling of current cell
        u32 gx, gz;
        if (LevelGridSystem::worldToGrid(grid, e.position, gx, gz) &&
            !LevelGridSystem::isSolid(grid, gx, gz)) {
            f32 floorH = LevelGridSystem::getFloorHeight(grid, gx, gz);
            f32 ceilH  = LevelGridSystem::getCeilingHeight(grid, gx, gz);
            f32 minY = floorH + e.halfExtents.y;
            f32 maxY = ceilH  - e.halfExtents.y;
            if (e.position.y < minY) e.position.y = minY;
            if (e.position.y > maxY) e.position.y = maxY;
        }
    }
}

// ---------------------------------------------------------------------------
// Line-of-sight checks (world only — ignores other entities)
// ---------------------------------------------------------------------------
bool hasLOS(const Entity& e, const Player& player,
            const LevelGrid& grid)
{
    Vec3 eyePos = player.position + Vec3{0, player.eyeHeight, 0};
    Vec3 toPlayer = eyePos - e.position;
    f32 dist = length(toPlayer);
    if (dist < 0.001f) return true;

    Vec3 dir = toPlayer * (1.0f / dist);
    RayHit hit = Raycast::cast(grid, e.position, dir, dist);
    return !hit.hit || hit.distance >= dist - 0.1f;
}

// General point-to-point LOS check (used for NPC→enemy and enemy→NPC)
bool hasLOSToPoint(Vec3 from, Vec3 to, const LevelGrid& grid)
{
    Vec3 delta = to - from;
    f32 dist = length(delta);
    if (dist < 0.001f) return true;
    Vec3 dir = delta * (1.0f / dist);
    RayHit hit = Raycast::cast(grid, from, dir, dist);
    return !hit.hit || hit.distance >= dist - 0.1f;
}

// ---------------------------------------------------------------------------
// Dormant-disguise watch set (weeping-angel wake rule) — see enemy_ai_internal.h
// ---------------------------------------------------------------------------
const Player* s_watchPlayers[MAX_WATCH_PLAYERS] = {};
u32 s_watchPlayerCount = 0;

bool anyPlayerWatching(Vec3 point, const LevelGrid& grid)
{
    for (u32 i = 0; i < s_watchPlayerCount; i++) {
        const Player* p = s_watchPlayers[i];
        if (!p || p->health <= 0.0f) continue;          // the dead watch nothing
        Vec3 eye = p->position + Vec3{0, p->eyeHeight, 0};
        if (!EnemyAI::inViewCone(eye, p->yaw, p->pitch, point, WATCH_CONE_COS)) continue;
        // Cone hit — but only real sight pins the statue: a wall between them means the
        // player merely faces its direction, and it is free to stir.
        if (hasLOSToPoint(eye, point, grid)) return true;
    }
    return false;
}

bool anyPlayerWithin(Vec3 pos, f32 range)
{
    for (u32 i = 0; i < s_watchPlayerCount; i++) {
        const Player* p = s_watchPlayers[i];
        if (!p || p->health <= 0.0f) continue;
        if (p->smokeTimer > 0.0f) continue;             // stealthed players don't provoke
        Vec3 d = p->position - pos;
        if (d.x * d.x + d.z * d.z <= range * range) return true;
    }
    return false;
}

// The ONE wake routine for disguised ambushers — called by the DORMANT state's
// weeping-angel trigger, the mimic damage-spring, and the Engine's E-interact path
// (Engine::onEntityInteract / SP-host chest "open"), so the reveal always looks the same.
void EnemyAI::wakeAmbusher(Entity& e)
{
    if (e.aiState != AIState::DORMANT) return;
    e.aiState     = AIState::CHASE;
    e.attackTimer = 0.0f;                 // attack immediately on wake
    if (e.enemyRole & EnemyRole::AMBUSH) {
        // Gargoyle: silent stone-shedding wake, no chomp animation
        e.speechText  = "...";
        e.speechTimer = 1.5f;
    } else {
        // Mimic: surprise chomp
        e.attackAnimT = 0.4f;
        e.speechText  = "*CHOMP*";
        e.speechTimer = 2.0f;
    }
}

bool hasWidthLOS(Vec3 from, Vec3 to, f32 radius, const LevelGrid& grid)
{
    // Degenerate radius — fall back to the cheap single ray.
    if (radius <= 0.01f) return hasLOSToPoint(from, to, grid);

    Vec3 delta = to - from;
    f32 dist = length(delta);
    if (dist < 0.001f) return true;
    Vec3 dir = delta * (1.0f / dist);

    // Perpendicular in the XZ plane, scaled to the body radius. Offsetting both
    // endpoints by ±perp gives the two "shoulder" rays alongside the centre ray;
    // the body only fits straight through if all three are clear.
    Vec3 perp = {-dir.z, 0.0f, dir.x};
    f32 plen = sqrtf(perp.x * perp.x + perp.z * perp.z);
    if (plen > 0.001f) perp = perp * (radius / plen);

    if (!hasLOSToPoint(from, to, grid)) return false;
    if (!hasLOSToPoint(from + perp, to + perp, grid)) return false;
    if (!hasLOSToPoint(from - perp, to - perp, grid)) return false;
    return true;
}

// ---------------------------------------------------------------------------
// Main AI update
// ---------------------------------------------------------------------------
void EnemyAI::update(EntityPool& pool, const LevelGrid& grid,
                      Player& player, ProjectilePool& projectiles, f32 dt,
                      SquadPool* squads,
                      Player** extraPlayers, u32 extraPlayerCount,
                      const DungeonResult* dungeon,
                      bool spawnCalm,
                      const NetPlayer* netPlayers, u32 netPlayerCount)
{
    // Rebuild the dormant-disguise watch set: primary + every extra view. Dead players are
    // kept and filtered inside the helpers (health can change mid-tick via retaliation).
    s_watchPlayers[0]  = &player;
    s_watchPlayerCount = 1;
    for (u32 ep = 0; extraPlayers && ep < extraPlayerCount && s_watchPlayerCount < MAX_WATCH_PLAYERS; ep++) {
        if (extraPlayers[ep]) s_watchPlayers[s_watchPlayerCount++] = extraPlayers[ep];
    }

    // ---------------------------------------------------------------------------
    // Herald aura pass — runs before per-entity AI so movement/cooldown mods
    // are in effect for the entire tick. We reset all buffs first, then reapply
    // from any active AURA heralds. This handles heralds dying mid-floor cleanly.
    // ---------------------------------------------------------------------------
    for (u32 a = 0; a < pool.activeCount; a++) {
        Entity& e = pool.entities[pool.activeList[a]];
        if (e.hasAuraBuff) {
            e.moveSpeed      = e.baseMoveSpeed;
            e.attackCooldown = e.baseAttackCooldown;
            e.hasAuraBuff    = false;
        }
    }

    // Frame counter for staggering expensive per-entity work (LOS, AI state).
    // s_frameTick is now file-scope (defined above) so extracted sub-files can use it.
    s_frameTick++;

    // Pre-compute friendly group center once per frame so archers don't each scan O(N).
    // s_friendlyGroupCenter / s_friendlyGroupCount are file-scope for the same reason.
    {
        Vec3 grpC = {0,0,0}; u32 grpN = 0;
        for (u32 a = 0; a < pool.activeCount; a++) {
            const Entity& ent = pool.entities[pool.activeList[a]];
            if ((ent.flags & ENT_FRIENDLY) && !(ent.flags & ENT_DEAD)) {
                grpC = grpC + ent.position; grpN++;
            }
        }
        if (grpN > 1) grpC = grpC * (1.0f / static_cast<f32>(grpN));
        s_friendlyGroupCenter = grpC;
        s_friendlyGroupCount  = grpN;
    }

    // Herald aura — staggered over 15 frames so the O(N²) check runs ~4×/sec
    constexpr f32 AURA_RADIUS_SQ = 18.0f * 18.0f;
    constexpr u32 AURA_STAGGER = 15;
    for (u32 a = 0; a < pool.activeCount; a++) {
        if ((a % AURA_STAGGER) != (s_frameTick % AURA_STAGGER)) continue;
        Entity& herald = pool.entities[pool.activeList[a]];
        if (!(herald.enemyRole & EnemyRole::AURA)) continue;
        if (herald.flags & ENT_DEAD) continue;

        for (u32 b = 0; b < pool.activeCount; b++) {
            if (b == a) continue;
            Entity& ally = pool.entities[pool.activeList[b]];
            if (ally.flags & ENT_DEAD) continue;
            if (ally.flags & ENT_FRIENDLY) continue;

            Vec3 delta = ally.position - herald.position;
            f32 distSq = delta.x*delta.x + delta.y*delta.y + delta.z*delta.z;
            if (distSq < AURA_RADIUS_SQ) {
                ally.moveSpeed      = ally.baseMoveSpeed * 1.1f;
                ally.attackCooldown = ally.baseAttackCooldown * 0.9f;
                ally.hasAuraBuff    = true;
            }
        }
    }

    for (u32 a = 0; a < pool.activeCount; a++) {
        u32 i = pool.activeList[a];
        Entity& e = pool.entities[i];
        if (e.flags & ENT_DEAD) continue;

        // Skip static props — they have no AI, no movement, no combat
        if (e.enemyType == EnemyType::PROP) continue;

        // Select nearest LIVING player as target (for co-op/multiplayer). Health-check BOTH the
        // primary and the extras: a dead player's corpse stays where it fell, so without this an
        // enemy keeps attacking the corpse instead of retargeting to a living player. On a SERVER the
        // primary can itself be the dead host (tickSharedSystems falls back to P0 when the host dies),
        // and a just-killed remote can linger one frame in extras (np.isDead is set in serverNetPost,
        // AFTER this AI pass). Fall back to &player only if EVERYONE is dead (all-dead / game-over
        // edge) so downstream code keeps a non-null targetPlayer. Pointer so damage hits the right player.
        Player* targetPlayer = &player;
        f32  bestDist    = 3.4e38f;
        bool foundLiving = false;
        if (player.health > 0.0f) {
            targetPlayer = &player;
            bestDist     = lengthSq(e.position - player.position);
            foundLiving  = true;
        }
        for (u32 ep = 0; extraPlayers && ep < extraPlayerCount; ep++) {
            if (!extraPlayers[ep] || extraPlayers[ep]->health <= 0.0f) continue; // skip null AND dead
            f32 d = lengthSq(e.position - extraPlayers[ep]->position);
            if (!foundLiving || d < bestDist) {
                bestDist     = d;
                targetPlayer = extraPlayers[ep];
                foundLiving  = true;
            }
        }
        Vec3 playerEye = targetPlayer->position + Vec3{0, targetPlayer->eyeHeight, 0};

        // Tick animation timer
        e.animTimer += dt;
        if (e.attackAnimT > 0.0f) e.attackAnimT -= dt;

        // Determine if this entity is a friendly NPC ally
        bool isFriendly = (e.flags & ENT_FRIENDLY) != 0;

        // Resolve the owner this friendly serves. Priorities:
        //   1) Net co-op (N4): if `ownerNetSlot` is set and that NetPlayer is active, use it
        //      so a remote-cast Tinkerer drone/Necromancer skeleton tethers to its caster.
        //   2) Split-screen: `ownerLocalPlayer > 0` resolves to an `extraPlayers[]` entry.
        //   3) Fallback: the primary local player.
        // `anchorP` stays a Player* only when we still have one (cases 2/3). For net case 1
        // we lift the parts the tether actually needs (position + eyeHeight) into local
        // vars so the rest of the function doesn't need a synthetic Player view.
        Player* anchorP = &player;
        Vec3 anchorPos  = player.position;
        f32  anchorEyeH = player.eyeHeight;
        if (isFriendly) {
            if (e.ownerNetSlot != 0xFF && netPlayers && e.ownerNetSlot < netPlayerCount &&
                netPlayers[e.ownerNetSlot].active) {
                anchorP    = nullptr;
                anchorPos  = netPlayers[e.ownerNetSlot].position;
                anchorEyeH = netPlayers[e.ownerNetSlot].eyeHeight;
            } else if (e.ownerLocalPlayer > 0 && extraPlayers &&
                       (e.ownerLocalPlayer - 1u) < extraPlayerCount &&
                       extraPlayers[e.ownerLocalPlayer - 1]) {
                anchorP    = extraPlayers[e.ownerLocalPlayer - 1];
                anchorPos  = anchorP->position;
                anchorEyeH = anchorP->eyeHeight;
            }
        }
        Vec3 anchorEye = anchorPos + Vec3{0, anchorEyeH, 0};

        // Tinkerer drones (friendly, npcClass NONE): teleport to owner if too far
        // Skip teleport while Overclock is active — let drones roam freely
        if (isFriendly && e.npcClass == NpcClass::NONE) {
            f32 distToPlayer = length(e.position - anchorEye);
            if (distToPlayer > 30.0f && e.overclockTimer <= 0.0f) {
                e.position = anchorEye + Vec3{1.0f, 0, 1.0f};
                Collision::ensureNotInWall(e.position, e.halfExtents, grid);
                snapEntityToFloor(e, grid);
            }

            // Tick overclock buff timer
            if (e.overclockTimer > 0.0f) e.overclockTimer -= dt;

            // Swarm Queen: auto-spawn mini drone every 2s, despawn after lifetime
            if (e.queenLifeTimer > 0.0f) {
                e.queenLifeTimer -= dt;
                e.queenSpawnTimer -= dt;
                if (e.queenSpawnTimer <= 0.0f) {
                    e.queenSpawnTimer = 2.0f;
                    // Spawn a mini spider near the queen
                    Vec3 spawnPos = e.position + Vec3{
                        (std::rand() % 200 - 100) * 0.01f, 0.0f,
                        (std::rand() % 200 - 100) * 0.01f};
                    // Use projectile pool callback would be cleanest but drone callback
                    // is available via static — spawn type 0 (spider) near queen
                    if (s_droneSpawnCb) s_droneSpawnCb(spawnPos, 0);
                }
                if (e.queenLifeTimer <= 0.0f) {
                    // Queen expires — kill without loot
                    e.flags |= ENT_DEAD;
                    e.aiState = AIState::DEAD;
                    e.deathTimer = 0.01f;
                }
            }
        }

        // ---------------------------------------------------------------------------
        // Friendly NPC AI — follows player, attacks nearest hostile enemy.
        // Extracted to enemy_ai_friendly.cpp. Always returns NextEntity because
        // friendly entities skip the hostile AI path (original `continue`).
        // ---------------------------------------------------------------------------
        if (isFriendly) {
            if (spawnCalm) {
                // Spawn-calm window: companions wait with the player instead of
                // marching toward the exit and starting fights before the player moves.
                e.velocity = {0, 0, 0};
            } else {
                // Always tick: updateFriendlyNPC is anchored on a POSITION (anchorPos/
                // anchorEye), so it works whether the owner is local (anchorP = &player,
                // host/split-screen lane) or remote (anchorP == nullptr, a co-op peer's
                // drone). Previously this was gated on `anchorP != nullptr`, which silently
                // froze every minion whose `ownerNetSlot` mapped to an active NetPlayer —
                // including the LOCAL player's own drones in singleplayer (m_players[0] is
                // active after startGame), leaving them floating at spawn height. anchorP is
                // forwarded only for the Cleric heal-the-owner path; nullptr just skips it.
                updateFriendlyNPC(e, i, pool, projectiles, anchorPos, anchorP, grid, dt, anchorEye);
            }
            continue; // friendly NPC path ends here; hostile AI below is skipped
        }

        // ---------------------------------------------------------------------------
        // Push apart from squad members only (same room) to prevent blobbing.
        // Entities in different rooms can't overlap anyway (walls separate them).
        // ---------------------------------------------------------------------------
        if (e.squadId != 0xFFFF && squads && e.squadId < squads->squadCount) {
            const Squad& sq = squads->squads[e.squadId];
            for (u8 mi = 0; mi < sq.memberCount; mi++) {
                u32 oIdx = sq.memberIndices[mi];
                if (oIdx == i) continue;
                Entity& other = pool.entities[oIdx];
                if (!(other.flags & ENT_ACTIVE)) continue;
                if (other.flags & ENT_DEAD) continue;
                Vec3 diff = e.position - other.position;
                f32 dist2 = diff.x * diff.x + diff.z * diff.z;
                f32 minDist = e.halfExtents.x + other.halfExtents.x + 0.15f;
                if (dist2 < minDist * minDist && dist2 > 0.001f) {
                    f32 dist = sqrtf(dist2);
                    Vec3 push = {diff.x / dist, 0, diff.z / dist};
                    f32 overlap = minDist - dist;
                    Vec3 newPos = e.position + push * (overlap * 0.3f);
                    if (!entityOverlapsGrid(newPos, e.halfExtents, grid)) {
                        e.position = newPos;
                    }
                }
            }
        }

        // ---------------------------------------------------------------------------
        // Hostile enemy AI — targets friendly NPCs first, then player
        // ---------------------------------------------------------------------------

        // Stunned enemies are completely immobilized — skip all AI
        if (e.stunTimer > 0.0f) {
            e.velocity = {0, 0, 0};
            continue;
        }

        // Knockback stagger: while a knockback impulse is decaying, let it carry the
        // entity (skip movement/AI this tick) instead of the AI overwriting velocity.
        if (e.knockbackTimer > 0.0f) continue;

        Vec3 toPlayer = playerEye - e.position;
        f32  dist     = length(toPlayer);

        // Default target is the player
        Vec3 targetPos = playerEye;
        f32  targetDist = dist;
        bool targetIsNPC = false;

        // Search for closer friendly NPCs to target instead of the player.
        // Staggered: only rescan every 8 frames. If cached target died, force rescan.
        {
            bool needNpcRescan = ((i + s_frameTick) % 8 == 0);
            if (e.targetEntityIdx < MAX_ENTITIES) {
                const Entity& cached = pool.entities[e.targetEntityIdx];
                if ((cached.flags & ENT_DEAD) || !(cached.flags & ENT_ACTIVE) ||
                    !(cached.flags & ENT_FRIENDLY))
                    needNpcRescan = true;
            }

            if (needNpcRescan) {
                f32 bestNpcDist = dist;
                for (u32 ni = 0; ni < pool.activeCount; ni++) {
                    u32 nIdx = pool.activeList[ni];
                    const Entity& npc = pool.entities[nIdx];
                    if (!(npc.flags & ENT_FRIENDLY)) continue;
                    if (npc.flags & ENT_DEAD) continue;
                    if (npc.flags & ENT_UNTARGETABLE) continue;

                    Vec3 toNpc = npc.position - e.position;
                    f32 npcDist = length(toNpc);
                    f32 effectiveDist = (npc.npcClass == NpcClass::PALADIN) ? npcDist * 0.5f : npcDist;
                    if (effectiveDist < bestNpcDist && npcDist <= e.detectionRange) {
                        bestNpcDist = npcDist;
                        // Aim at the entity's vertical CENTER (position), not its top
                        // (position + halfExtents.y). The top is a marginal edge hit for
                        // short targets like the Engineer turret (0.6 m tall) — a projectile
                        // descending from the enemy's higher eye clips just over it. Center
                        // gives a full half-height of vertical margin.
                        targetPos = npc.position;
                        targetDist = npcDist;
                        targetIsNPC = true;
                        e.targetEntityIdx = static_cast<u16>(nIdx);
                    }
                }
                if (!targetIsNPC) {
                    e.targetEntityIdx = 0xFFFF;
                }
            } else if (e.targetEntityIdx < MAX_ENTITIES) {
                // Use cached NPC target
                const Entity& npc = pool.entities[e.targetEntityIdx];
                if ((npc.flags & ENT_FRIENDLY) && !(npc.flags & ENT_DEAD)) {
                    targetPos = npc.position; // vertical center — see rescan branch above
                    targetDist = length(npc.position - e.position);
                    targetIsNPC = true;
                }
            }
        }

        // Velocity of the current target — used for projectile leading in ranged attacks.
        // NPC targets use entity velocity; otherwise fall back to player velocity.
        Vec3 targetVel = {0, 0, 0};
        if (targetIsNPC && e.targetEntityIdx < MAX_ENTITIES) {
            targetVel = pool.entities[e.targetEntityIdx].velocity;
        } else {
            targetVel = player.velocity;
        }

        // Direction toward current target (NPC or player)
        Vec3 toTarget = targetPos - e.position;
        f32  tDist = length(toTarget);
        Vec3 dirToTarget = (tDist > 0.001f) ? toTarget * (1.0f / tDist) : Vec3{0,0,0};

        bool isBat = (e.flags & ENT_FLYING) != 0;

        // Effective speed (halved by freeze)
        f32 effectiveSpeed = e.moveSpeed;
        if (e.freezeTimer > 0.0f) effectiveSpeed *= 0.5f;

        // Face toward target (yaw only) — except during flyby
        if (tDist > 0.001f && e.aiState != AIState::FLYBY) {
            e.yaw = atan2f(-dirToTarget.x, -dirToTarget.z);
        }

        // Boss behavior — unique skill per boss + aggressive chase after 1.5s LOS.
        // Each boss is identified by its spawn floor (e.level).
        // Extracted to enemy_ai_boss.cpp; no early loop exits in this block.
        if (e.enemyType == EnemyType::BOSS) {
            // Boss-room leash: only engage while the player is inside the arena;
            // otherwise disengage and walk back to the arena centre (homePosition).
            // Keeps milestone bosses in their room instead of roaming the floor.
            // Skipped during the ENTOMBING channel so a false-death boss finishes
            // channelling even if the player steps out.
            if (e.leashRadius > 0.0f && e.bossPhase != BossPhase::ENTOMBING) {
                f32 pdx = targetPos.x - e.homePosition.x;
                f32 pdz = targetPos.z - e.homePosition.z;
                bool playerInArena = (pdx*pdx + pdz*pdz) <= e.leashRadius * e.leashRadius;
                Vec3 toHome = {e.homePosition.x - e.position.x, 0.0f, e.homePosition.z - e.position.z};
                f32 hd = sqrtf(toHome.x*toHome.x + toHome.z*toHome.z);
                if (!playerInArena && !e.provoked) {
                    // Disengage: return toward the arena centre and idle (skip abilities + FSM).
                    // A provoked boss (one that has been attacked) never disengages — it stays
                    // engaged and rages at the arena edge even if the attacker is outside the room.
                    e.aiState = AIState::IDLE;
                    if (hd > 0.6f) {
                        Vec3 d = toHome * (1.0f / hd);
                        e.position.x += d.x * e.moveSpeed * dt;
                        e.position.z += d.z * e.moveSpeed * dt;
                        e.yaw = atan2f(-d.x, -d.z);
                        e.animTimer += dt; // keep the walk-home animation moving
                    }
                    e.velocity = {0,0,0};
                    e.flybyTarget.x = 0.0f; // reset LOS-aggro timer so he re-arms on re-entry
                    continue;
                }
                // Engaged: clamp inside the arena (catches teleport/knockback overshoot).
                if (hd > e.leashRadius) {
                    Vec3 d = toHome * (1.0f / hd);
                    e.position.x = e.homePosition.x - d.x * e.leashRadius;
                    e.position.z = e.homePosition.z - d.z * e.leashRadius;
                }
            }
            updateLegacyBossAbilities(e, i, pool, projectiles, player, targetPlayer,
                                      grid, dt, dist, playerEye);
            // Entombed boss (Malachar's false-death channel) stays put — skip the
            // role/FSM passes so he doesn't drift while invulnerable and channeling.
            if (e.bossPhase == BossPhase::ENTOMBING) { e.velocity = {0,0,0}; continue; }
            // The Dungeon Engine is an immobile turret: updateEngineBoss already faced the player
            // and ran its wave machine; skip the FSM/movement/attack passes so it never drifts.
            if (e.isEngine) { e.velocity = {0,0,0}; continue; }
        }

        // Archetype role modifiers + far-enemy stagger early-exit.
        // Extracted to enemy_ai_roles.cpp.
        // BreakLoop = suicide-bomber self-destruct (original outer `break`)
        // NextEntity = far-enemy stagger (original `continue`)
        {
            AIStep roleStep = applyRoleModifiers(e, i, pool, player, targetPlayer,
                                                 grid, dt, dist, playerEye);
            if (roleStep == AIStep::BreakLoop)  break;
            if (roleStep == AIStep::NextEntity) continue;
        }

        // LOS stagger: all enemies check LOS every 4th frame, use cached value otherwise
        bool shouldCheckLOS = ((i + s_frameTick) % 4 == 0);

        // Hostile entity FSM — IDLE/CHASE/FLYBY/ATTACK/DORMANT/FLANK/RETREAT/AMBUSH/STRAFE/SURROUND/DEAD
        // All `break`s inside are switch-internal (exit the switch, not the entity loop).
        // Extracted to enemy_ai_states.cpp.
        updateHostileStates(e, i, pool, projectiles, player, targetPlayer, grid, dt,
                            targetPos, targetDist, targetVel, targetIsNPC,
                            dirToTarget, isBat, effectiveSpeed, shouldCheckLOS,
                            dist, squads, dungeon, spawnCalm);
    }

    // --- Entity-entity separation: push overlapping entities apart ---
    // Prevents pileup in doorways and tight corridors.
    for (u32 a = 0; a < pool.activeCount; a++) {
        Entity& ea = pool.entities[pool.activeList[a]];
        if (!(ea.flags & ENT_ACTIVE) || (ea.flags & ENT_DEAD)) continue;
        if (ea.enemyType == EnemyType::PROP) continue;

        for (u32 b = a + 1; b < pool.activeCount; b++) {
            Entity& eb = pool.entities[pool.activeList[b]];
            if (!(eb.flags & ENT_ACTIVE) || (eb.flags & ENT_DEAD)) continue;
            if (eb.enemyType == EnemyType::PROP) continue;

            // Skip drone-drone pairs (let swarms overlap freely)
            bool aDrone = (ea.flags & ENT_FRIENDLY) && ea.npcClass == NpcClass::NONE;
            bool bDrone = (eb.flags & ENT_FRIENDLY) && eb.npcClass == NpcClass::NONE;
            if (aDrone && bDrone) continue;

            Vec3 delta = eb.position - ea.position;
            delta.y = 0.0f; // XZ plane only
            f32 distSq = delta.x * delta.x + delta.z * delta.z;
            constexpr f32 SEP_RADIUS = 1.0f;
            if (distSq > SEP_RADIUS * SEP_RADIUS || distSq < 0.0001f) continue;

            f32 d = sqrtf(distSq);
            f32 overlap = SEP_RADIUS - d;
            Vec3 pushDir = delta * (1.0f / d);
            f32 pushStr = overlap * 2.0f * dt; // gentle push proportional to overlap

            // Only push if the new position doesn't overlap a wall
            Vec3 newA = ea.position; newA.x -= pushDir.x * pushStr; newA.z -= pushDir.z * pushStr;
            Vec3 newB = eb.position; newB.x += pushDir.x * pushStr; newB.z += pushDir.z * pushStr;
            if (!Collision::entityOverlapsGrid(newA, ea.halfExtents, grid)) {
                ea.position.x = newA.x; ea.position.z = newA.z;
            }
            if (!Collision::entityOverlapsGrid(newB, eb.halfExtents, grid)) {
                eb.position.x = newB.x; eb.position.z = newB.z;
            }
        }
    }

    // --- Stuck detection for hostile enemies (last-resort safety net) ---
    // The clearance-gradient nudge in entityMoveAndSlide now walks enemies out of
    // corners on its own, so this is demoted to a rare backstop: only after a full
    // 2s of zero progress do we re-path, and only teleport if even A* fails.
    for (u32 a = 0; a < pool.activeCount; a++) {
        u32 idx = pool.activeList[a];
        Entity& e = pool.entities[idx];
        if (!(e.flags & ENT_ACTIVE) || (e.flags & ENT_DEAD)) continue;
        if (e.flags & ENT_FRIENDLY) continue; // friendlies already have their own stuck detection
        if (e.aiState != AIState::CHASE) continue; // only check during active pursuit

        f32 movedDist = length(e.position - e.lastSeenPos);
        if (movedDist < 0.05f) {
            e.stuckTimer += dt;
            if (e.stuckTimer > 2.0f) {
                // Try A* path to target as recovery
                e.pathLen = Pathfinder::findPath(grid, e.position,
                    player.position, e.pathWaypoints, MAX_PATH_WAYPOINTS, navRadius(e));
                e.pathIdx = 0;

                // If A* also fails, teleport to nearest walkable cell center
                if (e.pathLen == 0) {
                    u32 gx, gz;
                    if (LevelGridSystem::worldToGrid(grid, e.position, gx, gz)) {
                        Vec3 cellCenter = LevelGridSystem::gridToWorld(grid, gx, gz);
                        cellCenter.y = e.position.y;
                        if (!entityOverlapsGrid(cellCenter, navExtents(e), grid)) {
                            e.position = cellCenter;
                        }
                    }
                }
                e.stuckTimer = 0.0f;
            }
        } else {
            e.stuckTimer = 0.0f;
        }
        e.lastSeenPos = e.position;
    }
}
