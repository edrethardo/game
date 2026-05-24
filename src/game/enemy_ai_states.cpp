// enemy_ai_states.cpp — Hostile entity AI state machine for EnemyAI::update.
// Implements switch(e.aiState) for IDLE/CHASE/FLYBY/ATTACK/DORMANT/FLANK/
// RETREAT/AMBUSH/STRAFE/SURROUND/DEAD. All `break` statements are switch-
// internal (exit the switch, not the outer entity loop), so this function
// returns void. Called after preamble + role modifiers are resolved in the
// loop spine.
// See CLAUDE.md "Data Lifecycles" for entity handle usage and the AIStep convention.

#include "game/enemy_ai_internal.h"
#include "game/game_constants.h"
#include <cmath>
#include <cstdlib>

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
                          bool spawnCalm)
{
    // Stealth (Shadow Step / smoke bomb): an enemy actively engaging the PLAYER
    // loses track and disengages. Enemies engaging a friendly NPC (targetIsNPC)
    // are unaffected. ATTACK is intentionally NOT handled here — an enemy already
    // attacking gets to land a swing due this frame (see end of ATTACK case),
    // matching the "finish current swing only" rule. Passive IDLE/DORMANT/AMBUSH
    // are gated in their own cases so they don't get yanked out of position.
    if (player.smokeTimer > 0.0f && !targetIsNPC) {
        switch (e.aiState) {
            case AIState::CHASE:
            case AIState::FLANK:
            case AIState::STRAFE:
            case AIState::RETREAT:
            case AIState::SURROUND:
            case AIState::FLYBY:
                e.aiState   = AIState::IDLE;
                e.velocity  = {0, 0, 0};
                return;
            default: break;
        }
    }

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
        // spawnCalm: during the floor-start calm window, idle enemies still roam
        // (above) but never auto-aggro the player/NPCs. Damage-driven aggro in
        // Combat::applyDamage is unaffected, so hitting an enemy still wakes it.
        if (!spawnCalm && e.aiCheckIdx >= checkFreq && player.smokeTimer <= 0.0f) {
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

        // Stealth: the swing/shot due this frame (if any) has already resolved
        // above; now the player has slipped away, so drop the target. One final
        // hit can land, but the enemy cannot keep attacking or re-acquire while
        // stealth holds.
        if (player.smokeTimer > 0.0f && !targetIsNPC) {
            e.aiState  = AIState::IDLE;
            e.velocity = {0, 0, 0};
            break;
        }
    } break;

    case AIState::DORMANT: {
        // Dormant: sits still until player gets close or combat happens nearby.
        e.velocity = {0, 0, 0};
        f32 triggerDist = (e.enemyRole & EnemyRole::AMBUSH) ? e.detectionRange * 0.5f : GameConst::MIMIC_TRIGGER_DIST;
        // Also wake if damaged (flashTimer from being hit) or if player is fighting nearby
        bool combatNearby = (e.flashTimer > 0.0f);
        if ((dist <= triggerDist && player.smokeTimer <= 0.0f) || combatNearby) {
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
        if (dist <= 4.0f && hasLOS(e, player, grid) && player.smokeTimer <= 0.0f) {
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
