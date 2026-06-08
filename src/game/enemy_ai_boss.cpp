// enemy_ai_boss.cpp — Boss ability dispatch for EnemyAI::update.
// Handles the legacy per-floor boss abilities (switch on the 1-50 milestone
// floor, recovered from e.level so it works on every difficulty) plus
// personality-system delegation via BossAI::update and LOS-duration aggro.
// Called once per entity per tick, only when e.enemyType == EnemyType::BOSS.
// See CLAUDE.md "Data Lifecycles" for entity handle usage and death flow.

#include "game/enemy_ai_internal.h"
#include "game/boss_ai.h"
#include "game/game_constants.h"
#include <cmath>
#include <cstdlib>

void updateLegacyBossAbilities(Entity& e, u32 i,
                                EntityPool& pool, ProjectilePool& projectiles,
                                Player& player, Player* targetPlayer,
                                const LevelGrid& grid, f32 dt,
                                f32 dist, Vec3 playerEye)
{
    // Delegate to BossAI personality system if this boss has a loaded def.
    // Skipped while ENTOMBING so the boss stays rooted during its false-death channel.
    if (e.bossDefIdx != 0xFF && s_bossDefTable && e.bossDefIdx < s_bossDefTable->count
        && e.bossPhase != BossPhase::ENTOMBING) {
        // Pass the chased target (nearest local player) so boss movement/teleport/LOS
        // track whoever the boss is engaging, not always P1.
        BossAI::update(e, s_bossDefTable->defs[e.bossDefIdx], pool, projectiles, *targetPlayer, grid, dt);
    }

    bool bossLOS = dist < 20.0f && hasLOS(e, *targetPlayer, grid);

    // Difficulty-proof boss-ability keying: e.level is the *effective* floor
    // (raw floor + difficulty*50), so on the floor-50→1 difficulty loop the
    // milestone floors become 55,60,... and a raw `switch(e.level)` would miss
    // every boss. Recover the 1–50 milestone floor so each boss keeps its
    // signature ability on every difficulty. (Maps 5→5, 50→50, 55→5, 100→50.)
    u32 rawFloor = ((e.level - 1) % 50) + 1;

    // --- False-death phase machine (Malachar, floor 20) -------------------------
    // ENTOMBING: rooted, invulnerable channel (combat.cpp set sprintTimer = -1 as the
    // "not yet started" sentinel). Init once, count down, then erupt the guardians
    // and seal the boss (minionShield). The caller (enemy_ai.cpp) also skips the
    // FSM/role passes while ENTOMBING so he doesn't move.
    if (e.bossPhase == BossPhase::ENTOMBING) {
        e.velocity = {0, 0, 0};
        if (e.sprintTimer < 0.0f) {
            e.sprintTimer = 2.5f; // channel duration
            e.speechText  = "I am bound to this tomb — you cannot end me!";
            e.speechTimer = 3.0f;
            // Sarcophagus telegraph: a ring of purple bursts around the warden.
            for (u32 s = 0; s < 6; s++) {
                f32 a = s * (6.2832f / 6.0f);
                BossAI::magicBurst(e.position + Vec3{cosf(a) * 1.8f, 0.3f, sinf(a) * 1.8f},
                                   0.6f, 0.2f, 0.8f, 6);
            }
        } else {
            e.sprintTimer -= dt;
            if (e.sprintTimer <= 0.0f) {
                // Erupt 4 tomb-guardians in a ring; minionShield seals the boss until they die.
                f32 bossDmg = e.damage;
                for (u32 s = 0; s < 4; s++) {
                    f32 a = s * (6.2832f / 4.0f) + e.animTimer;
                    Vec3 pos = e.position + Vec3{sinf(a) * 2.5f, 0.0f, cosf(a) * 2.5f};
                    EntityHandle sh = EntitySystem::spawn(pool, pos,
                        {0.4f, 0.9f, 0.4f}, false,
                        e.maxHealth * 0.10f, 4.0f, 14.0f, 2.2f, 0.8f, bossDmg * 0.20f);
                    Entity* g = handleGet(pool, sh);
                    if (g) {
                        g->meshId             = s_skeletonMeshId;
                        g->materialId         = s_skeletonMatId;
                        g->enemyType          = EnemyType::SKELETON;
                        g->aiState            = AIState::CHASE;
                        g->level              = e.level;
                        g->baseMoveSpeed      = g->moveSpeed;
                        g->baseAttackCooldown = g->attackCooldown;
                        g->spawnerIdx         = static_cast<u16>(i); // drives minionShield
                        BossAI::magicBurst(pos, 0.6f, 0.2f, 0.8f, 8);
                    }
                }
                e.minionShield = true;
                e.bossPhase    = BossPhase::SEALED;
                e.speechText   = "Arise, wardens! Bind the intruder!";
                e.speechTimer  = 3.0f;
            }
        }
        e.attackAnimT = 0.0f;
        return; // entombed: no aggro/abilities this tick (movement skipped by caller)
    }
    if (e.bossPhase == BossPhase::SEALED && e.bossDefIdx != 0xFF &&
        !BossAI::hasMinionAlive(pool, static_cast<u16>(i))) {
        // Seal broken — every guardian is dead. Drop the shield and enrage for the finish.
        e.minionShield = false;
        e.bossPhase    = BossPhase::ENRAGED;
        e.speechText   = "My bonds... SHATTERED! Then I will kill you MYSELF!";
        e.speechTimer  = 3.0f;
        BossAI::magicBurst(e.position + Vec3{0, 1.0f, 0}, 0.9f, 0.2f, 0.9f, 24);
    }

    // Track LOS duration (repurpose flybyTarget.x as timer)
    if (bossLOS) {
        e.flybyTarget.x += dt;
    } else {
        e.flybyTarget.x = 0.0f;
    }

    // After 1.5s of LOS, force CHASE if still idle and sprint
    if (e.flybyTarget.x > 1.5f && e.aiState == AIState::IDLE) {
        e.aiState = AIState::CHASE;
        // Boss aggro speech — use nameTag for personalized line
        if (e.speechTimer <= 0.0f) {
            if (rawFloor == 15) {
                // Sethrak the lich — themed aggro rambling
                e.speechText = "Flesh is fleeting; bone is FOREVER. Kneel before Sethrak!";
                e.speechTimer = 3.0f;
            } else if (e.nameTag) {
                // Each boss has a unique aggro line based on identity
                e.speechText = "You dare challenge me!";
                e.speechTimer = 3.0f;
            }
        }
    }

    // --- Boss ability on cooldown (flybyTimer) ---
    e.flybyTimer -= dt;
    if (e.flybyTimer <= 0.0f && bossLOS) {
        Vec3 bossEye = e.position + Vec3{0, e.halfExtents.y, 0};
        // Aim at the player's torso, not the eye. A tall boss fires from well above
        // the player; aiming at the eye (the very top of the 1.8m hitbox) makes fast
        // bolts graze over the head and miss every frame. The torso point sends them
        // down through the body so the AABB test connects. (Charges re-flatten to XZ,
        // so their behaviour is unchanged.)
        Vec3 playerAim   = targetPlayer->position + Vec3{0.0f, 0.9f, 0.0f};
        Vec3 toPlayerDir = normalize(playerAim - bossEye);
        f32 bossDmg = e.damage;

        switch (rawFloor) {

        // Floor 5: The Butcher — cleaver throw then sprint to close the gap
        case 5: {
            e.flybyTimer = 4.0f;
            u16 pi5 = ProjectileSystem::spawn(projectiles, bossEye,
                toPlayerDir, 18.0f, bossDmg * 0.4f, 0.15f, 3.0f, false);
            if (pi5 != 0xFFFF) projectiles.projectiles[pi5].meshId = e.weaponMeshId;
            // Speed boost after throw — chase the target down
            e.moveSpeed *= 1.10f;
            e.speechText = "DIE!";
            e.speechTimer = 2.0f;
        } break;

        // Floor 10: Ygara, the Broodqueen — venom spit (an aimed poison-glob spread
        // that fits her venomous theme), escalating to a poison nova ring once
        // bloodied. EVERY projectile she throws applies poison and glows venom-green.
        case 10: {
            // Hybrid cadence: when she can't reach you she spits venom rapidly
            // (a real ranged threat while she closes the gap), then eases off once
            // in melee range so she mixes poison swings with the occasional spit.
            e.flybyTimer = (dist > e.attackRange * 1.5f) ? 1.6f : 4.0f;
            const Vec3 venomGlow = {0.35f, 0.95f, 0.2f}; // sickly green venom light

            // Signature ranged attack: a 3-glob venom spit aimed at the player.
            for (s32 s = -1; s <= 1; s++) {
                f32 spread = s * 0.13f; // ~7.5° fan around the aim line
                Vec3 dir = normalize(Vec3{
                    toPlayerDir.x + spread * toPlayerDir.z,
                    toPlayerDir.y,
                    toPlayerDir.z - spread * toPlayerDir.x});
                u16 pi = ProjectileSystem::spawn(projectiles, bossEye,
                    dir, 15.0f, bossDmg * 0.5f, 0.18f, 3.0f, false);
                if (pi != 0xFFFF) {
                    auto& pr = projectiles.projectiles[pi];
                    pr.onHitEffect   = 1;          // poison
                    pr.onHitDuration = 3.0f;
                    pr.lightColor    = venomGlow;
                }
            }

            // Bloodied (<50% HP): also erupt the classic poison nova ring for area
            // denial — now actually poisonous (the old nova set no on-hit effect).
            f32 hpPct = (e.maxHealth > 0.0f) ? (e.health / e.maxHealth) : 1.0f;
            if (hpPct < 0.5f) {
                for (u32 s = 0; s < 8; s++) {
                    f32 angle = s * (6.2832f / 8.0f);
                    Vec3 dir = {sinf(angle), 0.0f, cosf(angle)};
                    u16 pi = ProjectileSystem::spawn(projectiles, bossEye,
                        dir, 10.0f, bossDmg * 0.4f, 0.14f, 2.5f, false);
                    if (pi != 0xFFFF) {
                        auto& pr = projectiles.projectiles[pi];
                        pr.onHitEffect   = 1;       // poison
                        pr.onHitDuration = 3.0f;
                        pr.lightColor    = venomGlow;
                    }
                }
                e.speechText = "DROWN IN MY VENOM!";
            } else {
                e.speechText = "TASTE MY VENOM!";
            }
            e.speechTimer = 2.0f;
        } break;

        // Floor 15: Lich Lord — Frost Bolt fan (3 spread projectiles)
        case 15: {
            e.flybyTimer = 3.0f;
            for (s32 s = -1; s <= 1; s++) {
                f32 spread = s * 0.15f; // ~8.5 degrees
                Vec3 dir = normalize(Vec3{
                    toPlayerDir.x + spread * toPlayerDir.z,
                    toPlayerDir.y,
                    toPlayerDir.z - spread * toPlayerDir.x});
                ProjectileSystem::spawn(projectiles, bossEye,
                    dir, 16.0f, bossDmg * 0.6f, 0.10f, 3.0f, false);
            }
            static const char* kFrostLines[] = {
                "Feel the cold of the tomb!",
                "Frost shall still your beating heart!",
                "Shiver, mortal, as the dead do not.",
            };
            e.speechText  = kFrostLines[std::rand() % 3];
            e.speechTimer = 2.5f;
        } break;

        // Floor 20: Malachar — Chain Lightning (5 spark bolts in rapid fan).
        // After the seal breaks (ENRAGED) he casts it nearly twice as fast.
        case 20: {
            e.flybyTimer = (e.bossPhase == BossPhase::ENRAGED) ? 2.0f : 4.0f;
            for (s32 s = -2; s <= 2; s++) {
                f32 spread = s * 0.12f;
                Vec3 dir = normalize(Vec3{
                    toPlayerDir.x + spread * toPlayerDir.z,
                    toPlayerDir.y,
                    toPlayerDir.z - spread * toPlayerDir.x});
                ProjectileSystem::spawn(projectiles, bossEye,
                    dir, 14.0f, bossDmg * 0.4f, 0.15f, 3.0f, false,
                    PROJ_SPARK);
            }
            e.speechText = "LIGHTNING!";
            e.speechTimer = 2.0f;
        } break;

        // Floor 25: Spider Queen — spawns 3 spiderling minions
        case 25: {
            e.flybyTimer = 8.0f;
            for (u32 s = 0; s < 3; s++) {
                f32 angle = s * (6.2832f / 3.0f) + e.animTimer;
                Vec3 spawnPos = e.position + Vec3{sinf(angle) * 2.0f, 0, cosf(angle) * 2.0f};
                EntityHandle sh = EntitySystem::spawn(pool, spawnPos,
                    {0.5f, 0.3f, 0.5f}, false,
                    e.maxHealth * 0.15f, 4.5f, 12.0f, 2.0f, 0.8f, bossDmg * 0.3f);
                Entity* spider = handleGet(pool, sh);
                if (spider) {
                    spider->meshId = e.meshId;
                    spider->materialId = e.materialId;
                    spider->enemyType = EnemyType::SPIDER;
                    spider->aiState = AIState::CHASE;
                    spider->level = e.level;
                    spider->baseMoveSpeed = spider->moveSpeed;
                    spider->baseAttackCooldown = spider->attackCooldown;
                }
            }
            e.speechText = "MY CHILDREN!";
            e.speechTimer = 2.5f;
        } break;

        // Floor 30: Baal — Ground Slam (360° AoE burst around self)
        case 30: {
            e.flybyTimer = 5.0f;
            // Damage everything within 6 units of the boss
            if (dist < 6.0f) {
                Combat::applyDamageToPlayer(*targetPlayer, bossDmg * 0.7f, &e.position, static_cast<u16>(i));
            }
            // Also damage nearby friendly NPCs
            for (u32 ni = 0; ni < pool.activeCount; ni++) {
                u32 nIdx = pool.activeList[ni];
                Entity& npc = pool.entities[nIdx];
                if (!(npc.flags & ENT_FRIENDLY)) continue;
                if (npc.flags & ENT_DEAD) continue;
                f32 nDist = length(npc.position - e.position);
                if (nDist < 6.0f) {
                    EntityHandle nh = {static_cast<u16>(nIdx), npc.generation};
                    Combat::applyDamage(pool, nh, bossDmg * 0.5f);
                }
            }
            // Fire 6 shockwave projectiles outward for visual + ranged hit
            for (u32 s = 0; s < 6; s++) {
                f32 angle = s * (6.2832f / 6.0f);
                Vec3 dir = {sinf(angle), 0.0f, cosf(angle)};
                ProjectileSystem::spawn(projectiles, bossEye,
                    dir, 8.0f, bossDmg * 0.3f, 0.2f, 1.5f, false);
            }
            e.speechText = "TREMBLE!";
            e.speechTimer = 2.0f;
        } break;

        // Floor 35: Demon Knight — Charge (sprint + heavy slash)
        case 35: {
            e.flybyTimer = 5.0f;
            // Throw weapon then charge
            u16 pi35 = ProjectileSystem::spawn(projectiles, bossEye,
                toPlayerDir, 20.0f, bossDmg * 0.7f, 0.15f, 3.0f, false);
            if (pi35 != 0xFFFF) projectiles.projectiles[pi35].meshId = e.weaponMeshId;
            // Boost speed temporarily via velocity burst toward player
            Vec3 chargeDir = normalize(Vec3{toPlayerDir.x, 0, toPlayerDir.z});
            e.velocity = chargeDir * e.moveSpeed * 4.0f;
            e.speechText = "CHARGE!";
            e.speechTimer = 2.0f;
        } break;

        // Floor 40: DiaBRO — Fire Storm (dense splash ring + wide inner nova)
        case 40: {
            e.flybyTimer = 2.5f; // frequent nova
            // Inner nova damage — wide radius, hard to dodge up close
            if (dist < 6.0f) {
                Combat::applyDamageToPlayer(*targetPlayer, bossDmg * 0.5f, &e.position, static_cast<u16>(i));
            }
            // Dense ring of 10 fast fire projectiles with splash
            for (u32 s = 0; s < 10; s++) {
                f32 angle = s * (6.2832f / 10.0f);
                Vec3 dir = {sinf(angle), 0.1f, cosf(angle)};
                u16 pi40 = ProjectileSystem::spawn(projectiles, bossEye,
                    dir, 16.0f, bossDmg * 0.4f, 0.15f, 3.0f, false,
                    PROJ_SPLASH);
                if (pi40 != 0xFFFF) {
                    projectiles.projectiles[pi40].splashRadius = 2.5f;
                    projectiles.projectiles[pi40].splashDamage = bossDmg * 0.3f;
                }
            }
            e.speechText = "BURN!";
            e.speechTimer = 2.0f;
        } break;

        // Floor 45: Arch Mage — Arcane Barrage (rapid 5-projectile fan)
        case 45: {
            e.flybyTimer = 2.5f; // fast cooldown
            for (s32 s = -2; s <= 2; s++) {
                f32 spread = s * 0.18f;
                Vec3 dir = normalize(Vec3{
                    toPlayerDir.x + spread * toPlayerDir.z,
                    toPlayerDir.y,
                    toPlayerDir.z - spread * toPlayerDir.x});
                ProjectileSystem::spawn(projectiles, bossEye,
                    dir, 20.0f, bossDmg * 0.45f, 0.10f, 3.0f, false);
            }
            e.speechText = "ARCANE FURY!";
            e.speechTimer = 2.0f;
        } break;

        // Floor 50: Grim Reaper — Death Nova (360° burst) + skeleton summons
        case 50: {
            e.flybyTimer = 5.0f;
            // Death nova — damage everything within 8 units
            if (dist < 8.0f) {
                Combat::applyDamageToPlayer(*targetPlayer, bossDmg * 0.6f, &e.position, static_cast<u16>(i));
            }
            for (u32 ni = 0; ni < pool.activeCount; ni++) {
                u32 nIdx = pool.activeList[ni];
                Entity& npc = pool.entities[nIdx];
                if (!(npc.flags & ENT_FRIENDLY)) continue;
                if (npc.flags & ENT_DEAD) continue;
                f32 nDist = length(npc.position - e.position);
                if (nDist < 8.0f) {
                    EntityHandle nh = {static_cast<u16>(nIdx), npc.generation};
                    Combat::applyDamage(pool, nh, bossDmg * 0.4f);
                }
            }
            // 10 projectiles in a death ring
            for (u32 s = 0; s < 10; s++) {
                f32 angle = s * (6.2832f / 10.0f);
                Vec3 dir = {sinf(angle), 0.0f, cosf(angle)};
                ProjectileSystem::spawn(projectiles, bossEye,
                    dir, 8.0f, bossDmg * 0.3f, 0.15f, 2.0f, false);
            }
            // Summon 2 skeleton minions
            for (u32 s = 0; s < 2; s++) {
                f32 angle = s * 3.14159f + e.animTimer;
                Vec3 spawnPos = e.position + Vec3{sinf(angle) * 3.0f, 0, cosf(angle) * 3.0f};
                EntityHandle sh = EntitySystem::spawn(pool, spawnPos,
                    {0.4f, 0.9f, 0.4f}, false,
                    e.maxHealth * 0.08f, 3.0f, 12.0f, 2.5f, 1.0f, bossDmg * 0.2f);
                Entity* skel = handleGet(pool, sh);
                if (skel) {
                    skel->enemyType = EnemyType::SKELETON;
                    skel->aiState = AIState::CHASE;
                    skel->level = e.level;
                    skel->baseMoveSpeed = skel->moveSpeed;
                    skel->baseAttackCooldown = skel->attackCooldown;
                }
            }
            e.speechText = "DEATH COMES!";
            e.speechTimer = 2.5f;
        } break;

        // Default: basic cleaver throw for any other boss floor
        default: {
            e.flybyTimer = 4.0f;
            ProjectileSystem::spawn(projectiles, bossEye,
                toPlayerDir, 18.0f, bossDmg * 0.8f, 0.15f, 3.0f, false);
            e.speechText = "DIE!";
            e.speechTimer = 2.0f;
        } break;
        }

        e.attackAnimT = 0.4f;
    }

    // Floor-15 lich (Sethrak) secondary ability: raise a guard of 5 skeletons in a
    // ring around himself, on its own cooldown (sprintTimer — unused by a
    // KITER+SUMMONER boss) so it runs independently of the frost fan above.
    if (rawFloor == 15 && bossLOS) {
        e.sprintTimer -= dt;
        if (e.sprintTimer <= 0.0f) {
            e.sprintTimer = 5.0f;
            f32 bossDmg = e.damage;
            for (u32 s = 0; s < 5; s++) {
                f32 ang = s * (6.2832f / 5.0f) + e.animTimer;
                Vec3 pos = e.position + Vec3{sinf(ang) * 2.5f, 0.0f, cosf(ang) * 2.5f};
                EntityHandle sh = EntitySystem::spawn(pool, pos,
                    {0.4f, 0.9f, 0.4f}, false,
                    e.maxHealth * 0.08f, 4.0f, 14.0f, 2.2f, 0.8f, bossDmg * 0.20f);
                Entity* skel = handleGet(pool, sh);
                if (skel) {
                    skel->meshId             = s_skeletonMeshId; // resolved at init
                    skel->materialId         = s_skeletonMatId;
                    skel->enemyType          = EnemyType::SKELETON;
                    skel->aiState            = AIState::CHASE;
                    skel->level              = e.level;
                    skel->baseMoveSpeed      = skel->moveSpeed;
                    skel->baseAttackCooldown = skel->attackCooldown;
                    skel->spawnerIdx         = static_cast<u16>(i); // minion linkage
                }
            }
            static const char* kSummonLines[] = {
                "Arise, my bony legion! Surround this fool!",
                "From dust and grave-dirt, I call your end!",
                "My guards never tire, never bleed, never stop.",
            };
            e.speechText  = kSummonLines[std::rand() % 3];
            e.speechTimer = 3.0f;
        }
    }
}
