// Engine callback wiring — registers all Combat/SkillSystem/ProjectileSystem/Inventory
// event callbacks that require access to the live engine state via s_engine.
// Called by Engine::init() after initAssets() so mesh IDs and defs are valid.

#define SDL_MAIN_HANDLED
#include <SDL.h>
#include "audio/audio.h"

#include "engine/engine.h"
#include "platform/window.h"
#include "platform/clock.h"
#include "platform/input.h"
#include "renderer/gl_context.h"
#include "renderer/renderer.h"
#include "renderer/debug_draw.h"
#include "renderer/hud.h"
#include "renderer/minimap.h"
#include "renderer/font.h"
#include "renderer/item_icons.h"
#include "renderer/material.h"
#include "renderer/obj_loader.h"
#include "renderer/projectile_renderer.h"
#include "world/level_gen.h"
#include "world/level_mesh.h"
#include "world/level_loader.h"
#include "world/collision.h"
#include "world/combat_query.h"
#include "game/player.h"
#include "game/combat.h"
#include "game/enemy_ai.h"
#include "game/squad.h"
#include "game/limb_system.h"
#include "game/projectile.h"
#include "game/item.h"
#include "game/skill.h"
#include "game/inventory_ui.h"
#include "game/game_constants.h"
#include "game/boss_def.h"
#include "game/boss_ai.h"
#include "game/boss_loader.h"
#include "game/enemy_loader.h"
#include "net/net.h"
#include "net/server.h"
#include "net/client.h"
#include "net/snapshot.h"
#include "net/packet.h"
#include "core/log.h"
#include "core/math.h"
#include "core/frame_allocator.h"
#include "core/allocation_tracker.h"
#include "core/profiler.h"

#include <glad/glad.h>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>

// Shared statics defined in engine.cpp
extern Engine* s_engine;
extern FrameAllocator s_frameAllocator;
extern bool s_firstKillDropGiven;

void Engine::initCallbacks() {
    // Wire particle pool and screen shake into combat, skill, and projectile systems
    Combat::setFXTargets(&m_particles, &m_camera.shake);
    SkillSystem::setFXTargets(&m_particles, &m_camera.shake);
    extern void ProjectileSystem_setTrailPool(ParticlePool* pool);
    ProjectileSystem_setTrailPool(&m_particles);

    // Apply energy_flat affix bonus to SkillState.maxEnergy on equip/unequip
    Inventory::setStatsChangedCallback([](PlayerInventory& inv) {
        if (!s_engine) return;
        SkillState& ss = s_engine->m_skillStates[s_engine->m_localPlayerIndex];
        f32 baseEnergy = kClassDefs[static_cast<u32>(s_engine->m_playerClass)].baseEnergy;
        f32 oldMax = ss.maxEnergy;
        ss.maxEnergy = baseEnergy + inv.bonusEnergyFlat;
        // Preserve energy percentage so equipping doesn't drain/fill
        if (oldMax > 0.0f) ss.energy = ss.energy * (ss.maxEnergy / oldMax);
        if (ss.energy > ss.maxEnergy) ss.energy = ss.maxEnergy;
    });

    // Thread isCrit and isKill into the damage number renderer.
    // Killing blows reuse the crit visual style (larger, brighter number).
    Combat::setDamageNumberCallback([](Vec3 pos, f32 amount, bool isCrit, bool isKill) {
        if (!s_engine) return;
        s_engine->spawnDamageNumber(pos, amount, /*isHeal*/false, isCrit || isKill);
    });

    Combat::setDeathCallback([](EntityPool& pool, u16 entityIndex, Vec3 position) {
        if (!s_engine) return;

        // Remove from squad so roles are reassigned before next AI tick
        SquadSystem::onMemberDeath(s_engine->m_level.squads, entityIndex, pool);

        // Friendly NPC death speech — set before loot drop so it's visible
        if (pool.entities[entityIndex].flags & ENT_FRIENDLY) {
            pool.entities[entityIndex].speechText = "Avenge... me...";
            pool.entities[entityIndex].speechTimer = 4.0f;
        }

        // Shadow Dance: extend by 0.3s on each kill
        if (!(pool.entities[entityIndex].flags & ENT_FRIENDLY) &&
            s_engine->m_localPlayer.shadowDanceTimer > 0.0f) {
            s_engine->m_localPlayer.shadowDanceTimer += 0.3f;
            s_engine->m_localPlayer.smokeTimer = s_engine->m_localPlayer.shadowDanceTimer;
        }

        // Wanderer: killing a marked enemy grants speed stack + resets Exploit Weakness cooldown
        if (s_engine->m_playerClass == PlayerClass::WANDERER &&
            pool.entities[entityIndex].markPreyTimer > 0.0f) {
            Player& wp = s_engine->m_localPlayer;
            // +5% speed stack (3s, non-refreshing)
            if (wp.markSpeedStacks < 20) {
                wp.markSpeedTimers[wp.markSpeedStacks] = 3.0f;
                wp.markSpeedStacks++;
            }
            // Reset Exploit Weakness cooldown so it can be recast immediately
            for (u8 cs = 0; cs < 4; cs++) {
                if (s_engine->m_classSkillStates[cs].activeSkill == SkillId::EXPLOIT_WEAKNESS) {
                    s_engine->m_classSkillStates[cs].cooldownTimer = 0.0f;
                    break;
                }
            }
        }

        // Wanderer Exploit Weakness upgrade (floor >= 20): mark spreads to nearest alive enemy on kill
        if (s_engine->m_playerClass == PlayerClass::WANDERER &&
            s_engine->m_level.currentFloor >= 20 &&
            pool.entities[entityIndex].markPreyTimer > 0.0f) {
            constexpr f32 SPREAD_RANGE_SQ = 5.0f * 5.0f;
            for (u32 si = 0; si < pool.activeCount; si++) {
                u32 sIdx = pool.activeList[si];
                if (sIdx == entityIndex) continue;
                Entity& se = pool.entities[sIdx];
                if (!(se.flags & ENT_ACTIVE) || (se.flags & ENT_DEAD)) continue;
                if (se.flags & ENT_FRIENDLY) continue;
                if (se.markPreyTimer > 0.0f) continue; // already marked
                f32 dSq = lengthSq(se.position - position);
                if (dSq < SPREAD_RANGE_SQ) {
                    se.markPreyDmgMult = 1.6f;
                    se.markPreyTimer = 5.0f;
                    s_engine->m_localPlayer.markTimer = 5.0f;
                    break; // spread to one nearest
                }
            }
        }

        // Mark Prey chain clear: if marked enemy dies, arrows rain on nearby enemies
        if (pool.entities[entityIndex].markPreyTimer > 0.0f &&
            !(pool.entities[entityIndex].flags & ENT_FRIENDLY)) {
            f32 chainDmg = s_engine->m_localPlayer.moveSpeed > 0.0f  // use weapon damage if available
                ? 15.0f * (1.0f + (s_engine->m_level.currentFloor + s_engine->m_difficulty * 50 - 1) * 0.06f)
                : 15.0f;
            EntityHandle nearby[MAX_ENTITIES];
            f32 nearDists[MAX_ENTITIES];
            u32 nearCnt = CombatQuery::queryConeSorted(
                pool, position, {0, -1, 0}, -1.0f, 6.0f, nearby, nearDists, MAX_ENTITIES);
            for (u32 nc = 0; nc < nearCnt; nc++) {
                if (nearby[nc].index == entityIndex) continue;
                Entity* ne = handleGet(pool, nearby[nc]);
                if (!ne || (ne->flags & ENT_FRIENDLY) || (ne->flags & ENT_DEAD)) continue;
                // Spawn arrow impacts as PendingMeteors
                for (u32 arr = 0; arr < 5; arr++) {
                    extern PendingMeteor s_meteors[MAX_PENDING_METEORS];
                    for (u32 m = 0; m < MAX_PENDING_METEORS; m++) {
                        if (!s_meteors[m].active) {
                            f32 ofs = (std::rand() / static_cast<f32>(RAND_MAX)) * 0.5f;
                            s_meteors[m].position = ne->position + Vec3{ofs - 0.25f, 0, ofs - 0.25f};
                            s_meteors[m].damage   = chainDmg;
                            s_meteors[m].radius   = 0.8f;
                            s_meteors[m].timer    = 0.1f + arr * 0.05f;
                            s_meteors[m].active   = true;
                            s_meteors[m].healsPlayer = false;
                            s_meteors[m].color    = {0.6f, 0.4f, 0.1f};
                            break;
                        }
                    }
                }
            }
        }

        // Bomber death explosion — AoE damage + burn in radius on death
        if ((pool.entities[entityIndex].enemyRole & EnemyRole::BOMBER) &&
            !(pool.entities[entityIndex].flags & ENT_FRIENDLY)) {
            f32 explosionRadius = 3.0f;
            f32 explosionDmg = pool.entities[entityIndex].damage * 0.8f;
            f32 burnDur = pool.entities[entityIndex].onHitDuration;
            f32 burnDps = pool.entities[entityIndex].onHitDps;

            // Damage player if in radius
            Vec3 playerDelta = s_engine->m_localPlayer.position - position;
            f32 playerDist = sqrtf(playerDelta.x * playerDelta.x + playerDelta.z * playerDelta.z);
            if (playerDist < explosionRadius) {
                Combat::applyDamageToPlayer(s_engine->m_localPlayer, explosionDmg, &position);
                if (burnDur > 0.0f) {
                    s_engine->m_localPlayer.burnTimer = fmaxf(s_engine->m_localPlayer.burnTimer, burnDur);
                    s_engine->m_localPlayer.burnDps = burnDps;
                }
            }
            // Damage friendly NPCs in radius
            for (u32 ni = 0; ni < pool.activeCount; ni++) {
                u32 nIdx = pool.activeList[ni];
                Entity& npc = pool.entities[nIdx];
                if (nIdx == entityIndex) continue;
                if (!(npc.flags & ENT_FRIENDLY)) continue;
                if (npc.flags & ENT_DEAD) continue;
                f32 nDist = length(npc.position - position);
                if (nDist < explosionRadius) {
                    EntityHandle nh = {static_cast<u16>(nIdx), npc.generation};
                    Combat::applyDamage(pool, nh, explosionDmg);
                }
            }
            // Visual: spawn explosion particles
            ParticleSystem::spawnExplosion(s_engine->m_particles, position, explosionRadius * 0.5f);
        }

        // Track hostile kills for floor transition screen
        if (!(pool.entities[entityIndex].flags & ENT_FRIENDLY)) {
            s_engine->m_transition.floorKillCount++;
            AudioSystem::playAt(SfxId::ENEMY_DEATH, position, s_engine->m_localPlayer.position);
        }

        // Floors 1-3: first hostile kill guarantees a magic (green) quality drop
        if (!s_firstKillDropGiven && s_engine->m_level.currentFloor <= 3 &&
            !(pool.entities[entityIndex].flags & ENT_FRIENDLY)) {
            s_firstKillDropGiven = true;
            u8 lvl = pool.entities[entityIndex].level;
            if (lvl < 1) lvl = 1;
            // Floor 1: force armor drops so tutorial teaches equipping gear
            ItemInstance item;
            for (u32 attempt = 0; attempt < 20; attempt++) {
                item = ItemGen::rollItem(lvl, s_engine->m_itemDefs,
                                         s_engine->m_itemDefCount,
                                         s_engine->m_affixDefs,
                                         s_engine->m_affixDefCount);
                if (isItemEmpty(item)) break;
                if (s_engine->m_level.currentFloor > 1) break; // no restriction above floor 1
                // On floor 1, only allow shields so the tutorial teaches blocking
                ItemSlot slot = s_engine->m_itemDefs[item.defId].slot;
                if (slot == ItemSlot::OFFHAND) break;
            }
            if (!isItemEmpty(item)) {
                // Force to at least MAGIC rarity
                if (item.rarity < Rarity::MAGIC) item.rarity = Rarity::MAGIC;
                // Re-roll affixes for magic quality (1-2 affixes)
                if (item.affixCount == 0) {
                    const ItemDef& idef = s_engine->m_itemDefs[item.defId];
                    ItemGen::rollAffixes(item, lvl, idef.slot,
                                          s_engine->m_affixDefs, s_engine->m_affixDefCount,
                                          idef.weaponType);
                }
                WorldItemSystem::spawn(s_engine->m_worldItems, item,
                                       position + Vec3{0, 0.5f, 0}, &s_engine->m_level.grid);
            }
            return; // skip normal drop logic for this kill
        }

        // Boss guaranteed loot — mini-bosses drop rare+, major bosses drop legendary
        if (pool.entities[entityIndex].bossDefIdx != 0xFF &&
            pool.entities[entityIndex].bossDefIdx < s_engine->m_bossDefs.count) {
            const BossDef& bd = s_engine->m_bossDefs.defs[pool.entities[entityIndex].bossDefIdx];
            u8 bossLvl = pool.entities[entityIndex].level;
            if (bossLvl < 1) bossLvl = 1;

            // Guaranteed quality drop — re-roll until we get a true legendary
            // (an item whose definition supports legendary rarity + has a skill)
            Rarity minRarity = static_cast<Rarity>(bd.lootGuarantee);
            ItemInstance bossItem;
            for (u32 attempt = 0; attempt < 50; attempt++) {
                bossItem = ItemGen::rollItem(bossLvl, s_engine->m_itemDefs,
                                              s_engine->m_itemDefCount,
                                              s_engine->m_affixDefs,
                                              s_engine->m_affixDefCount);
                if (isItemEmpty(bossItem)) break;
                // Accept if the item's definition can actually be this rarity
                if (s_engine->m_itemDefs[bossItem.defId].maxRarity >= minRarity) break;
            }
            if (!isItemEmpty(bossItem)) {
                if (bossItem.rarity < minRarity) {
                    bossItem.rarity = minRarity;
                    // Re-roll affixes for the upgraded rarity with correct weapon type
                    bossItem.affixCount = 0;
                    const ItemDef& biDef = s_engine->m_itemDefs[bossItem.defId];
                    ItemGen::rollAffixes(bossItem, bossLvl, biDef.slot,
                                          s_engine->m_affixDefs, s_engine->m_affixDefCount,
                                          biDef.weaponType);
                }
                WorldItemSystem::spawn(s_engine->m_worldItems, bossItem,
                                       position + Vec3{0, 0.5f, 0}, &s_engine->m_level.grid);
            }

            // Bonus drops for major bosses
            for (u8 bd_i = 0; bd_i < bd.bonusDrops; bd_i++) {
                ItemInstance bonus = ItemGen::rollItem(bossLvl, s_engine->m_itemDefs,
                                                       s_engine->m_itemDefCount,
                                                       s_engine->m_affixDefs,
                                                       s_engine->m_affixDefCount);
                if (!isItemEmpty(bonus)) {
                    Vec3 offset = {(f32)(bd_i) * 0.3f - 0.15f, 0.5f, 0.2f};
                    WorldItemSystem::spawn(s_engine->m_worldItems, bonus,
                                           position + offset, &s_engine->m_level.grid);
                }
            }

            // Always drop a globe from bosses
            ItemInstance globe;
            globe.defId = GLOBE_HEALTH_ID;
            globe.uid   = s_engine->m_worldItems.nextUid++;
            WorldItemSystem::spawn(s_engine->m_worldItems, globe,
                                   position + Vec3{0.2f, 0.5f, 0.0f}, &s_engine->m_level.grid);
            return; // skip normal loot path
        }

        // Hostile enemies only drop loot; chance scales with floor depth
        u8 enemyLevel = pool.entities[entityIndex].level;
        f32 dropChance = GameConst::LOOT_DROP_CHANCE + enemyLevel * 0.01f;
        if (dropChance > 0.70f) dropChance = 0.70f;
        if (!(pool.entities[entityIndex].flags & ENT_FRIENDLY) &&
            (std::rand() % 100) < static_cast<int>(dropChance * 100.0f)) {
            if (enemyLevel < 1) enemyLevel = 1;
            ItemInstance item;
            // Floor 1: force shield drops so the tutorial teaches blocking
            for (u32 attempt = 0; attempt < 20; attempt++) {
                item = ItemGen::rollItem(enemyLevel, s_engine->m_itemDefs,
                                         s_engine->m_itemDefCount,
                                         s_engine->m_affixDefs,
                                         s_engine->m_affixDefCount);
                if (isItemEmpty(item)) break;
                if (s_engine->m_level.currentFloor > 1) break;
                ItemSlot slot = s_engine->m_itemDefs[item.defId].slot;
                if (slot == ItemSlot::OFFHAND) break;
            }
            if (!isItemEmpty(item)) {
                WorldItemSystem::spawn(s_engine->m_worldItems, item,
                                       position + Vec3{0, 0.5f, 0}, &s_engine->m_level.grid);
            }

            // Chance to drop a globe (restores both HP and energy on pickup)
            if ((std::rand() % 100) < static_cast<int>(GameConst::GLOBE_DROP_CHANCE * 100.0f)) {
                ItemInstance globe;
                globe.defId = GLOBE_HEALTH_ID; // single globe type
                globe.uid   = s_engine->m_worldItems.nextUid++;
                WorldItemSystem::spawn(s_engine->m_worldItems, globe,
                                       position + Vec3{0.2f, 0.5f, 0.0f});
            }
        }

        // --- Ring on-kill passives ---
        if (s_engine->m_ringPassive != SkillId::NONE && !(pool.entities[entityIndex].flags & ENT_FRIENDLY)) {
            // Soul Harvest: +5% speed, +3% damage per stack for 10s (max 5)
            if (s_engine->m_ringPassive == SkillId::SOUL_HARVEST) {
                Player& p = s_engine->m_localPlayer;
                if (p.soulHarvestStacks < 5) p.soulHarvestStacks++;
                p.soulHarvestTimer = 5.0f; // 5s window to get next kill or stacks reset
                // Speed bonus applied via moveSpeed multiplier
            }
            // Phase Strike (Shadow Ring): 20% on kill, drop smoke bomb — 0.5s stealth + aggro reset
            if (s_engine->m_ringPassive == SkillId::PHASE_STRIKE && (std::rand() % 100) < 20) {
                Player& p = s_engine->m_localPlayer;
                p.smokeTimer = 0.5f;
                // Reset aggro on all enemies within 6m
                for (u32 si = 0; si < MAX_ENTITIES; si++) {
                    Entity& se = pool.entities[si];
                    if (!(se.flags & ENT_ACTIVE) || (se.flags & ENT_DEAD)) continue;
                    if (se.flags & ENT_FRIENDLY) continue;
                    Vec3 diff = se.position - position;
                    if (diff.x * diff.x + diff.z * diff.z < 6.0f * 6.0f) {
                        se.aiState = AIState::IDLE;
                        se.velocity = {0, 0, 0};
                    }
                }
                // Dark grey smoke nova at kill position
                for (u32 ni = 0; ni < Engine::MAX_NOVA_FX; ni++) {
                    if (!s_engine->m_fx.novaFX[ni].active) {
                        s_engine->m_fx.novaFX[ni] = {position, 4.0f, 1.2f, true, {0.3f, 0.3f, 0.35f}};
                        break;
                    }
                }
            }
            // Void Kill: 15% chance to spawn void zone on corpse
            if (s_engine->m_ringPassive == SkillId::VOID_KILL && (std::rand() % 100) < 15) {
                // AoE void damage to nearby enemies
                EntityHandle aoeHits[MAX_ENTITIES];
                f32 aoeDists[MAX_ENTITIES];
                u32 aoeCount = CombatQuery::queryConeSorted(
                    pool, position, {0,-1,0}, -1.0f, 3.0f,
                    aoeHits, aoeDists, MAX_ENTITIES);
                for (u32 h = 0; h < aoeCount; h++) {
                    Entity* ve = handleGet(pool, aoeHits[h]);
                    if (!ve || (ve->flags & ENT_DEAD) || (ve->flags & ENT_FRIENDLY)) continue;
                    f32 missingHp = ve->maxHealth - ve->health;
                    Combat::applyDamage(pool, aoeHits[h], 10.0f + missingHp * 0.6f);
                }
                // Dark purple nova visual
                for (u32 ni = 0; ni < Engine::MAX_NOVA_FX; ni++) {
                    if (!s_engine->m_fx.novaFX[ni].active) {
                        s_engine->m_fx.novaFX[ni] = {position, 3.0f, 1.0f, true, {0.4f, 0.1f, 0.6f}};
                        break;
                    }
                }
            }
        }

        (void)position;
    });

    // Splash effect callback — spawns fire VFX at impact point
    ProjectileSystem::setSplashCallback([](Vec3 position, f32 radius) {
        if (!s_engine) return;
        // Snap fire effect to floor level so it doesn't render underground
        u32 gx, gz;
        Vec3 fxPos = position;
        if (LevelGridSystem::worldToGrid(s_engine->m_level.grid, position, gx, gz) &&
            !LevelGridSystem::isSolid(s_engine->m_level.grid, gx, gz)) {
            fxPos.y = LevelGridSystem::getFloorHeight(s_engine->m_level.grid, gx, gz) + 0.1f;
        }
        for (u32 i = 0; i < Engine::MAX_FIRE_FX; i++) {
            if (!s_engine->m_fx.fireFX[i].active) {
                s_engine->m_fx.fireFX[i] = {fxPos, radius, 1.0f, true};
                break;
            }
        }
        // Fireball splash particles — fiery burst at impact point
        ParticleSystem::spawnExplosion(s_engine->m_particles, position, radius);
        s_engine->m_camera.shake.trigger(0.06f, 0.3f);
    });

    // Projectile hit callback — triggers weapon on-hit procs for projectile weapons
    ProjectileSystem::setHitCallback([](Vec3 position, EntityHandle target) {
        if (!s_engine) return;
        if (s_engine->m_weaponProc == SkillId::NONE) return;

        u32 procRoll = static_cast<u32>(std::rand()) % 100;
        u32 procChance = 20;
        if (s_engine->m_weaponProc == SkillId::VOID_ZONE)       procChance = 5;
        if (s_engine->m_weaponProc == SkillId::FROZEN_ORB)      procChance = 15;
        if (s_engine->m_weaponProc == SkillId::CHAIN_LIGHTNING)  procChance = 25;
        if (s_engine->m_weaponProc == SkillId::METEOR_STRIKE)    procChance = 10;
        if (s_engine->m_weaponProc == SkillId::BLOOD_NOVA)       procChance = 20;
        if (s_engine->m_weaponProc == SkillId::SHADOW_RICOCHET)  procChance = 30;

        LOG_INFO("Projectile hit: weaponProc=%u, roll=%u/%u",
                 static_cast<u32>(s_engine->m_weaponProc), procRoll, procChance);

        if (procRoll >= procChance) return;

        const SkillDef* sd = SkillSystem::findSkillDef(s_engine->m_skillDefs,
                                                         s_engine->m_skillDefCount,
                                                         s_engine->m_weaponProc);
        if (!sd) { LOG_WARN("  Proc skill def not found!"); return; }

        LOG_INFO("WEAPON PROC triggered! skill=%u at (%.1f, %.1f, %.1f)",
                 static_cast<u32>(s_engine->m_weaponProc), position.x, position.y, position.z);

        switch (s_engine->m_weaponProc) {
            case SkillId::VOID_ZONE: {
                // AoE void zone — hits all enemies in radius with flat + 60% missing HP
                EntityHandle aoeHits[MAX_ENTITIES];
                f32 aoeDists[MAX_ENTITIES];
                u32 aoeCount = CombatQuery::queryConeSorted(
                    s_engine->m_entities, position, {0,-1,0}, -1.0f, sd->radius,
                    aoeHits, aoeDists, MAX_ENTITIES);
                for (u32 h = 0; h < aoeCount; h++) {
                    Entity* ve = handleGet(s_engine->m_entities, aoeHits[h]);
                    if (!ve || (ve->flags & ENT_DEAD) || (ve->flags & ENT_FRIENDLY)) continue;
                    f32 missingHp = ve->maxHealth - ve->health;
                    f32 voidDmg = sd->damage + missingHp * 0.6f;
                    Combat::applyDamage(s_engine->m_entities, aoeHits[h], voidDmg);
                }
                LOG_INFO("  VOID ZONE AoE: hit %u enemies", aoeCount);
                // Large dark-purple nova + scorch zone for visibility
                for (u32 ni = 0; ni < Engine::MAX_NOVA_FX; ni++) {
                    if (!s_engine->m_fx.novaFX[ni].active) {
                        s_engine->m_fx.novaFX[ni] = {position, 3.0f, 1.2f, true, {0.4f, 0.1f, 0.6f}};
                        break;
                    }
                }
                // Also spawn a dark scorch zone on the ground
                for (u32 si = 0; si < Engine::MAX_SCORCH; si++) {
                    if (!s_engine->m_fx.scorchZones[si].active) {
                        s_engine->m_fx.scorchZones[si] = {position, 3.0f, 1.5f, 0.0f, true};
                        break;
                    }
                }
            } break;
            case SkillId::FROZEN_ORB: {
                Vec3 dir = s_engine->m_localPlayer.forward;
                u16 orbIdx = ProjectileSystem::spawn(s_engine->m_projectiles, position, dir,
                    sd->projectileSpeed, sd->damage, sd->radius, sd->duration, true);
                if (orbIdx != 0xFFFF) s_engine->m_projectiles.projectiles[orbIdx].projFlags = PROJ_ORB;
            } break;
            case SkillId::CHAIN_LIGHTNING: {
                SkillState tempSS;
                tempSS.activeSkill = SkillId::CHAIN_LIGHTNING;
                tempSS.energy = 999.0f; tempSS.maxEnergy = 999.0f;
                SkillSystem::tryActivate(tempSS, s_engine->m_skillDefs, s_engine->m_skillDefCount,
                    position, s_engine->m_localPlayer.forward, 0,
                    s_engine->m_projectiles, s_engine->m_entities,
                    s_engine->m_level.grid, s_engine->m_localPlayer);
            } break;
            case SkillId::BLOOD_NOVA: {
                EntityHandle hits[MAX_ENTITIES];
                f32 dists[MAX_ENTITIES];
                u32 hitCount = CombatQuery::queryConeSorted(
                    s_engine->m_entities, position, {0,0,-1}, -1.0f, sd->radius,
                    hits, dists, MAX_ENTITIES);
                for (u32 h = 0; h < hitCount; h++)
                    Combat::applyDamage(s_engine->m_entities, hits[h], sd->damage * 0.5f);
                for (u32 ni = 0; ni < Engine::MAX_NOVA_FX; ni++) {
                    if (!s_engine->m_fx.novaFX[ni].active) {
                        s_engine->m_fx.novaFX[ni] = {position, sd->radius, 0.6f, true, {1.0f, 0.15f, 0.1f}};
                        break;
                    }
                }
            } break;
            case SkillId::METEOR_STRIKE: {
                extern PendingMeteor s_meteors[MAX_PENDING_METEORS];
                for (u32 mi = 0; mi < MAX_PENDING_METEORS; mi++) {
                    if (!s_meteors[mi].active) {
                        s_meteors[mi] = {position, sd->damage, sd->radius, sd->delay, true};
                        break;
                    }
                }
            } break;
            case SkillId::SHADOW_RICOCHET: {
                // Find 2 nearest enemies (excluding the one we just hit) and fire
                // shadow bolts at them. Each bolt is a normal player projectile so
                // it can re-trigger the proc on hit (10% chance → natural decay).
                EntityHandle nearby[8];
                f32 nearDists[8];
                u32 found = CombatQuery::queryConeSorted(
                    s_engine->m_entities, position, {0,-1,0}, -1.0f, 12.0f,
                    nearby, nearDists, 8);
                u32 spawned = 0;
                for (u32 h = 0; h < found && spawned < 2; h++) {
                    if (nearby[h].index == target.index) continue; // skip primary target
                    Entity* ne = handleGet(s_engine->m_entities, nearby[h]);
                    if (!ne || (ne->flags & ENT_DEAD) || (ne->flags & ENT_FRIENDLY)) continue;
                    Vec3 toEnemy = ne->position - position;
                    f32 dist = length(toEnemy);
                    if (dist < 0.1f) continue;
                    Vec3 dir = toEnemy * (1.0f / dist);
                    // Shadow bolt: 60% of weapon damage, fast, small radius
                    f32 boltDmg = sd->damage * 0.6f;
                    u16 idx = ProjectileSystem::spawn(s_engine->m_projectiles, position, dir,
                        20.0f, boltDmg, 0.1f, 2.0f, true);
                    if (idx != 0xFFFF) {
                        s_engine->m_projectiles.projectiles[idx].projFlags = PROJ_VOID;
                    }
                    spawned++;
                }
                LOG_INFO("  SHADOW RICOCHET: spawned %u bolts", spawned);
            } break;
            default: break;
        }
    });

    // Floating damage numbers for projectile hits
    ProjectileSystem::setDamageNumberCallback([](Vec3 position, f32 damage) {
        if (!s_engine) return;
        s_engine->spawnDamageNumber(position, damage);
    });

    // Perfect block callback — legendary shield stun bash
    Combat::setPerfectBlockCallback([](Player& player) {
        if (!s_engine) return;

        // Check if offhand is a legendary shield
        const ItemInstance& shield = s_engine->m_inventories[s_engine->m_localPlayerIndex].equipped[static_cast<u32>(ItemSlot::OFFHAND)];
        bool hasLegendaryShield = !isItemEmpty(shield) && shield.rarity == Rarity::LEGENDARY;

        if (hasLegendaryShield) {
            for (u32 a = 0; a < s_engine->m_entities.activeCount; a++) {
                u32 idx = s_engine->m_entities.activeList[a];
                Entity& ent = s_engine->m_entities.entities[idx];
                if (ent.flags & ENT_DEAD) continue;
                if (ent.flags & ENT_FRIENDLY) continue;
                if (ent.enemyType == EnemyType::PROP) continue;
                f32 dist = length(ent.position - player.position);
                if (dist < 3.0f) {
                    ent.freezeTimer = 1.0f;
                }
            }
            // Visual nova feedback
            for (u32 ni = 0; ni < MAX_NOVA_FX; ni++) {
                if (!s_engine->m_fx.novaFX[ni].active) {
                    s_engine->m_fx.novaFX[ni] = {player.position, 3.0f, 0.4f, true, Vec3{0.8f, 0.8f, 1.0f}};
                    break;
                }
            }
        }
    });

    // Dodge-through callback: fires an automatic riposte counter-hit when the player
    // dodges through an incoming attack, and grants an adrenaline stack if unlocked.
    Combat::setDodgeThroughCallback([](u16 attackerIdx, Vec3 attackerPos) {
        if (!s_engine) return;
        Player& player = s_engine->m_localPlayer;

        // 1. Instant riposte: 50% weapon damage counter-hit on the attacker.
        // m_weaponDefs[0] is the fallback if no item is equipped (unarmed).
        WeaponDef effectiveWeapon = Inventory::getEffectiveWeapon(
            s_engine->m_inventories[s_engine->m_localPlayerIndex],
            s_engine->m_itemDefs, s_engine->m_weaponDefs[0]);
        f32 riposteDmg = effectiveWeapon.damage * 0.5f;

        if (attackerIdx < MAX_ENTITIES &&
            (s_engine->m_entities.entities[attackerIdx].flags & ENT_ACTIVE)) {
            EntityHandle h;
            h.index      = attackerIdx;
            h.generation = s_engine->m_entities.entities[attackerIdx].generation;
            Combat::applyDamage(s_engine->m_entities, h, riposteDmg, &player.position);
            Combat::spawnDamageNumber(attackerPos, riposteDmg);
        }

        // 2. Adrenaline surge stacks (only if skill 3 is unlocked)
        if (player.adrenalineUnlocked) {
            DodgeState& ds = player.dodgeState;
            if (ds.counterStacks < 5) {
                ds.counterTimers[ds.counterStacks] = 4.0f;
                ds.counterStacks++;
            } else {
                // Refresh the oldest (shortest remaining) stack timer rather than losing a stack
                f32 minT  = ds.counterTimers[0];
                u8  minIdx = 0;
                for (u8 i = 1; i < 5; i++) {
                    if (ds.counterTimers[i] < minT) { minT = ds.counterTimers[i]; minIdx = i; }
                }
                ds.counterTimers[minIdx] = 4.0f;
            }
        }

        // 3. Exploit Weakness: dodge-through a marked enemy grants +5% speed stack (3s)
        if (attackerIdx < MAX_ENTITIES) {
            Entity& attacker = s_engine->m_entities.entities[attackerIdx];
            if (attacker.markPreyTimer > 0.0f && player.markSpeedStacks < 20) {
                player.markSpeedTimers[player.markSpeedStacks] = 3.0f;
                player.markSpeedStacks++;
            }
        }

        // 4. Death's Dance AoE slash: if the ultimate is active, radiate a full-circle
        //    slash to all nearby enemies on every dodge-through
        if (player.deathsDanceTimer > 0.0f) {
            WeaponDef ew = Inventory::getEffectiveWeapon(
                s_engine->m_inventories[s_engine->m_localPlayerIndex],
                s_engine->m_itemDefs, s_engine->m_weaponDefs[0]);
            f32 slashDmg = ew.damage;
            Vec3 eyePos  = player.position + Vec3{0, player.eyeHeight, 0};

            EntityHandle hits[MAX_ENTITIES];
            f32 dists[MAX_ENTITIES];
            // cosCone = -1.0f → cos(180°) → query everything in a sphere of radius 3 m
            u32 hitCount = CombatQuery::queryConeSorted(
                s_engine->m_entities, eyePos, player.forward, -1.0f, 3.0f,
                hits, dists, MAX_ENTITIES);
            f32 totalDmg = 0.0f;
            for (u32 k = 0; k < hitCount; k++) {
                Entity* he = handleGet(s_engine->m_entities, hits[k]);
                if (he && !(he->flags & ENT_DEAD)) {
                    // Accumulate actual damage dealt (capped by remaining HP before hit)
                    f32 hpBefore = he->health;
                    Combat::applyDamage(s_engine->m_entities, hits[k], slashDmg, &eyePos);
                    totalDmg += hpBefore - (he->health < 0.0f ? 0.0f : he->health);
                }
            }
            // Death's Dance upgrade: heal 10% of AoE damage dealt (floor >= 40)
            if (s_engine->m_level.currentFloor >= 40 && totalDmg > 0.0f) {
                player.health += totalDmg * 0.1f;
                if (player.health > player.maxHealth) player.health = player.maxHealth;
            }
        }

        // 4. Subtle screen-shake for tactile riposte feedback
        if (s_engine->m_camera.shake.intensity < 0.03f) {
            s_engine->m_camera.shake.trigger(0.03f, 0.2f);
        }
    });

    // Drone/turret spawn callback — skill.cpp delegates spawning here so we
    // have direct access to the mesh registry.  type: 0=combat drone, 1=swarm, 2=turret
    SkillSystem::setDroneSpawnCallback([](Vec3 position, u8 type) {
        if (!s_engine) return;
        EntityPool& pool = s_engine->m_entities;

        f32 floorMult = 1.0f + (s_engine->m_level.currentFloor + s_engine->m_difficulty * 50 - 1) * 0.06f;

        if (type == 0) {
            // Spider drone — melee ground unit. Swarm Overlord spawns many of these.
            EntityHandle h = EntitySystem::spawn(pool, position,
                {0.3f, 0.2f, 0.3f}, false, 30.0f * floorMult, 6.0f * floorMult,
                12.0f, 4.0f, 0.5f, 8.0f);
            Entity* e = handleGet(pool, h);
            if (e) {
                e->flags        |= ENT_FRIENDLY;
                e->enemyType     = EnemyType::SPIDER;
                e->meshId        = s_engine->m_meshIdSpider;
                e->materialId    = 49; // prop_iron
                e->npcWeaponType = WeaponType::MELEE;
                e->aiState       = AIState::IDLE;
                e->baseMoveSpeed      = e->moveSpeed;
                e->baseAttackCooldown = e->attackCooldown;
            }
        } else if (type == 1) {
            // Bat drone — flying melee swarm unit. Fast, closes distance to attack.
            EntityHandle h = EntitySystem::spawn(pool, position,
                {0.15f, 0.1f, 0.15f}, true, 20.0f * floorMult, 7.0f * floorMult,
                12.0f, 3.0f, 0.5f, 6.0f);
            Entity* e = handleGet(pool, h);
            if (e) {
                e->flags        |= ENT_FRIENDLY | ENT_FLYING;
                e->enemyType     = EnemyType::GENERIC;
                e->meshId        = s_engine->m_meshIdBat;
                e->materialId    = 49; // prop_iron
                e->npcWeaponType = WeaponType::MELEE;
                e->aiState       = AIState::IDLE;
                e->baseMoveSpeed      = e->moveSpeed;
                e->baseAttackCooldown = e->attackCooldown;
            }
        } else if (type == 3) {
            // Swarm Queen — large tanky spider that auto-spawns minis every 2s for 20s
            EntityHandle h = EntitySystem::spawn(pool, position,
                {0.5f, 0.4f, 0.5f}, false, 200.0f * floorMult, 8.0f * floorMult,
                15.0f, 3.0f, 1.0f, 10.0f);
            Entity* e = handleGet(pool, h);
            if (e) {
                e->flags        |= ENT_FRIENDLY;
                e->enemyType     = EnemyType::SPIDER;
                e->meshId        = s_engine->m_meshIdSpider;
                e->materialId    = MaterialSystem::getIdByName("gold_trim");
                if (e->materialId == 0) e->materialId = 49; // fallback
                e->npcWeaponType = WeaponType::MELEE;
                e->aiState       = AIState::IDLE;
                e->queenLifeTimer  = 20.0f;
                e->queenSpawnTimer = 2.0f;
                e->baseMoveSpeed      = e->moveSpeed;
                e->baseAttackCooldown = e->attackCooldown;
                // Scale up visually (halfExtents already larger)
            }
        } else if (type == 2) {
            // Mobile turret bot — armored body on tank treads, follows player when idle.
            // HP scales with floor depth so turrets stay relevant in later tiers.
            f32 baseHp = 160.0f;
            f32 floorMult = 1.0f + (s_engine->m_level.currentFloor - 1) * 0.06f;
            EntityHandle h = EntitySystem::spawn(pool, position,
                {0.2f, 0.3f, 0.2f}, false, baseHp * floorMult, 3.0f, 15.0f, 10.0f, 1.5f, 12.0f);
            Entity* e = handleGet(pool, h);
            if (e) {
                e->flags        |= ENT_FRIENDLY;
                e->enemyType     = EnemyType::GENERIC;
                e->meshId        = s_engine->findMeshByName("turret");
                e->materialId    = MaterialSystem::getIdByName("turret_skin");
                e->npcWeaponType = WeaponType::PROJECTILE;
                e->npcProjectileSpeed  = 30.0f;
                e->npcProjectileRadius = 0.06f;
                e->aiState       = AIState::IDLE;
                e->baseMoveSpeed      = e->moveSpeed;
                e->baseAttackCooldown = e->attackCooldown;
                // Spark burst on deploy — visual feedback
                ParticleSystem::spawnSparks(s_engine->m_particles, position, {0, 1, 0}, 8);
            }
        }
    });
    // Share the same drone spawn path with EnemyAI for Swarm Queen auto-spawning.
    // The SkillSystem callback is a captureless lambda stored as function pointer —
    // EnemyAI needs the same capability so the queen can spawn mini drones.
    EnemyAI::setDroneSpawnCallback([](Vec3 pos, u8 type) {
        if (!s_engine) return;
        // Re-invoke the skill system's drone spawn (same lambda body as above)
        // by calling the SkillSystem callback which is stored as a static.
        // Simplest: just inline the spawn for type 0 (spider mini drone)
        EntityPool& pool = s_engine->m_entities;
        f32 fm = 1.0f + (s_engine->m_level.currentFloor + s_engine->m_difficulty * 50 - 1) * 0.06f;
        EntityHandle h = EntitySystem::spawn(pool, pos,
            {0.3f, 0.2f, 0.3f}, false, 30.0f * fm, 6.0f * fm,
            12.0f, 4.0f, 0.5f, 8.0f);
        Entity* e = handleGet(pool, h);
        if (e) {
            e->flags        |= ENT_FRIENDLY;
            e->enemyType     = EnemyType::SPIDER;
            e->meshId        = s_engine->m_meshIdSpider;
            e->materialId    = 49;
            e->npcWeaponType = WeaponType::MELEE;
            e->aiState       = AIState::IDLE;
            e->baseMoveSpeed      = e->moveSpeed;
            e->baseAttackCooldown = e->attackCooldown;
        }
    });
}
