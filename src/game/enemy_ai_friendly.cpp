// enemy_ai_friendly.cpp — Friendly NPC AI for EnemyAI::update.
// Handles party NPCs (Paladin/Cleric/Archer/Mage/Rogue) and player-owned drones.
// Each tick: stuck-detection, target scan, class-specific combat or flow-field
// pathing, NPC separation, and speech. Friendly entities skip the hostile AI
// path, so this function always returns AIStep::NextEntity.
//
// Reads file-scope statics s_frameTick, s_friendlyGroupCenter,
// s_friendlyGroupCount, s_droneSpawnCb from enemy_ai_internal.h.
// See CLAUDE.md "Data Lifecycles" for entity handle usage.

#include "game/enemy_ai_internal.h"
#include "game/game_constants.h"
#include <cmath>
#include <cstdlib>

AIStep updateFriendlyNPC(Entity& e, u32 i,
                          EntityPool& pool, ProjectilePool& projectiles,
                          Vec3 anchorPos, Player* anchorPlayer,
                          const LevelGrid& grid, f32 dt,
                          Vec3 playerEye)
{
    // --- TOWNSFOLK mode: hold the post, greet visitors, make small talk. Replaces the whole
    // companion brain (combat, healing, flow-field pathfinding — there is no exit to find and
    // nothing to fight). Wander leash keeps them near homePosition so the plaza stays composed.
    if (EnemyAI::townMode()) {
        Vec3 toHome = e.homePosition - e.position;
        toHome.y = 0.0f;
        f32 homeDist = length(toHome);
        if (homeDist > 2.0f) {
            Vec3 dir = toHome * (1.0f / homeDist);
            e.velocity.x = dir.x * e.moveSpeed * 0.35f;   // amble, don't march
            e.velocity.z = dir.z * e.moveSpeed * 0.35f;
            e.yaw = atan2f(-dir.x, -dir.z);
        } else {
            e.velocity.x = 0.0f;
            e.velocity.z = 0.0f;
            // Face a nearby visitor — a townsfolk that ignores you reads as furniture.
            Vec3 toP = anchorPos - e.position;
            f32 pd = length(Vec3{toP.x, 0.0f, toP.z});
            if (pd < 5.0f && pd > 0.01f) e.yaw = atan2f(-toP.x / pd, -toP.z / pd);
        }
        // Ambient small talk on a slow randomized clock (tacticalTimer is free here).
        e.tacticalTimer -= dt;
        if (e.tacticalTimer <= 0.0f) {
            e.tacticalTimer = 14.0f + static_cast<f32>(std::rand() % 12);
            if (std::rand() % 2 == 0) {   // sometimes just stand quietly
                static const char* clericLines[] = {"The light is warm out here.", "Rest a while, hero.", "It is over. Truly."};
                static const char* rogueLines[]  = {"Quiet around here. I like it.", "Watch your purse.", "I kept a few souvenirs."};
                static const char* archerLines[] = {"Fine weather for fletching.", "No more corridors. Good.", "The sky. Look at it."};
                const char** lines = (e.npcClass == NpcClass::CLERIC) ? clericLines
                                   : (e.npcClass == NpcClass::ROGUE)  ? rogueLines : archerLines;
                e.speechText  = lines[std::rand() % 3];
                e.speechTimer = 3.0f;
            }
        }
        entityMoveAndSlide(e, grid, dt, anchorPos, PLAYER_HALF_WIDTH);
        snapEntityToFloor(e, grid);
        return AIStep::NextEntity;
    }

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
    // Minipet (PET): cosmetic companion — trots after its owner and does
    // nothing else. No target scan, no combat, no flow-field exit march.
    // Hostiles never see it (spawned ENT_UNTARGETABLE) and applyDamage
    // ignores it, so there is no death/respawn handling here either.
    // ---------------------------------------------------------------
    if (e.npcClass == NpcClass::PET) {
        f32 dpx = anchorPos.x - e.position.x, dpz = anchorPos.z - e.position.z;
        f32 distToOwner = sqrtf(dpx * dpx + dpz * dpz);
        if (distToOwner > 25.0f) {
            // Hopelessly separated (owner portaled/teleported) — pop to their
            // side, the same recovery the Tinkerer drones use at 30 m.
            e.position = anchorPos + Vec3{1.0f, 0.0f, 1.0f};
            Collision::ensureNotInWall(e.position, e.halfExtents, grid);
            e.velocity = {0, 0, 0};
        } else if (distToOwner > 2.0f) {
            Vec3 toOwner = normalize(Vec3{dpx, 0.0f, dpz});
            // Scamper harder the further it falls behind, so it catches a
            // sprinting owner instead of permanently trailing them.
            f32 urgency = (distToOwner > 6.0f) ? 1.35f : 1.0f;
            e.velocity.x = toOwner.x * npcSpeed * urgency;
            e.velocity.z = toOwner.z * npcSpeed * urgency;
            e.yaw = atan2f(-toOwner.x, -toOwner.z);
        } else {
            // Heel: stand by the owner, facing them.
            e.velocity.x = 0.0f;
            e.velocity.z = 0.0f;
            if (distToOwner > 0.01f) e.yaw = atan2f(-dpx, -dpz);
        }
        entityMoveAndSlide(e, grid, dt, anchorPos, PLAYER_HALF_WIDTH);
        snapEntityToFloor(e, grid);
        return AIStep::NextEntity;
    }

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
            // Heal the owning player first — only when the owner is local (anchorPlayer
            // non-null). A remote-cast Cleric can't reach its owner's HP from this host,
            // so it falls through to healing nearby NPCs instead.
            if (anchorPlayer && anchorPlayer->health < anchorPlayer->maxHealth * 0.5f &&
                length(playerEye - e.position) < healRange) {
                f32 amt = 8.0f + e.level * 0.5f;
                anchorPlayer->health += amt;
                if (anchorPlayer->health > anchorPlayer->maxHealth) anchorPlayer->health = anchorPlayer->maxHealth;
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
            // Flying bats hunt out to their detection range; the ground turret bot
            // (GENERIC body) fires at its real attackRange; ground spider combat-drones
            // stay short-range melee at 6 m.
            if (e.flags & ENT_FLYING)                   engageDist = e.detectionRange;
            else if (e.enemyType == EnemyType::GENERIC) engageDist = e.attackRange;
            else                                        engageDist = 6.0f;
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
            entityMoveAndSlide(e, grid, dt, anchorPos, PLAYER_HALF_WIDTH);
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

        entityMoveAndSlide(e, grid, dt, anchorPos, PLAYER_HALF_WIDTH);

        // -- Attack on cooldown --
        // Ranged NPCs ALWAYS require LOS to fire (prevents shooting walls)
        if (e.npcClass != NpcClass::CLERIC || eDist <= e.attackRange) {
            bool canAttack = false;
            if (e.npcWeaponType == WeaponType::PROJECTILE) {
                // Projectile: must have LOS regardless of distance
                canAttack = (eDist <= engageDist && e.hasTargetLOS);
            } else {
                // Melee: in range AND line of sight. Without the LOS gate a drone
                // (e.g. a spider with 4 m attackRange) deals its damage directly through
                // a wall to any enemy within range on the other side.
                canAttack = (eDist <= e.attackRange && e.hasTargetLOS);
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
                    entityMoveAndSlide(e, grid, dt, anchorPos, PLAYER_HALF_WIDTH);
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
            // Mobile turret bot (idle path — combat is handled by the inCombat block above):
            // follow the player first; once within the player's range, drift toward the level
            // exit via the flow field so the turret advances with the party instead of just
            // trailing behind it.
            constexpr f32 FOLLOW_RANGE = 7.0f; // "the player's range" — stay within this
            f32 dpx = anchorPos.x - e.position.x, dpz = anchorPos.z - e.position.z;
            f32 distToPlayer = sqrtf(dpx * dpx + dpz * dpz);
            if (distToPlayer > FOLLOW_RANGE) {
                // Too far — catch up to the player
                Vec3 toPlayer = normalize(Vec3{dpx, 0, dpz});
                e.velocity.x = toPlayer.x * npcSpeed;
                e.velocity.z = toPlayer.z * npcSpeed;
                e.yaw = atan2f(-toPlayer.x, -toPlayer.z);
            } else {
                // Within range — head toward the exit via the flow field
                Vec3 flowDir = LevelGridSystem::flowDirection(grid, e.position);
                if (lengthSq(flowDir) > 0.001f) {
                    e.velocity.x = flowDir.x * npcSpeed;
                    e.velocity.z = flowDir.z * npcSpeed;
                    e.yaw = atan2f(-flowDir.x, -flowDir.z);
                } else {
                    e.velocity = {0, 0, 0}; // at the exit
                }
            }
            entityMoveAndSlide(e, grid, dt, anchorPos, PLAYER_HALF_WIDTH);
            if (!(e.flags & ENT_FLYING)) snapEntityToFloor(e, grid);
        } else if (isCombatDrone) {
            // Combat drone (spider): orbit player at unique angle, spread out.
            // Each drone gets a distinct phase so they fan around the player.
            f32 phase = static_cast<f32>(i) * 1.618f * 6.2832f; // golden angle spread
            f32 orbitDist = 3.0f + sinf(e.animTimer * 0.8f + phase) * 1.5f;
            Vec3 orbitTarget = anchorPos + Vec3{
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
            entityMoveAndSlide(e, grid, dt, anchorPos, PLAYER_HALF_WIDTH);
        } else {
            // Swarm drones (flying bats): orbit player at different heights and angles.
            // Wide spread with varying orbit radii for a real swarm look.
            f32 phase = static_cast<f32>(i) * 2.399f; // golden angle (137.5°)
            f32 orbitRadius = 2.5f + sinf(phase * 3.0f) * 2.0f; // 0.5–4.5m spread
            f32 orbitSpeed = 1.2f + sinf(phase * 2.0f) * 0.5f;  // varied orbit speed
            f32 angle = e.animTimer * orbitSpeed + phase;
            Vec3 orbitTarget = anchorPos + Vec3{
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

            entityMoveAndSlide(e, grid, dt, anchorPos, PLAYER_HALF_WIDTH);
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
            entityMoveAndSlide(e, grid, dt, anchorPos, PLAYER_HALF_WIDTH);
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

    // Original code: `continue;` — skip to next entity (hostile AI path).
    return AIStep::NextEntity;
}
