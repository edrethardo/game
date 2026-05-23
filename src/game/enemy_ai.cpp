// Enemy AI: per-tick FSM (IDLE -> CHASE -> ATTACK; FLYBY for flying enemies)
// driven from EnemyAI::update. Handles LOS checks (staggered by aiCheckIdx for
// budget), grid-axis-separated movement (flying ignores floor/ceiling unless
// blocked), and attack execution against the player. Death is routed through
// Combat::applyDamage; this file does not free entities directly. See
// CLAUDE.md "Data Lifecycles" for the entity handle/death flow.

#include "game/enemy_ai.h"
#include "game/enemy_ai_internal.h"
#include "game/player.h"
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

    // X axis — zero velocity on collision to prevent wall penetration over time
    Vec3 tryPos = e.position + Vec3{delta.x, 0, 0};
    if (!entityOverlapsGrid(tryPos, e.halfExtents, grid)) {
        e.position.x = tryPos.x;
    } else {
        e.velocity.x = 0.0f;
    }

    // Z axis
    tryPos = e.position + Vec3{0, 0, delta.z};
    if (!entityOverlapsGrid(tryPos, e.halfExtents, grid)) {
        e.position.z = tryPos.z;
    } else {
        e.velocity.z = 0.0f;
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
// Main AI update
// ---------------------------------------------------------------------------
void EnemyAI::update(EntityPool& pool, const LevelGrid& grid,
                      Player& player, ProjectilePool& projectiles, f32 dt,
                      SquadPool* squads,
                      Player** extraPlayers, u32 extraPlayerCount,
                      const DungeonResult* dungeon)
{
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

        // Select nearest player as target (for co-op/multiplayer)
        // Uses pointer so damage is applied to the correct player
        Player* targetPlayer = &player;
        if (extraPlayers && extraPlayerCount > 0) {
            f32 bestDist = lengthSq(e.position - player.position);
            for (u32 ep = 0; ep < extraPlayerCount; ep++) {
                if (!extraPlayers[ep]) continue;
                f32 d = lengthSq(e.position - extraPlayers[ep]->position);
                if (d < bestDist) {
                    bestDist = d;
                    targetPlayer = extraPlayers[ep];
                }
            }
        }
        Vec3 playerEye = targetPlayer->position + Vec3{0, targetPlayer->eyeHeight, 0};

        // Tick animation timer
        e.animTimer += dt;
        if (e.attackAnimT > 0.0f) e.attackAnimT -= dt;

        // Determine if this entity is a friendly NPC ally
        bool isFriendly = (e.flags & ENT_FRIENDLY) != 0;

        // Tinkerer drones (friendly, npcClass NONE): teleport to player if too far
        // Skip teleport while Overclock is active — let drones roam freely
        if (isFriendly && e.npcClass == NpcClass::NONE) {
            f32 distToPlayer = length(e.position - playerEye);
            if (distToPlayer > 30.0f && e.overclockTimer <= 0.0f) {
                e.position = playerEye + Vec3{1.0f, 0, 1.0f};
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
        // Friendly NPC AI — follows player, attacks nearest hostile enemy
        // ---------------------------------------------------------------------------
        if (isFriendly) {
            // Stuck detection: if NPC barely moved in 0.5s, teleport to cell center.
            // Uses dedicated stuckTimer to avoid conflicting with flybyTimer (used by drones).
            f32 movedDist = length(e.position - e.lastSeenPos);
            if (movedDist < 0.05f) {
                e.stuckTimer += dt;
                if (e.stuckTimer > 0.5f) {
                    // Teleport to center of current cell (safe, validated)
                    u32 gx, gz;
                    if (LevelGridSystem::worldToGrid(grid, e.position, gx, gz)) {
                        Vec3 cellCenter = LevelGridSystem::gridToWorld(grid, gx, gz);
                        cellCenter.y = e.position.y;
                        if (!entityOverlapsGrid(cellCenter, e.halfExtents, grid)) {
                            e.position = cellCenter;
                        } else {
                            // Current cell center overlaps — try next flow cell
                            Vec3 flowDir = LevelGridSystem::flowDirection(grid, e.position);
                            if (lengthSq(flowDir) > 0.001f) {
                                Vec3 nextCenter = cellCenter + flowDir * grid.cellSize;
                                nextCenter.y = e.position.y;
                                if (!entityOverlapsGrid(nextCenter, e.halfExtents, grid)) {
                                    e.position = nextCenter;
                                }
                            }
                        }
                    }
                    e.stuckTimer = 0.0f;
                    e.lastSeenPos = e.position;
                }
            } else {
                e.stuckTimer = 0.0f;
                e.lastSeenPos = e.position;
            }

            // NPC halfExtents are set smaller at spawn time (0.35 instead of 0.4)
            // so they fit through 1-cell corridors naturally — no shrink/restore needed

            // Freeze halves friendly NPC speed
            f32 npcSpeed = e.moveSpeed;
            if (e.freezeTimer > 0.0f) npcSpeed *= 0.5f;
            if (e.overclockTimer > 0.0f) npcSpeed *= 1.5f; // Overclock: +50% speed

            // ---------------------------------------------------------------
            // Friendly NPC AI: pathfind toward the exit AND fight enemies
            // encountered along the way.  Each class has a distinct combat
            // style:
            //   Paladin — charges melee enemies, frontline tank
            //   Cleric  — stays back, heals allies, only melees if cornered
            //   Archer  — shoots from range, kites backward if enemies close
            //   Mage    — stands at range, fires splash projectiles
            //   Rogue   — quick hit-and-run, then resumes pathing
            // ---------------------------------------------------------------

            // --- 1. Find closest hostile enemy within detection range ---
            // Staggered: only rescan every 8 frames. Use cached target otherwise.
            // If cached target died, force an immediate rescan.
            bool needTargetRescan = ((i + s_frameTick) % 8 == 0);
            if (e.targetEntityIdx < MAX_ENTITIES) {
                const Entity& cached = pool.entities[e.targetEntityIdx];
                if ((cached.flags & ENT_DEAD) || !(cached.flags & ENT_ACTIVE) ||
                    (cached.flags & ENT_FRIENDLY))
                    needTargetRescan = true;
            } else {
                needTargetRescan = true; // no cached target
            }

            f32 bestEnemyDist = e.detectionRange;
            u16 bestEnemyIdx = e.targetEntityIdx;
            Vec3 enemyPos = {0,0,0};

            if (needTargetRescan) {
                bestEnemyIdx = 0xFFFF;
                for (u32 ni = 0; ni < pool.activeCount; ni++) {
                    u32 nIdx = pool.activeList[ni];
                    const Entity& enemy = pool.entities[nIdx];
                    if (enemy.flags & ENT_FRIENDLY) continue;
                    if (enemy.flags & ENT_DEAD) continue;
                    if (!(enemy.flags & ENT_ACTIVE)) continue;
                    if (enemy.enemyType == EnemyType::PROP) continue;
                    Vec3 toE = enemy.position - e.position;
                    f32 d2 = length(toE);
                    if (d2 < bestEnemyDist) {
                        bestEnemyDist = d2;
                        bestEnemyIdx = static_cast<u16>(nIdx);
                        enemyPos = enemy.position;
                    }
                }
                e.targetEntityIdx = bestEnemyIdx;
            } else if (bestEnemyIdx < MAX_ENTITIES) {
                // Use cached target position
                enemyPos = pool.entities[bestEnemyIdx].position;
                bestEnemyDist = length(enemyPos - e.position);
            }

            // --- 2. Cleric healing (always runs, even during combat) ---
            if (e.npcClass == NpcClass::CLERIC) {
                e.attackTimer -= dt;
                if (e.attackTimer <= 0.0f) {
                    f32 healRange = 6.0f;
                    bool healed = false;
                    // Heal player first
                    if (player.health < player.maxHealth * 0.5f &&
                        length(playerEye - e.position) < healRange) {
                        f32 amt = 8.0f + e.level * 0.5f;
                        player.health += amt;
                        if (player.health > player.maxHealth) player.health = player.maxHealth;
                        healed = true;
                    }
                    // Then check other NPCs
                    if (!healed) {
                        for (u32 ni = 0; ni < pool.activeCount; ni++) {
                            u32 nIdx = pool.activeList[ni];
                            Entity& npc2 = pool.entities[nIdx];
                            if (nIdx == i) continue;
                            if (!(npc2.flags & ENT_FRIENDLY) || (npc2.flags & ENT_DEAD)) continue;
                            if (npc2.health >= npc2.maxHealth * 0.5f) continue;
                            if (length(npc2.position - e.position) > healRange) continue;
                            f32 amt = 8.0f + e.level * 0.5f;
                            npc2.health += amt;
                            if (npc2.health > npc2.maxHealth) npc2.health = npc2.maxHealth;
                            healed = true;
                            break;
                        }
                    }
                    if (healed) {
                        e.attackTimer = 3.0f;
                        e.attackAnimT = 0.3f;
                        static const char* hl[] = {"Heal!", "Light guide you!", "Hold on!"};
                        e.speechText = hl[std::rand() % 3];
                        e.speechTimer = 2.5f;
                    }
                }
            }

            // --- 3. Determine behavior: combat vs pathfind ---
            // Each class has an engagement range — enemies closer than this
            // trigger combat; otherwise the NPC follows the flow field.
            f32 engageDist = 0.0f;
            switch (e.npcClass) {
                case NpcClass::PALADIN: engageDist = 8.0f;  break;
                case NpcClass::CLERIC:  engageDist = 3.0f;  break;
                case NpcClass::ARCHER:  engageDist = 12.0f; break;
                case NpcClass::MAGE:    engageDist = 14.0f; break;
                case NpcClass::ROGUE:   engageDist = 6.0f;  break;
                case NpcClass::NONE:
                    engageDist = (e.flags & ENT_FLYING) ? e.detectionRange : 6.0f;
                    break;
                default:                engageDist = 6.0f;  break;
            }
            // Overclock: drones hunt enemies up to 30m away
            if (e.overclockTimer > 0.0f && e.npcClass == NpcClass::NONE) {
                engageDist = 30.0f;
            }

            bool inCombat = (bestEnemyIdx != 0xFFFF && bestEnemyDist < engageDist);

            if (inCombat) {
                // --- COMBAT MODE: class-specific behavior ---
                Vec3 toEnemy = enemyPos - e.position;
                f32 eDist = length(toEnemy);
                Vec3 dirToEnemy = (eDist > 0.001f) ? toEnemy * (1.0f / eDist) : Vec3{0,0,0};

                // Face the enemy
                if (eDist > 0.001f) {
                    e.yaw = atan2f(-dirToEnemy.x, -dirToEnemy.z);
                }

                // LOS check — ranged NPCs can only fire if they can see the target
                Vec3 npcEye = e.position + Vec3{0, e.halfExtents.y, 0};
                Vec3 enemyCenter = enemyPos + Vec3{0, 0.5f, 0};
                e.hasTargetLOS = hasLOSToPoint(npcEye, enemyCenter, grid);
                if (e.hasTargetLOS) {
                    e.lastSeenPos = enemyPos;
                }

                // If ranged and no LOS, use flow field to navigate around walls
                // instead of beelining toward lastSeenPos (which causes wall-hugging)
                if (!e.hasTargetLOS &&
                    (e.npcWeaponType == WeaponType::PROJECTILE || e.npcWeaponType == WeaponType::HITSCAN)) {
                    Vec3 flowDir = LevelGridSystem::flowDirection(grid, e.position);
                    if (lengthSq(flowDir) > 0.001f) {
                        e.velocity.x = flowDir.x * npcSpeed;
                        e.velocity.z = flowDir.z * npcSpeed;
                        e.yaw = atan2f(-flowDir.x, -flowDir.z);
                    } else {
                        e.velocity = {0, 0, 0};
                    }
                    entityMoveAndSlide(e, grid, dt, player.position, PLAYER_HALF_WIDTH);
                    if (!(e.flags & ENT_FLYING)) snapEntityToFloor(e, grid);
                    inCombat = false; // suppress combat, fall through to speech
                }

                // -- Movement by class --
                if (e.npcClass == NpcClass::PALADIN) {
                    // Paladin: charge toward enemies, get in melee range
                    if (eDist > e.attackRange) {
                        Vec3 flatDir = normalize(Vec3{dirToEnemy.x, 0, dirToEnemy.z});
                        e.velocity.x = flatDir.x * npcSpeed;
                        e.velocity.z = flatDir.z * npcSpeed;
                    } else {
                        e.velocity = {0, 0, 0};
                    }
                } else if (e.npcClass == NpcClass::ARCHER) {
                    // Archer: only kite if has LOS (can actually shoot).
                    // Without LOS, rejoin the group instead.
                    // Group center is pre-computed once per frame (s_friendlyGroupCenter)
                    Vec3 grpC = s_friendlyGroupCenter;
                    u32 grpN = s_friendlyGroupCount;
                    f32 distToGrp = (grpN > 1) ? length(e.position - grpC) : 0.0f;

                    if (!e.hasTargetLOS) {
                        // No LOS — reposition via flow field to find a firing angle
                        Vec3 flowDir = LevelGridSystem::flowDirection(grid, e.position);
                        if (lengthSq(flowDir) > 0.001f) {
                            e.velocity.x = flowDir.x * npcSpeed * 0.8f;
                            e.velocity.z = flowDir.z * npcSpeed * 0.8f;
                            e.yaw = atan2f(-flowDir.x, -flowDir.z);
                        } else {
                            e.velocity = {0, 0, 0};
                        }
                    } else if (eDist < 5.0f && distToGrp < 6.0f) {
                        // Has LOS and enemy close: kite, but check for wall behind
                        Vec3 awayDir = normalize(Vec3{-dirToEnemy.x, 0, -dirToEnemy.z});
                        Vec3 testPos = e.position + awayDir * 0.5f;
                        if (!entityOverlapsGrid(testPos, e.halfExtents, grid)) {
                            e.velocity.x = awayDir.x * npcSpeed * 0.5f;
                            e.velocity.z = awayDir.z * npcSpeed * 0.5f;
                        } else {
                            // Wall behind — strafe perpendicular instead
                            Vec3 strafeDir = {awayDir.z, 0, -awayDir.x};
                            e.velocity.x = strafeDir.x * npcSpeed * 0.5f;
                            e.velocity.z = strafeDir.z * npcSpeed * 0.5f;
                        }
                    } else if (eDist < 5.0f) {
                        e.velocity = {0, 0, 0};
                    } else {
                        e.velocity = {0, 0, 0};
                    }
                } else if (e.npcClass == NpcClass::MAGE) {
                    // Mage: stand at range, reposition if no LOS
                    if (!e.hasTargetLOS) {
                        // No LOS — reposition via flow field to find a casting angle
                        Vec3 flowDir = LevelGridSystem::flowDirection(grid, e.position);
                        if (lengthSq(flowDir) > 0.001f) {
                            e.velocity.x = flowDir.x * npcSpeed * 0.8f;
                            e.velocity.z = flowDir.z * npcSpeed * 0.8f;
                            e.yaw = atan2f(-flowDir.x, -flowDir.z);
                        } else {
                            e.velocity = {0, 0, 0};
                        }
                    } else if (eDist < 6.0f) {
                        // Back up, but check wall behind first
                        Vec3 awayDir = normalize(Vec3{-dirToEnemy.x, 0, -dirToEnemy.z});
                        Vec3 testPos = e.position + awayDir * 0.5f;
                        if (!entityOverlapsGrid(testPos, e.halfExtents, grid)) {
                            e.velocity.x = awayDir.x * npcSpeed * 0.7f;
                            e.velocity.z = awayDir.z * npcSpeed * 0.7f;
                        } else {
                            // Wall behind — hold position instead of faceplanting
                            e.velocity = {0, 0, 0};
                        }
                    } else if (eDist > e.attackRange) {
                        Vec3 flatDir = normalize(Vec3{dirToEnemy.x, 0, dirToEnemy.z});
                        e.velocity.x = flatDir.x * npcSpeed * 0.5f;
                        e.velocity.z = flatDir.z * npcSpeed * 0.5f;
                    } else {
                        e.velocity = {0, 0, 0};
                    }
                } else if (e.npcClass == NpcClass::CLERIC) {
                    // Cleric: stay back, only engage in melee if cornered
                    if (eDist < 3.0f && eDist > e.attackRange) {
                        // Too close — back away slowly
                        Vec3 awayDir = normalize(Vec3{-dirToEnemy.x, 0, -dirToEnemy.z});
                        e.velocity.x = awayDir.x * npcSpeed * 0.5f;
                        e.velocity.z = awayDir.z * npcSpeed * 0.5f;
                    } else if (eDist <= e.attackRange) {
                        e.velocity = {0, 0, 0}; // cornered — stand and fight
                    } else {
                        // Continue pathing if enemy is far
                        Vec3 flowDir = LevelGridSystem::flowDirection(grid, e.position);
                        if (lengthSq(flowDir) > 0.001f) {
                            e.velocity.x = flowDir.x * npcSpeed;
                            e.velocity.z = flowDir.z * npcSpeed;
                            e.yaw = atan2f(-flowDir.x, -flowDir.z);
                        } else {
                            e.velocity = {0, 0, 0};
                        }
                    }
                } else if (e.npcClass == NpcClass::ROGUE) {
                    // Rogue: flank from the side, quick hit then disengage
                    Vec3 playerToEnemy = normalize(Vec3{enemyPos.x - playerEye.x, 0, enemyPos.z - playerEye.z});
                    Vec3 flankOffset = {-playerToEnemy.z, 0, playerToEnemy.x};
                    Vec3 flankTarget = enemyPos + flankOffset * 2.0f;
                    Vec3 toFlank = flankTarget - e.position;
                    f32 fDist = length(toFlank);
                    if (fDist > e.attackRange) {
                        Vec3 flatDir = normalize(Vec3{toFlank.x, 0, toFlank.z});
                        e.velocity.x = flatDir.x * npcSpeed;
                        e.velocity.z = flatDir.z * npcSpeed;
                    } else {
                        e.velocity = {0, 0, 0};
                    }
                } else {
                    // Default: approach enemy
                    if (eDist > e.attackRange) {
                        Vec3 flatDir = normalize(Vec3{dirToEnemy.x, 0, dirToEnemy.z});
                        e.velocity.x = flatDir.x * npcSpeed;
                        e.velocity.z = flatDir.z * npcSpeed;
                    } else {
                        e.velocity = {0, 0, 0};
                    }
                }

                entityMoveAndSlide(e, grid, dt, player.position, PLAYER_HALF_WIDTH);

                // -- Attack on cooldown --
                // Ranged NPCs ALWAYS require LOS to fire (prevents shooting walls)
                if (e.npcClass != NpcClass::CLERIC || eDist <= e.attackRange) {
                    bool canAttack = false;
                    if (e.npcWeaponType == WeaponType::PROJECTILE) {
                        // Projectile: must have LOS regardless of distance
                        canAttack = (eDist <= engageDist && e.hasTargetLOS);
                    } else {
                        // Melee: just needs to be in range
                        canAttack = (eDist <= e.attackRange);
                    }

                    // If ranged and no LOS, move toward enemy to get a clear shot
                    if (e.npcWeaponType == WeaponType::PROJECTILE && !e.hasTargetLOS && eDist <= engageDist) {
                        Vec3 toTarget = e.lastSeenPos - e.position;
                        f32 tDist = length(toTarget);
                        if (tDist > 1.0f) {
                            Vec3 flatDir = normalize(Vec3{toTarget.x, 0, toTarget.z});
                            e.velocity.x = flatDir.x * npcSpeed;
                            e.velocity.z = flatDir.z * npcSpeed;
                            e.yaw = atan2f(-flatDir.x, -flatDir.z);
                            entityMoveAndSlide(e, grid, dt, player.position, PLAYER_HALF_WIDTH);
                        }
                    }

                    if (canAttack) {
                        e.attackTimer -= dt;
                        if (e.attackTimer <= 0.0f) {
                            e.attackTimer = e.attackCooldown;
                            e.attackAnimT = 0.3f;

                            Entity& target = pool.entities[bestEnemyIdx];
                            if (!(target.flags & ENT_DEAD)) {
                                Vec3 eyePos = e.position + Vec3{0, e.halfExtents.y, 0};
                                // Overclock buff: double damage while active
                                f32 savedDmg = e.damage;
                                if (e.overclockTimer > 0.0f) e.damage *= 2.0f;

                                if (e.npcWeaponType == WeaponType::HITSCAN) {
                                    // Instant raycast attack (swarm drones, turrets)
                                    Vec3 targetCenter = target.position + Vec3{0, target.halfExtents.y, 0};
                                    Vec3 fireDir = normalize(targetCenter - eyePos);
                                    CombatHit hit = CombatQuery::raycast(grid, pool, eyePos, fireDir, e.attackRange);
                                    if (hit.hit && hit.type == CombatHit::ENTITY) {
                                        Combat::applyDamage(pool, hit.entityHandle, e.damage);
                                    }
                                } else if (e.npcWeaponType == WeaponType::PROJECTILE) {
                                    Vec3 targetCenter = target.position + Vec3{0, target.halfExtents.y, 0};
                                    Vec3 fireDir = normalize(targetCenter - eyePos);
                                    f32 speed = e.npcProjectileSpeed > 0.0f ? e.npcProjectileSpeed : 15.0f;
                                    f32 radius = e.npcProjectileRadius > 0.0f ? e.npcProjectileRadius : 0.1f;
                                    u8 extraFlags = (e.npcClass == NpcClass::MAGE) ? PROJ_SPLASH : 0;
                                    // Turret bots fire electric bolts (PROJ_SPARK visual)
                                    if (e.npcClass == NpcClass::NONE && !(e.flags & ENT_FLYING))
                                        extraFlags |= PROJ_SPARK;
                                    u16 projIdx = ProjectileSystem::spawn(projectiles, eyePos,
                                        fireDir, speed, e.damage, radius, 3.0f, true, extraFlags);
                                    if (e.npcClass == NpcClass::MAGE && projIdx != 0xFFFF) {
                                        projectiles.projectiles[projIdx].splashRadius = 1.5f;
                                        projectiles.projectiles[projIdx].splashDamage = e.damage * 0.5f;
                                    }
                                    // Turret electric glow
                                    if (projIdx != 0xFFFF && (extraFlags & PROJ_SPARK)) {
                                        projectiles.projectiles[projIdx].lightColor = {0.4f, 0.7f, 1.0f};
                                    }
                                } else {
                                    EntityHandle th = {bestEnemyIdx, target.generation};
                                    Combat::applyDamage(pool, th, e.damage);
                                }
                                e.damage = savedDmg; // restore base damage after overclock

                                if ((std::rand() % 5) == 0) {
                                    static const char* atk[] = {"Take that!", "Die, beast!", "For glory!"};
                                    e.speechText = atk[std::rand() % 3];
                                    e.speechTimer = 2.5f;
                                }
                            }
                        }
                    }
                }
            } else if (e.npcClass == NpcClass::NONE) {
                // --- DRONE MODE ---
                // Mobile turret: hold position in combat, follow player when idle
                // Combat drone (spider, not flying): run ahead of the player, rush enemies
                // Swarm drones (flying): hover near the player
                bool isTurret = (e.enemyType == EnemyType::GENERIC && !(e.flags & ENT_FLYING)
                                 && !(e.flags & ENT_UNTARGETABLE));
                bool isCombatDrone = (e.enemyType == EnemyType::SPIDER);

                if (isTurret) {
                    // Mobile turret bot: follow player when no enemies in range.
                    // Stays put during combat (handled by the inCombat block above).
                    f32 distToPlayer = length(e.position - player.position);
                    if (distToPlayer > 4.0f) {
                        // Too far from player — walk toward them using flow field
                        Vec3 toPlayer = normalize(Vec3{player.position.x - e.position.x, 0,
                                                       player.position.z - e.position.z});
                        e.velocity.x = toPlayer.x * npcSpeed;
                        e.velocity.z = toPlayer.z * npcSpeed;
                        e.yaw = atan2f(-toPlayer.x, -toPlayer.z);
                        entityMoveAndSlide(e, grid, dt, player.position, PLAYER_HALF_WIDTH);
                        if (!(e.flags & ENT_FLYING)) snapEntityToFloor(e, grid);
                    } else {
                        e.velocity = {0, 0, 0};
                    }
                } else if (isCombatDrone) {
                    // Combat drone (spider): orbit player at unique angle, spread out.
                    // Each drone gets a distinct phase so they fan around the player.
                    f32 phase = static_cast<f32>(i) * 1.618f * 6.2832f; // golden angle spread
                    f32 orbitDist = 3.0f + sinf(e.animTimer * 0.8f + phase) * 1.5f;
                    Vec3 orbitTarget = player.position + Vec3{
                        cosf(e.animTimer * 0.5f + phase) * orbitDist, 0.0f,
                        sinf(e.animTimer * 0.5f + phase) * orbitDist
                    };
                    Vec3 toTarget = orbitTarget - e.position;
                    f32 dist2Tgt = lengthSq(toTarget);
                    if (dist2Tgt > 0.5f) {
                        Vec3 moveDir = normalize(toTarget);
                        e.velocity.x = moveDir.x * npcSpeed * 1.2f;
                        e.velocity.z = moveDir.z * npcSpeed * 1.2f;
                        e.yaw = atan2f(-moveDir.x, -moveDir.z);
                    } else {
                        e.velocity.x = 0; e.velocity.z = 0;
                    }
                    entityMoveAndSlide(e, grid, dt, player.position, PLAYER_HALF_WIDTH);
                } else {
                    // Swarm drones (flying bats): orbit player at different heights and angles.
                    // Wide spread with varying orbit radii for a real swarm look.
                    f32 phase = static_cast<f32>(i) * 2.399f; // golden angle (137.5°)
                    f32 orbitRadius = 2.5f + sinf(phase * 3.0f) * 2.0f; // 0.5–4.5m spread
                    f32 orbitSpeed = 1.2f + sinf(phase * 2.0f) * 0.5f;  // varied orbit speed
                    f32 angle = e.animTimer * orbitSpeed + phase;
                    Vec3 orbitTarget = player.position + Vec3{
                        cosf(angle) * orbitRadius, 1.0f + sinf(angle * 1.7f) * 0.5f,
                        sinf(angle) * orbitRadius
                    };
                    Vec3 toTarget = orbitTarget - e.position;
                    f32 d = length(toTarget);
                    if (d > 0.3f) {
                        Vec3 moveDir = toTarget * (1.0f / d);
                        f32 speed = fminf(npcSpeed, d * 4.0f); // ease in near target
                        e.velocity.x = moveDir.x * speed;
                        e.velocity.y = moveDir.y * speed;
                        e.velocity.z = moveDir.z * speed;
                        e.yaw = atan2f(-moveDir.x, -moveDir.z);
                    } else {
                        e.velocity = {0, 0, 0};
                    }

                    entityMoveAndSlide(e, grid, dt, player.position, PLAYER_HALF_WIDTH);
                }
            } else {
                // --- PATHFIND MODE: follow flow field toward exit ---
                // Party cohesion: always walk toward exit, but frontrunners slow
                // down and stragglers speed up so the group stays together.
                Vec3 groupCenter = {0, 0, 0};
                u32 groupCount = 0;
                for (u32 ni = 0; ni < pool.activeCount; ni++) {
                    u32 nIdx = pool.activeList[ni];
                    const Entity& npc2 = pool.entities[nIdx];
                    if (!(npc2.flags & ENT_FRIENDLY)) continue;
                    if (npc2.flags & ENT_DEAD) continue;
                    groupCenter = groupCenter + npc2.position;
                    groupCount++;
                }
                if (groupCount > 1) groupCenter = groupCenter * (1.0f / static_cast<f32>(groupCount));

                Vec3 flowDir = LevelGridSystem::flowDirection(grid, e.position);
                bool atExit = (lengthSq(flowDir) < 0.001f);

                if (atExit) {
                    e.velocity = {0, 0, 0};
                    if (e.speechTimer <= 0.0f && (std::rand() % 300) == 0) {
                        static const char* wl[] = {"Over here!", "This way!", "Found the exit!"};
                        e.speechText = wl[std::rand() % 3];
                        e.speechTimer = 3.0f;
                    }
                } else {
                    // Speed scales with distance from group center:
                    //   ahead of group → slower (0.4x)
                    //   behind group   → faster (1.3x)
                    //   near center    → normal (0.7x base — always a slow walk)
                    f32 speedMult = 0.7f;
                    if (groupCount > 1) {
                        Vec3 toGroup = groupCenter - e.position;
                        f32 distToGroup = length(toGroup);
                        // Check if this NPC is ahead or behind the group along the flow direction
                        f32 dotAhead = toGroup.x * flowDir.x + toGroup.z * flowDir.z;
                        // dotAhead < 0 means NPC is ahead of group center (flow points away from group)
                        if (dotAhead < -2.0f) {
                            // Ahead: slow down proportionally
                            speedMult = 0.35f;
                        } else if (dotAhead > 2.0f) {
                            // Behind: speed up to catch up
                            speedMult = 1.3f;
                        } else if (distToGroup > 6.0f) {
                            // Far from group in any direction: move toward group
                            speedMult = 0.9f;
                        }
                    }

                    e.velocity.x = flowDir.x * npcSpeed * speedMult;
                    e.velocity.z = flowDir.z * npcSpeed * speedMult;
                    e.yaw = atan2f(-flowDir.x, -flowDir.z);
                    entityMoveAndSlide(e, grid, dt, player.position, PLAYER_HALF_WIDTH);
                }
            }

            // Flying drones stay airborne; ground NPCs snap to floor
            if (!(e.flags & ENT_FLYING)) snapEntityToFloor(e, grid);

            // Wall avoidance: nudge NPC toward cell center when near walls.
            // Check 4 cardinal neighbors — if any is solid, push away from it.
            {
                u32 gx, gz;
                if (LevelGridSystem::worldToGrid(grid, e.position, gx, gz)) {
                    Vec3 cc = LevelGridSystem::gridToWorld(grid, gx, gz);
                    cc.y = e.position.y;
                    // Gently steer toward cell center (avoids hugging walls)
                    Vec3 toCc = cc - e.position;
                    f32 offCenter = length(toCc);
                    if (offCenter > 0.15f) {
                        Vec3 nudge = toCc * 0.03f; // gentle centering force
                        if (!entityOverlapsGrid(e.position + nudge, e.halfExtents, grid)) {
                            e.position = e.position + nudge;
                        }
                    }
                }
            }

            // Push apart from other nearby friendly entities to prevent stacking
            for (u32 ni = 0; ni < pool.activeCount; ni++) {
                u32 nIdx = pool.activeList[ni];
                if (nIdx == i) continue;
                Entity& other = pool.entities[nIdx];
                if (!(other.flags & ENT_FRIENDLY)) continue;
                if (other.flags & ENT_DEAD) continue;
                // Early distance reject — skip entities more than 2m away
                f32 ddx = e.position.x - other.position.x;
                f32 ddz = e.position.z - other.position.z;
                if (ddx * ddx + ddz * ddz > 4.0f) continue;
                Vec3 diff = e.position - other.position;
                f32 dist2 = lengthSq(diff);
                f32 minDist = e.halfExtents.x + other.halfExtents.x;
                if (dist2 < minDist * minDist && dist2 > 0.001f) {
                    f32 overlap = minDist - sqrtf(dist2);
                    Vec3 push = normalize(diff) * (overlap * 0.5f + 0.02f);
                    Vec3 newPos = e.position + push;
                    if (!entityOverlapsGrid(newPos, e.halfExtents, grid)) {
                        e.position = newPos;
                    }
                }
            }

            // Speech timer decay
            if (e.speechTimer > 0.0f) {
                e.speechTimer -= dt;
                if (e.speechTimer <= 0.0f) e.speechText = nullptr;
            }
            // Low health speech
            if (e.health < e.maxHealth * 0.3f && e.health > 0 && e.speechTimer <= 0.0f) {
                if ((std::rand() % 120) == 0) {
                    static const char* hl[] = {"I'm hurt...", "Help!", "Can't... hold on..."};
                    e.speechText = hl[std::rand() % 3];
                    e.speechTimer = 3.0f;
                }
            }

            continue; // skip hostile AI path for friendly NPCs
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
                        targetPos = npc.position + Vec3{0, npc.halfExtents.y, 0};
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
                    targetPos = npc.position + Vec3{0, npc.halfExtents.y, 0};
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
            updateLegacyBossAbilities(e, i, pool, projectiles, player, targetPlayer,
                                      grid, dt, dist, playerEye);
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

        switch (e.aiState) {

        case AIState::IDLE: {
            // Idle roaming — enemies wander slowly when no target detected.
            // Uses flybyTimer as roam countdown (unused in IDLE for non-bats).
            e.flybyTimer -= dt;
            if (e.flybyTimer <= 0.0f) {
                // Pick a new random roam direction + duration
                f32 angle = (std::rand() % 628) * 0.01f; // 0 to 2pi
                f32 roamSpeed = e.moveSpeed * 0.3f;       // slow wander
                if (isBat) {
                    // Bats: gentle hover drift
                    e.velocity.x = sinf(angle) * roamSpeed * 0.5f;
                    e.velocity.z = cosf(angle) * roamSpeed * 0.5f;
                } else {
                    e.velocity.x = sinf(angle) * roamSpeed;
                    e.velocity.z = cosf(angle) * roamSpeed;
                }
                e.yaw = atan2f(-e.velocity.x, -e.velocity.z);
                // Roam for 0.5-1.5 seconds, then pause 2-4 seconds
                e.flybyTimer = 0.5f + (std::rand() % 100) * 0.01f; // roam duration
                e.flybyTarget.z = 1.0f; // flag: currently roaming
            } else if (e.flybyTarget.z > 0.0f) {
                // Currently roaming — move
                entityMoveAndSlide(e, grid, dt, player.position, PLAYER_HALF_WIDTH);
                if (!isBat) snapEntityToFloor(e, grid);
                // Check if roam time expired → pause
                if (e.flybyTimer <= 0.0f) {
                    e.velocity = {0, 0, 0};
                    e.flybyTimer = 2.0f + (std::rand() % 200) * 0.01f; // pause duration
                    e.flybyTarget.z = 0.0f; // flag: pausing
                }
            } else {
                // Pausing between roams
                e.velocity = {0, 0, 0};
            }

            // LOS check every 2 frames — fast detection
            // Smoke bomb: player is undetectable while smokeTimer > 0
            e.aiCheckIdx++;
            u16 checkFreq = 2;
            if (e.aiCheckIdx >= checkFreq && player.smokeTimer <= 0.0f) {
                e.aiCheckIdx = 0;
                // A static empty dungeon result is used as a fallback when no dungeon
                // is provided, so alertSquad always has valid adjacency data to read.
                static const DungeonResult s_emptyDungeon{};
                const DungeonResult& alertDungeon = dungeon ? *dungeon : s_emptyDungeon;

                if (targetIsNPC && targetDist <= e.detectionRange) {
                    e.aiState = AIState::CHASE;
                    e.velocity = {0, 0, 0};
                    if (squads) SquadSystem::alertSquad(*squads, static_cast<u16>(i), pool, alertDungeon);
                } else if (dist <= e.detectionRange && hasLOS(e, player, grid)) {
                    e.aiState = AIState::CHASE;
                    e.velocity = {0, 0, 0};
                    if (squads) SquadSystem::alertSquad(*squads, static_cast<u16>(i), pool, alertDungeon);
                } else if (dist <= e.detectionRange * 0.6f) {
                    // Close enough to hear — chase without LOS
                    e.aiState = AIState::CHASE;
                    e.velocity = {0, 0, 0};
                    if (squads) SquadSystem::alertSquad(*squads, static_cast<u16>(i), pool, alertDungeon);
                }
                // Aggro propagation handled by SquadSystem::alertSquad (no per-entity loop needed)
            }
        } break;

        case AIState::CHASE: {
            // Smoke bomb: enemies lose the player and return to idle
            if (player.smokeTimer > 0.0f) {
                e.aiState = AIState::IDLE;
                e.velocity = {0, 0, 0};
                break;
            }
            // Squad role: redirect flankers and hold-and-fire enemies to their tactical states.
            if (e.squadRole == SquadRole::ROLE_FLANK && !(e.flags & ENT_FLYING)) {
                if (e.pathLen == 0 || e.tacticalTimer <= 0.0f) {
                    Vec3 flankPos;
                    bool preferRight = (i % 2 == 0);
                    if (LevelGridQuery::findFlankCell(grid, e.position, targetPos,
                            e.attackRange, preferRight, flankPos)) {
                        e.pathLen = Pathfinder::findPath(grid, e.position, flankPos,
                            e.pathWaypoints);
                        e.pathIdx = 0;
                        if (e.pathLen > 0) {
                            e.aiState = AIState::FLANK;
                            e.tacticalTimer = 4.0f;
                            break;
                        }
                    }
                }
            }
            // HOLD role with ranged attack range: strafe while in sight instead of just chasing
            if (e.squadRole == SquadRole::ROLE_HOLD && e.attackRange > 5.0f) {
                if (targetDist <= e.attackRange && hasLOSToPoint(
                        e.position + Vec3{0, e.halfExtents.y, 0}, targetPos, grid)) {
                    e.aiState = AIState::STRAFE;
                    e.tacticalTimer = 1.0f;
                    break;
                }
            }

            if (isBat) {
                // Fly toward target but maintain minimum distance.
                // Ranged flyers (imps, attackRange > 5) keep far away and shoot.
                // Melee flyers (bats) hover closer for swooping attacks.
                bool isRangedFlyer = (e.attackRange > 5.0f);
                f32 hoverHeight = isRangedFlyer ? 2.5f : 1.5f;
                f32 minDist = isRangedFlyer ? e.attackRange * 0.85f : e.attackRange * 0.7f;

                Vec3 flyTarget = targetPos + Vec3{0, hoverHeight, 0};
                Vec3 toFly = flyTarget - e.position;
                f32 flyDist = length(toFly);

                if (flyDist > 0.01f) {
                    if (flyDist < minDist) {
                        // Too close — smoothly retreat with gentle lateral drift
                        Vec3 away = e.position - flyTarget;
                        f32 awayLen = length(away);
                        if (awayLen > 0.01f) away = away * (1.0f / awayLen);
                        Vec3 lateral = {-toFly.z, 0, toFly.x};
                        f32 latLen = sqrtf(lateral.x * lateral.x + lateral.z * lateral.z);
                        if (latLen > 0.01f) lateral = lateral * (1.0f / latLen);
                        // Ranged flyers retreat mostly backward with slight orbit;
                        // melee flyers orbit more aggressively
                        f32 retreatWeight = isRangedFlyer ? 0.7f : 0.2f;
                        f32 orbitWeight   = isRangedFlyer ? 0.3f : 0.6f;
                        e.velocity = (away * retreatWeight + lateral * orbitWeight) * effectiveSpeed;
                        e.velocity.y = (flyTarget.y - e.position.y) * 2.0f;
                    } else if (flyDist > e.attackRange * 0.95f || !isRangedFlyer) {
                        // Approach target (ranged flyers stop at 95% of attack range)
                        Vec3 flyDir = toFly * (1.0f / flyDist);
                        e.velocity = flyDir * effectiveSpeed;
                    } else {
                        // Ranged flyer in sweet spot — hold position, slight orbit
                        Vec3 lateral = {-toFly.z, 0, toFly.x};
                        f32 latLen = sqrtf(lateral.x * lateral.x + lateral.z * lateral.z);
                        if (latLen > 0.01f) lateral = lateral * (1.0f / latLen);
                        e.velocity = lateral * effectiveSpeed * 0.3f;
                        e.velocity.y = (flyTarget.y - e.position.y) * 2.0f;
                    }
                }
            } else {
                // Ground movement: direct chase when LOS, A* pathfinding when blocked.
                Vec3 moveDir = {0, 0, 0};
                bool hasDirectLOS = hasLOSToPoint(
                    e.position + Vec3{0, e.halfExtents.y, 0}, targetPos, grid);

                if (hasDirectLOS) {
                    // Direct line to target — walk straight, clear any stale path
                    moveDir = normalize(Vec3{dirToTarget.x, 0.0f, dirToTarget.z});
                    e.pathLen = 0;
                } else {
                    // No LOS — use A* pathfinding to navigate around obstacles.
                    // Recompute path every ~2s or when path is exhausted.
                    if (e.pathLen == 0 || e.pathIdx >= e.pathLen || e.tacticalTimer <= 0.0f) {
                        e.pathLen = Pathfinder::findPath(grid, e.position, targetPos,
                            e.pathWaypoints);
                        e.pathIdx = 0;
                        e.tacticalTimer = 2.0f; // recompute interval
                    }
                    e.tacticalTimer -= dt;

                    // Follow waypoints
                    if (e.pathLen > 0 && e.pathIdx < e.pathLen) {
                        Vec3 wp = e.pathWaypoints[e.pathIdx];
                        Vec3 toWp = wp - e.position;
                        f32 wpDist = sqrtf(toWp.x * toWp.x + toWp.z * toWp.z);
                        if (wpDist < 0.5f) {
                            e.pathIdx++; // reached waypoint, advance
                            if (e.pathIdx < e.pathLen) {
                                wp = e.pathWaypoints[e.pathIdx];
                                toWp = wp - e.position;
                            }
                        }
                        moveDir = normalize(Vec3{toWp.x, 0.0f, toWp.z});
                    } else {
                        // A* failed or exhausted — fallback to direct approach
                        moveDir = normalize(Vec3{dirToTarget.x, 0.0f, dirToTarget.z});
                    }
                }

                Vec3 flatDir = moveDir;
                if (lengthSq(flatDir) > 0.001f) {
                    // Stop and attack if already within attack range
                    if (targetDist <= e.attackRange * 0.9f) {
                        e.velocity.x = 0.0f;
                        e.velocity.z = 0.0f;
                        e.aiState = AIState::ATTACK;
                        e.attackTimer = 0.1f; // attack almost immediately
                    } else {
                        // Enemies sprint at 1.3x speed when chasing
                        f32 speed = effectiveSpeed * 1.3f;
                        if (e.enemyType == EnemyType::BOSS && e.flybyTarget.x > 1.5f) {
                            speed = effectiveSpeed * 2.5f;
                        }
                        e.velocity.x = flatDir.x * speed;
                        e.velocity.z = flatDir.z * speed;

                        // Anti-kite: accumulate kiteTimer when target holds distance;
                        // after 2 seconds burst to 2x speed for 3 seconds to close the gap.
                        if (targetDist > e.attackRange * 1.5f && targetDist < e.detectionRange) {
                            e.kiteTimer += dt;
                            if (e.kiteTimer > 2.0f && e.sprintTimer <= 0.0f) {
                                e.velocity.x = flatDir.x * effectiveSpeed * 2.0f;
                                e.velocity.z = flatDir.z * effectiveSpeed * 2.0f;
                                e.sprintTimer = 3.0f;
                                e.kiteTimer = 0.0f;
                            }
                        } else {
                            e.kiteTimer = 0.0f;
                        }
                        if (e.sprintTimer > 0.0f) e.sprintTimer -= dt;
                    }
                }
                snapEntityToFloor(e, grid);
            }

            entityMoveAndSlide(e, grid, dt, player.position, PLAYER_HALF_WIDTH);

            // Transition to attack if close enough to target.
            // Chargers use wider transition zone (1.3x range) so they don't overshoot.
            f32 atkTransition = (e.enemyRole & EnemyRole::CHARGER) ? e.attackRange * 1.3f : e.attackRange;
            if (targetDist <= atkTransition) {
                e.aiState = AIState::ATTACK;
                e.attackTimer = 0.3f; // fast first strike for all enemies
            }
            // Lost interest: fall back to idle only if target is extremely far
            if (targetDist > e.detectionRange * 5.0f) {
                e.aiState = AIState::IDLE;
            }
        } break;

        case AIState::FLYBY: {
            // Bat swoop attack — fly toward flyby waypoint, damage on contact with player
            Vec3 toFlybyTarget = e.flybyTarget - e.position;
            f32 flybyTargetDist = length(toFlybyTarget);

            if (flybyTargetDist > 0.3f) {
                Vec3 flyDir = toFlybyTarget * (1.0f / flybyTargetDist);
                f32 speed = e.moveSpeed * 2.2f; // aggressive fast dive
                e.velocity = flyDir * speed;

                // Fly straight during swoop — no wobble
            }

            // Face movement direction
            if (lengthSq(e.velocity) > 0.01f) {
                e.yaw = atan2f(-e.velocity.x, -e.velocity.z);
            }

            entityMoveAndSlide(e, grid, dt, player.position, PLAYER_HALF_WIDTH);

            // Deal damage when passing close to target
            if (dist <= e.attackRange * 0.8f) {
                if (targetIsNPC && e.targetEntityIdx < MAX_ENTITIES) {
                    // Damage the NPC target during flyby
                    Entity& npcTarget = pool.entities[e.targetEntityIdx];
                    if (!(npcTarget.flags & ENT_DEAD)) {
                        EntityHandle th = {e.targetEntityIdx, npcTarget.generation};
                        Combat::applyDamage(pool, th, e.damage);
                        e.attackAnimT = 0.3f;
                    }
                } else if (hasLOSToPoint(e.position, targetPlayer->position + Vec3{0, targetPlayer->eyeHeight, 0}, grid)) {
                    // Damage the targeted player (not just player 0); pass pool index for riposte
                    Combat::applyDamageToPlayer(*targetPlayer, e.damage, &e.position, static_cast<u16>(i));
                    e.attackAnimT = 0.3f;
                }

                if (e.attackAnimT > 0.0f) {
                    // After hitting: pull up and circle back
                    Vec3 retreatDir = e.velocity * (-1.0f);
                    if (lengthSq(retreatDir) > 0.01f) retreatDir = normalize(retreatDir);
                    else retreatDir = {0, 1, 0};
                    e.flybyTarget = e.position + retreatDir * 3.0f + Vec3{0, 2.5f, 0};
                    e.flybyTimer = 1.0f; // quick retreat, return to attack faster
                    // Stay in FLYBY to fly to retreat point, then expire to CHASE
                }
            }

            e.flybyTimer -= dt;
            if (flybyTargetDist < 0.3f || e.flybyTimer <= 0.0f) {
                e.aiState = AIState::CHASE;
            }
        } break;

        case AIState::ATTACK: {
            // Melee enemies pursue at 60% speed while attacking — keeps pressure on.
            // Ranged enemies stand still. Chargers maintain 80% speed.
            e.velocity = {0, 0, 0};
            bool isRangedAttacker = (e.attackRange > 5.0f);
            if (!isRangedAttacker && targetDist > e.attackRange * 0.4f) {
                // Pursue target while swinging — melee enemies don't stop moving
                Vec3 toTarget = targetPos - e.position;
                f32 tLen = sqrtf(toTarget.x * toTarget.x + toTarget.z * toTarget.z);
                if (tLen > 0.01f) {
                    f32 creepMult = (e.enemyRole & EnemyRole::CHARGER) ? 0.8f : 0.6f;
                    e.velocity.x = (toTarget.x / tLen) * e.moveSpeed * creepMult;
                    e.velocity.z = (toTarget.z / tLen) * e.moveSpeed * creepMult;
                }
            } else if (isRangedAttacker && targetDist < e.attackRange * 0.4f && targetDist > 0.1f) {
                // Ranged enemies back away if player is too close
                Vec3 away = e.position - targetPos;
                f32 awayLen = sqrtf(away.x * away.x + away.z * away.z);
                if (awayLen > 0.01f) {
                    e.velocity.x = (away.x / awayLen) * e.moveSpeed * 0.5f;
                    e.velocity.z = (away.z / awayLen) * e.moveSpeed * 0.5f;
                }
            }

            // Ranged enemies: check LOS before attacking (staggered every 4 frames).
            bool hostileIsRanged = (e.attackRange > 5.0f);
            if (hostileIsRanged && shouldCheckLOS) {
                Vec3 atkEye = e.position + Vec3{0, e.halfExtents.y, 0};
                bool canSee = hasLOSToPoint(atkEye, targetPos, grid);
                if (canSee) {
                    e.lastSeenPos = targetPos;
                    e.hasTargetLOS = true;
                } else {
                    e.hasTargetLOS = false;
                    // Move toward last seen position to regain LOS
                    Vec3 toLS = e.lastSeenPos - e.position;
                    f32 lsDist = length(toLS);
                    if (lsDist > 1.0f) {
                        Vec3 flatDir = normalize(Vec3{toLS.x, 0, toLS.z});
                        e.velocity.x = flatDir.x * effectiveSpeed;
                        e.velocity.z = flatDir.z * effectiveSpeed;
                        entityMoveAndSlide(e, grid, dt, player.position, PLAYER_HALF_WIDTH);
                        if (!(e.flags & ENT_FLYING)) snapEntityToFloor(e, grid);
                    } else {
                        // Reached last-seen spot but still no LOS — switch back to chase
                        e.aiState = AIState::CHASE;
                    }
                    break; // skip attack this frame
                }
            }

            e.attackTimer -= dt;
            if (e.attackTimer <= 0.0f) {
                e.attackTimer = e.attackCooldown;
                e.attackAnimT = 0.3f; // trigger attack animation

                // Ranged enemies fire projectiles; melee do direct damage.
                // Also apply on-hit status effects (poison/slow/burn/freeze).
                if (targetDist <= e.attackRange * 1.1f) {

                    if (hostileIsRanged) {
                        // Ranged hostile (imp, bone mage, demon): lead the shot using
                        // predicted target position to account for target movement.
                        Vec3 atkOrigin = e.position + Vec3{0, e.halfExtents.y, 0};
                        f32 projSpeed = 16.1f;  // was 14, +15%
                        f32 projRadius = 0.08f;
                        if (e.flags & ENT_FLYING) { projSpeed = 11.5f; projRadius = 0.06f; }
                        f32 timeToHit = dist / projSpeed;
                        Vec3 predictedPos = targetPos + targetVel * timeToHit;
                        Vec3 atkDir = normalize(predictedPos - atkOrigin);
                        ProjectileSystem::spawn(projectiles, atkOrigin,
                            atkDir, projSpeed, e.damage, projRadius, 3.0f, false);
                    } else if (targetIsNPC && e.targetEntityIdx < MAX_ENTITIES) {
                        Entity& npcTarget = pool.entities[e.targetEntityIdx];
                        if (!(npcTarget.flags & ENT_DEAD)) {
                            EntityHandle th = {e.targetEntityIdx, npcTarget.generation};
                            Combat::applyDamage(pool, th, e.damage);
                            // Apply on-hit effect to NPC
                            if (e.onHitEffect == 1) { npcTarget.poisonTimer = e.onHitDuration; npcTarget.poisonDps = e.onHitDps; }
                            if (e.onHitEffect == 3) { npcTarget.burnTimer   = e.onHitDuration; npcTarget.burnDps   = e.onHitDps; }
                            if (e.onHitEffect == 4) { npcTarget.freezeTimer = e.onHitDuration; }
                        }
                    } else if (hasLOSToPoint(e.position, targetPlayer->position + Vec3{0, targetPlayer->eyeHeight, 0}, grid)) {
                        // Pass entity pool index so dodge-through detection can riposte the correct attacker
                        Combat::applyDamageToPlayer(*targetPlayer, e.damage, &e.position, static_cast<u16>(i));
                        // Apply on-hit effect to player
                        if (e.onHitEffect == 1) { targetPlayer->poisonTimer = e.onHitDuration; targetPlayer->poisonDps = e.onHitDps; }
                        if (e.onHitEffect == 2) { targetPlayer->slowTimer   = e.onHitDuration; }
                        if (e.onHitEffect == 3) { targetPlayer->burnTimer   = e.onHitDuration; targetPlayer->burnDps   = e.onHitDps; }
                        if (e.onHitEffect == 4) { targetPlayer->freezeTimer = e.onHitDuration; }

                        // Drain heal: melee freeze enemies heal 50% of damage dealt (Mind Flayer)
                        if (e.onHitEffect == 4 && e.attackRange <= 5.0f) {
                            e.health = fminf(e.health + e.damage * 0.5f, e.maxHealth);
                        }

                        // Melee cleave: skeleton-type melee enemies also hit nearby friendlies.
                        // Bosses cleave in 3m at 50% damage; regular melee in 2m at 40%.
                        if (e.attackRange <= 5.0f && (e.enemyType == EnemyType::SKELETON ||
                            e.enemyType == EnemyType::BOSS)) {
                            f32 cleaveRadius = (e.bossDefIdx != 0xFF) ? 3.0f : 2.0f;
                            f32 cleaveMult   = (e.bossDefIdx != 0xFF) ? 0.5f : 0.4f;
                            f32 cleaveRadSq  = cleaveRadius * cleaveRadius;
                            for (u32 ca = 0; ca < pool.activeCount; ca++) {
                                u32 ci = pool.activeList[ca];
                                Entity& victim = pool.entities[ci];
                                if (!(victim.flags & ENT_FRIENDLY) || (victim.flags & ENT_DEAD)) continue;
                                if (lengthSq(victim.position - e.position) < cleaveRadSq) {
                                    EntityHandle vh = {static_cast<u16>(ci), victim.generation};
                                    Combat::applyDamage(pool, vh, e.damage * cleaveMult);
                                }
                            }
                        }
                    }
                }
            }

            // If target moved well out of attack range, switch back to CHASE
            if (targetDist > e.attackRange * 1.5f) {
                e.aiState = AIState::CHASE;
            }

            // Retreat when low HP: find cover and path toward it.
            // Only ground non-boss enemies retreat (flying enemies can always escape freely).
            if (e.health < e.maxHealth * 0.3f && !e.hasRetreated &&
                !(e.flags & ENT_FLYING) && e.enemyType != EnemyType::BOSS) {
                Vec3 coverPos;
                if (LevelGridQuery::findCoverCell(grid, e.position, targetPos, coverPos)) {
                    e.pathLen = Pathfinder::findPath(grid, e.position, coverPos,
                        e.pathWaypoints);
                    e.pathIdx = 0;
                    if (e.pathLen > 0) {
                        e.aiState = AIState::RETREAT;
                        e.tacticalTimer = 1.5f;
                        break;
                    }
                }
            }

            // Transition back to chase if out of range
            if (targetDist > e.attackRange * 1.3f) {
                e.aiState = AIState::CHASE;
            }
        } break;

        case AIState::DORMANT: {
            // Dormant: sits still until player gets close or combat happens nearby.
            e.velocity = {0, 0, 0};
            f32 triggerDist = (e.enemyRole & EnemyRole::AMBUSH) ? e.detectionRange * 0.5f : GameConst::MIMIC_TRIGGER_DIST;
            // Also wake if damaged (flashTimer from being hit) or if player is fighting nearby
            bool combatNearby = (e.flashTimer > 0.0f);
            if (dist <= triggerDist || combatNearby) {
                e.aiState = AIState::CHASE;
                e.attackTimer = 0.0f; // attack immediately on wake
                if (e.enemyRole & EnemyRole::AMBUSH) {
                    // Gargoyle: silent wake, no chomp animation
                    e.speechText = "...";
                    e.speechTimer = 1.5f;
                } else {
                    // Mimic: surprise chomp
                    e.attackAnimT = 0.4f;
                    e.speechText = "*CHOMP*";
                    e.speechTimer = 2.0f;
                }
            }
        } break;

        case AIState::FLANK: {
            // Follow A* path to a flanking position, then transition to ATTACK.
            // tacticalTimer acts as a timeout so flankers don't get stuck forever.
            if (e.pathIdx < e.pathLen) {
                Vec3 wp = e.pathWaypoints[e.pathIdx];
                Vec3 toWP = wp - e.position;
                f32 wpDist = length(Vec3{toWP.x, 0, toWP.z});
                if (wpDist < 0.5f) {
                    e.pathIdx++;
                } else {
                    Vec3 moveDir = normalize(Vec3{toWP.x, 0, toWP.z});
                    e.velocity.x = moveDir.x * effectiveSpeed * 1.2f;
                    e.velocity.z = moveDir.z * effectiveSpeed * 1.2f;
                    e.yaw = atan2f(-moveDir.x, -moveDir.z);
                }
            } else {
                // Arrived at flank position — switch to attack immediately
                e.aiState = AIState::ATTACK;
                e.attackTimer = 0.1f;
                e.hasRetreated = false;
            }
            entityMoveAndSlide(e, grid, dt, player.position, PLAYER_HALF_WIDTH);
            if (!(e.flags & ENT_FLYING)) snapEntityToFloor(e, grid);
            // Timeout: give up and chase directly if flanking takes too long
            e.tacticalTimer -= dt;
            if (e.tacticalTimer <= 0.0f) e.aiState = AIState::CHASE;
        } break;

        case AIState::RETREAT: {
            // Ranged enemies shoot while retreating — backpedal and fire.
            // Re-check LOS here since the ATTACK state's LOS update doesn't run.
            if (e.attackRange > 5.0f && dist <= e.attackRange && shouldCheckLOS) {
                Vec3 atkEye = e.position + Vec3{0, e.halfExtents.y, 0};
                Vec3 tgt = player.position + Vec3{0, player.eyeHeight, 0};
                e.hasTargetLOS = hasLOSToPoint(atkEye, tgt, grid);
            }
            if (e.attackRange > 5.0f && dist <= e.attackRange && e.hasTargetLOS) {
                e.attackTimer -= dt;
                if (e.attackTimer <= 0.0f) {
                    e.attackTimer = e.attackCooldown;
                    e.attackAnimT = 0.3f;
                    Vec3 atkOrigin = e.position + Vec3{0, e.halfExtents.y, 0};
                    Vec3 tPos = player.position + Vec3{0, player.eyeHeight, 0};
                    f32 projSpeed = (e.flags & ENT_FLYING) ? 11.5f : 16.1f;
                    f32 projRadius = (e.flags & ENT_FLYING) ? 0.06f : 0.08f;
                    f32 timeToHit = dist / projSpeed;
                    Vec3 predictedPos = tPos + player.velocity * timeToHit;
                    Vec3 atkDir = normalize(predictedPos - atkOrigin);
                    ProjectileSystem::spawn(projectiles, atkOrigin,
                        atkDir, projSpeed, e.damage, projRadius, 3.0f, false);
                }
            }
            // Re-engage once at preferred range — ranged enemies stay in retreat
            // while the player is too close so they keep backpedaling and firing.
            bool rangedTooClose = (e.attackRange > 5.0f && dist < e.attackRange * 0.5f);
            if (player.health > 0.0f && dist <= e.detectionRange && !rangedTooClose) {
                e.aiState = AIState::CHASE;
                e.velocity = {0, 0, 0};
                break;
            }
            // Follow A* path to cover, then hold position briefly.
            // hasRetreated is set after reaching cover so enemies only retreat once per fight.
            if (e.pathIdx < e.pathLen) {
                Vec3 wp = e.pathWaypoints[e.pathIdx];
                Vec3 toWP = wp - e.position;
                f32 wpDist = length(Vec3{toWP.x, 0, toWP.z});
                if (wpDist < 0.5f) {
                    e.pathIdx++;
                } else {
                    Vec3 moveDir = normalize(Vec3{toWP.x, 0, toWP.z});
                    // Ranged: kite at 70% speed facing player; melee: sprint away
                    f32 retreatMult = (e.attackRange > 5.0f) ? 0.8f : 1.4f;
                    e.velocity.x = moveDir.x * effectiveSpeed * retreatMult;
                    e.velocity.z = moveDir.z * effectiveSpeed * retreatMult;
                    if (e.attackRange > 5.0f) {
                        // Face the player while kiting
                        Vec3 toPlayer = player.position - e.position;
                        e.yaw = atan2f(-toPlayer.x, -toPlayer.z);
                    } else {
                        e.yaw = atan2f(-moveDir.x, -moveDir.z);
                    }
                }
            } else {
                // No path or reached end — ranged enemies backpedal directly
                // away from the player instead of standing still.
                if (e.attackRange > 5.0f && dist < e.attackRange * 0.8f) {
                    // Backpedal at 70% speed while facing the player
                    Vec3 away = e.position - player.position;
                    away.y = 0.0f;
                    if (lengthSq(away) > 0.01f) {
                        away = normalize(away);
                        e.velocity.x = away.x * effectiveSpeed * 0.8f;
                        e.velocity.z = away.z * effectiveSpeed * 0.8f;
                        Vec3 toPlayer = player.position - e.position;
                        e.yaw = atan2f(-toPlayer.x, -toPlayer.z);
                    }
                } else {
                    // Melee or far enough — check home position
                    Vec3 toHome = e.homePosition - e.position;
                    f32 homeDist2 = toHome.x * toHome.x + toHome.z * toHome.z;
                    if (homeDist2 < 2.0f * 2.0f) {
                        e.velocity = {0, 0, 0};
                        e.aiState = AIState::IDLE;
                        e.hasRetreated = false;
                        break;
                    }
                    e.velocity = {0, 0, 0};
                }
            }
            entityMoveAndSlide(e, grid, dt, player.position, PLAYER_HALF_WIDTH);
            if (!(e.flags & ENT_FLYING)) snapEntityToFloor(e, grid);
            e.tacticalTimer -= dt;
            if (e.tacticalTimer <= 0.0f) {
                e.aiState = AIState::CHASE;
                e.hasRetreated = true; // prevents retreating again this fight
            }
        } break;

        case AIState::AMBUSH: {
            // Hold position until player is very close, then burst into attack.
            // Used for mimic-style or pre-positioned ambush setups.
            e.velocity = {0, 0, 0};
            Vec3 toTarget2 = targetPos - e.position;
            if (lengthSq(toTarget2) > 0.01f) {
                e.yaw = atan2f(-toTarget2.x, -toTarget2.z);
            }
            if (dist <= 4.0f && hasLOS(e, player, grid)) {
                e.aiState = AIState::ATTACK;
                e.attackTimer = 0.0f; // burst immediately on reveal
            }
        } break;

        case AIState::STRAFE: {
            // Lateral movement while maintaining attack range — used by HOLD-role ranged enemies.
            // tacticalTimer controls strafe direction: positive = strafe right, negative = strafe left.
            Vec3 toTargetS = targetPos - e.position;
            f32 targetLenS = length(Vec3{toTargetS.x, 0, toTargetS.z});
            if (targetLenS > 0.01f) {
                Vec3 forward = Vec3{toTargetS.x, 0, toTargetS.z} * (1.0f / targetLenS);
                f32 side = (e.tacticalTimer > 0.0f) ? 1.0f : -1.0f;
                Vec3 lateral = {-forward.z * side, 0, forward.x * side};
                e.velocity.x = lateral.x * effectiveSpeed * 0.7f;
                e.velocity.z = lateral.z * effectiveSpeed * 0.7f;
                e.yaw = atan2f(-forward.x, -forward.z);
            }
            entityMoveAndSlide(e, grid, dt, player.position, PLAYER_HALF_WIDTH);
            if (!(e.flags & ENT_FLYING)) snapEntityToFloor(e, grid);

            // Flip strafe direction periodically with a bit of randomness
            e.tacticalTimer -= dt;
            if (e.tacticalTimer <= -1.5f) e.tacticalTimer = 1.0f + (std::rand() % 100) * 0.01f;

            // Fire while strafing — predict target position for leading the shot
            e.attackTimer -= dt;
            if (e.attackTimer <= 0.0f && hasLOSToPoint(
                    e.position + Vec3{0, e.halfExtents.y, 0}, targetPos, grid)) {
                e.attackTimer = e.attackCooldown;
                e.attackAnimT = 0.3f;
                Vec3 atkOrigin = e.position + Vec3{0, e.halfExtents.y, 0};
                f32 projSpeed = 14.0f;
                f32 timeToHit = dist / projSpeed;
                Vec3 predictedPos = targetPos + targetVel * timeToHit;
                Vec3 atkDir = normalize(predictedPos - atkOrigin);
                ProjectileSystem::spawn(projectiles, atkOrigin,
                    atkDir, projSpeed, e.damage, 0.08f, 3.0f, false);
            }

            // Return to chase if target moved out of comfortable range
            if (targetDist < e.attackRange * 0.5f) e.aiState = AIState::CHASE;
            if (targetDist > e.attackRange * 1.5f) e.aiState = AIState::CHASE;
        } break;

        case AIState::SURROUND: {
            // Move toward a spread position around the target based on entity pool slot.
            // When close enough, switch to ATTACK. Used by squad leaders to position members.
            Vec3 goalPos = LevelGridQuery::getSurroundPosition(
                targetPos, static_cast<u8>(i % 6), 4, e.attackRange * 0.8f);
            Vec3 toGoal = goalPos - e.position;
            f32 goalDist = length(Vec3{toGoal.x, 0, toGoal.z});
            if (goalDist > 0.5f) {
                Vec3 moveDir = normalize(Vec3{toGoal.x, 0, toGoal.z});
                e.velocity.x = moveDir.x * effectiveSpeed;
                e.velocity.z = moveDir.z * effectiveSpeed;
                e.yaw = atan2f(-moveDir.x, -moveDir.z);
            } else {
                // In position — begin attacking
                e.aiState = AIState::ATTACK;
                e.attackTimer = 0.2f;
            }
            entityMoveAndSlide(e, grid, dt, player.position, PLAYER_HALF_WIDTH);
            if (!(e.flags & ENT_FLYING)) snapEntityToFloor(e, grid);
        } break;

        case AIState::DEAD:
            break;
        }
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

    // --- Stuck detection for hostile enemies ---
    // Mirrors the friendly NPC stuck detection. Teleports to cell center after 0.8s stuck.
    for (u32 a = 0; a < pool.activeCount; a++) {
        u32 idx = pool.activeList[a];
        Entity& e = pool.entities[idx];
        if (!(e.flags & ENT_ACTIVE) || (e.flags & ENT_DEAD)) continue;
        if (e.flags & ENT_FRIENDLY) continue; // friendlies already have their own stuck detection
        if (e.aiState != AIState::CHASE) continue; // only check during active pursuit

        f32 movedDist = length(e.position - e.lastSeenPos);
        if (movedDist < 0.05f) {
            e.stuckTimer += dt;
            if (e.stuckTimer > 0.8f) {
                // Try A* path to target as recovery
                e.pathLen = Pathfinder::findPath(grid, e.position,
                    player.position, e.pathWaypoints);
                e.pathIdx = 0;

                // If A* also fails, teleport to nearest walkable cell center
                if (e.pathLen == 0) {
                    u32 gx, gz;
                    if (LevelGridSystem::worldToGrid(grid, e.position, gx, gz)) {
                        Vec3 cellCenter = LevelGridSystem::gridToWorld(grid, gx, gz);
                        cellCenter.y = e.position.y;
                        if (!entityOverlapsGrid(cellCenter, e.halfExtents, grid)) {
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
