// engine_death.cpp — Engine death-event handler implementations.
// These are private Engine methods extracted from the Combat death callback in
// engine_init_callbacks.cpp to break up the monolithic lambda.  Each method
// corresponds to one logical phase of the death event and is called in order
// by the lambda; the bool-returning variants signal that they fully handled the
// loot path so the lambda can early-return without running the later phases.
//
// Control-flow contract (mirrors the original lambda exactly):
//   handleDeathPreamble   — always runs (squad, speech, passives, bomber)
//   handleFirstKillDrop   — runs kill tracking then checks first-kill guarantee;
//                           returns true → lambda returns (skip boss/normal/ring)
//   handleBossLootDrop    — returns true → lambda returns (skip normal/ring)
//   handleNormalLootDrop  — void, no early exit
//   handleOnKillRingPassives — void, always last

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

// Runs unconditionally for every death, before any loot logic.
// Handles: squad reassignment, friendly NPC speech, Shadow Dance extension,
// Wanderer mark-prey passives, Mark Prey arrow chain, and Bomber death explosion.
void Engine::handleDeathPreamble(EntityPool& pool, u16 idx, Vec3 pos) {
    // Remove from squad so roles are reassigned before next AI tick
    SquadSystem::onMemberDeath(m_level.squads, idx, pool);

    // Friendly NPC death speech — set before loot drop so it's visible
    if (pool.entities[idx].flags & ENT_FRIENDLY) {
        pool.entities[idx].speechText = "Avenge... me...";
        pool.entities[idx].speechTimer = 4.0f;
    }

    // Shadow Dance: extend by 0.3s on each kill
    if (!(pool.entities[idx].flags & ENT_FRIENDLY) &&
        m_localPlayer.shadowDanceTimer > 0.0f) {
        m_localPlayer.shadowDanceTimer += 0.3f;
        m_localPlayer.smokeTimer = m_localPlayer.shadowDanceTimer;
    }

    // Wanderer: killing a marked enemy grants speed stack + resets Exploit Weakness cooldown
    if (m_playerClass == PlayerClass::WANDERER &&
        pool.entities[idx].markPreyTimer > 0.0f) {
        Player& wp = m_localPlayer;
        // +5% speed stack (3s, non-refreshing)
        if (wp.markSpeedStacks < 20) {
            wp.markSpeedTimers[wp.markSpeedStacks] = 3.0f;
            wp.markSpeedStacks++;
        }
        // Reset Exploit Weakness cooldown so it can be recast immediately
        for (u8 cs = 0; cs < 4; cs++) {
            if (m_classSkillStates[cs].activeSkill == SkillId::EXPLOIT_WEAKNESS) {
                m_classSkillStates[cs].cooldownTimer = 0.0f;
                break;
            }
        }
    }

    // Wanderer Exploit Weakness upgrade (floor >= 20): mark spreads to nearest alive enemy on kill
    if (m_playerClass == PlayerClass::WANDERER &&
        m_level.currentFloor >= 20 &&
        pool.entities[idx].markPreyTimer > 0.0f) {
        constexpr f32 SPREAD_RANGE_SQ = 5.0f * 5.0f;
        for (u32 si = 0; si < pool.activeCount; si++) {
            u32 sIdx = pool.activeList[si];
            if (sIdx == idx) continue;
            Entity& se = pool.entities[sIdx];
            if (!(se.flags & ENT_ACTIVE) || (se.flags & ENT_DEAD)) continue;
            if (se.flags & ENT_FRIENDLY) continue;
            if (se.markPreyTimer > 0.0f) continue; // already marked
            f32 dSq = lengthSq(se.position - pos);
            if (dSq < SPREAD_RANGE_SQ) {
                se.markPreyDmgMult = 1.6f;
                se.markPreyTimer = 5.0f;
                m_localPlayer.markTimer = 5.0f;
                break; // spread to one nearest
            }
        }
    }

    // Mark Prey chain clear: if marked enemy dies, arrows rain on nearby enemies
    if (pool.entities[idx].markPreyTimer > 0.0f &&
        !(pool.entities[idx].flags & ENT_FRIENDLY)) {
        f32 chainDmg = m_localPlayer.moveSpeed > 0.0f  // use weapon damage if available
            ? 15.0f * (1.0f + (m_level.currentFloor + m_difficulty * 50 - 1) * 0.06f)
            : 15.0f;
        EntityHandle nearby[MAX_ENTITIES];
        f32 nearDists[MAX_ENTITIES];
        u32 nearCnt = CombatQuery::queryConeSorted(
            pool, pos, {0, -1, 0}, -1.0f, 6.0f, nearby, nearDists, MAX_ENTITIES);
        for (u32 nc = 0; nc < nearCnt; nc++) {
            if (nearby[nc].index == idx) continue;
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
    if ((pool.entities[idx].enemyRole & EnemyRole::BOMBER) &&
        !(pool.entities[idx].flags & ENT_FRIENDLY)) {
        f32 explosionRadius = 3.0f;
        f32 explosionDmg = pool.entities[idx].damage * 0.8f;
        f32 burnDur = pool.entities[idx].onHitDuration;
        f32 burnDps = pool.entities[idx].onHitDps;

        // Damage player if in radius
        Vec3 playerDelta = m_localPlayer.position - pos;
        f32 playerDist = sqrtf(playerDelta.x * playerDelta.x + playerDelta.z * playerDelta.z);
        if (playerDist < explosionRadius) {
            Combat::applyDamageToPlayer(m_localPlayer, explosionDmg, &pos);
            if (burnDur > 0.0f) {
                m_localPlayer.burnTimer = fmaxf(m_localPlayer.burnTimer, burnDur);
                m_localPlayer.burnDps = burnDps;
            }
        }
        // Damage friendly NPCs in radius
        for (u32 ni = 0; ni < pool.activeCount; ni++) {
            u32 nIdx = pool.activeList[ni];
            Entity& npc = pool.entities[nIdx];
            if (nIdx == idx) continue;
            if (!(npc.flags & ENT_FRIENDLY)) continue;
            if (npc.flags & ENT_DEAD) continue;
            f32 nDist = length(npc.position - pos);
            if (nDist < explosionRadius) {
                EntityHandle nh = {static_cast<u16>(nIdx), npc.generation};
                Combat::applyDamage(pool, nh, explosionDmg);
            }
        }
        // Visual: spawn explosion particles
        ParticleSystem::spawnExplosion(m_particles, pos, explosionRadius * 0.5f);
    }
}

// Tracks hostile kills, then checks the floor 1-3 first-kill magic-drop guarantee.
// Returns true if the first-kill guarantee fired — caller must return immediately
// to skip boss and normal loot paths (matches original `return; // skip normal drop logic`).
bool Engine::handleFirstKillDrop(EntityPool& pool, u16 idx, Vec3 pos) {
    // Track hostile kills for floor transition screen
    if (!(pool.entities[idx].flags & ENT_FRIENDLY)) {
        m_transition.floorKillCount++;
        AudioSystem::playAt(SfxId::ENEMY_DEATH, pos, m_localPlayer.position);
    }

    // Floors 1-3: first hostile kill guarantees a magic (green) quality drop
    if (!s_firstKillDropGiven && m_level.currentFloor <= 3 &&
        !(pool.entities[idx].flags & ENT_FRIENDLY)) {
        s_firstKillDropGiven = true;
        // Loot/item level is u8 (ItemInstance.itemLevel is save-bound) and rollItem
        // wraps it mod 50 anyway, so cap the effective floor at 255 for loot.
        u16 entLvl = pool.entities[idx].level;
        u8 lvl = static_cast<u8>(entLvl > 255 ? 255 : entLvl);
        if (lvl < 1) lvl = 1;
        // Floor 1: force armor drops so tutorial teaches equipping gear
        ItemInstance item;
        for (u32 attempt = 0; attempt < 20; attempt++) {
            item = ItemGen::rollItem(lvl, m_itemDefs,
                                     m_itemDefCount,
                                     m_affixDefs,
                                     m_affixDefCount);
            if (isItemEmpty(item)) break;
            if (m_level.currentFloor > 1) break; // no restriction above floor 1
            // On floor 1, only allow shields so the tutorial teaches blocking
            ItemSlot slot = m_itemDefs[item.defId].slot;
            if (slot == ItemSlot::OFFHAND) break;
        }
        if (!isItemEmpty(item)) {
            // Force to at least MAGIC rarity
            if (item.rarity < Rarity::MAGIC) item.rarity = Rarity::MAGIC;
            // Re-roll affixes for magic quality (1-2 affixes)
            if (item.affixCount == 0) {
                const ItemDef& idef = m_itemDefs[item.defId];
                ItemGen::rollAffixes(item, lvl, idef.slot,
                                      m_affixDefs, m_affixDefCount,
                                      idef.weaponType);
            }
            WorldItemSystem::spawn(m_worldItems, item,
                                   pos + Vec3{0, 0.5f, 0}, &m_level.grid,
                                   pool.entities[idx].killerSlot); // (L8) reserve to the killer
        }
        return true; // skip normal drop logic for this kill
    }

    return false; // no early exit — proceed to boss/normal loot checks
}

// Boss guaranteed loot drop — mini-bosses drop rare+, major bosses drop legendary.
// Returns true if a boss loot drop was handled — caller must return immediately
// to skip normal loot path (matches original `return; // skip normal loot path`).
bool Engine::handleBossLootDrop(EntityPool& pool, u16 idx, Vec3 pos) {
    if (pool.entities[idx].bossDefIdx != 0xFF &&
        pool.entities[idx].bossDefIdx < m_bossDefs.count) {
        const BossDef& bd = m_bossDefs.defs[pool.entities[idx].bossDefIdx];
        // Loot level is u8 (save-bound ItemInstance.itemLevel) — cap effective floor at 255.
        u16 bossEntLvl = pool.entities[idx].level;
        u8 bossLvl = static_cast<u8>(bossEntLvl > 255 ? 255 : bossEntLvl);
        if (bossLvl < 1) bossLvl = 1;

        // Guaranteed quality drop — re-roll until we get a true legendary
        // (an item whose definition supports legendary rarity + has a skill)
        Rarity minRarity = static_cast<Rarity>(bd.lootGuarantee);
        ItemInstance bossItem;
        for (u32 attempt = 0; attempt < 50; attempt++) {
            bossItem = ItemGen::rollItem(bossLvl, m_itemDefs,
                                          m_itemDefCount,
                                          m_affixDefs,
                                          m_affixDefCount);
            if (isItemEmpty(bossItem)) break;
            // Accept if the item's definition can actually be this rarity
            if (m_itemDefs[bossItem.defId].maxRarity >= minRarity) break;
        }
        if (!isItemEmpty(bossItem)) {
            if (bossItem.rarity < minRarity) {
                bossItem.rarity = minRarity;
                // Re-roll affixes for the upgraded rarity with correct weapon type
                bossItem.affixCount = 0;
                const ItemDef& biDef = m_itemDefs[bossItem.defId];
                ItemGen::rollAffixes(bossItem, bossLvl, biDef.slot,
                                      m_affixDefs, m_affixDefCount,
                                      biDef.weaponType);
            }
            WorldItemSystem::spawn(m_worldItems, bossItem,
                                   pos + Vec3{0, 0.5f, 0}, &m_level.grid,
                                   pool.entities[idx].killerSlot); // (L8) reserve to the killer
        }

        // Bonus drops for major bosses
        for (u8 bd_i = 0; bd_i < bd.bonusDrops; bd_i++) {
            ItemInstance bonus = ItemGen::rollItem(bossLvl, m_itemDefs,
                                                   m_itemDefCount,
                                                   m_affixDefs,
                                                   m_affixDefCount);
            if (!isItemEmpty(bonus)) {
                Vec3 offset = {(f32)(bd_i) * 0.3f - 0.15f, 0.5f, 0.2f};
                WorldItemSystem::spawn(m_worldItems, bonus,
                                       pos + offset, &m_level.grid,
                                       pool.entities[idx].killerSlot); // (L8) reserve to the killer

            }
        }

        // Always drop a globe from bosses
        ItemInstance globe;
        globe.defId = GLOBE_HEALTH_ID;
        globe.uid   = m_worldItems.nextUid++;
        WorldItemSystem::spawn(m_worldItems, globe,
                               pos + Vec3{0.2f, 0.5f, 0.0f}, &m_level.grid);
        return true; // skip normal loot path
    }

    return false; // not a boss — proceed to normal loot roll
}

// Normal (non-boss, non-first-kill) loot roll + globe drop.
// Drop chance scales with enemy level/floor depth. Floor 1 forces shield drops
// so the tutorial teaches blocking. Void return — no early exit in original.
void Engine::handleNormalLootDrop(EntityPool& pool, u16 idx, Vec3 pos) {
    // Hostile enemies only drop loot; chance scales with floor depth.
    // Loot level is u8 (save-bound ItemInstance.itemLevel) — cap effective floor at 255.
    u16 entLvl = pool.entities[idx].level;
    u8 enemyLevel = static_cast<u8>(entLvl > 255 ? 255 : entLvl);
    f32 dropChance = GameConst::LOOT_DROP_CHANCE + enemyLevel * 0.01f;
    if (dropChance > 0.70f) dropChance = 0.70f;
    if (!(pool.entities[idx].flags & ENT_FRIENDLY) &&
        (std::rand() % 100) < static_cast<int>(dropChance * 100.0f)) {
        if (enemyLevel < 1) enemyLevel = 1;
        ItemInstance item;
        // Floor 1: force shield drops so the tutorial teaches blocking
        for (u32 attempt = 0; attempt < 20; attempt++) {
            item = ItemGen::rollItem(enemyLevel, m_itemDefs,
                                     m_itemDefCount,
                                     m_affixDefs,
                                     m_affixDefCount);
            if (isItemEmpty(item)) break;
            if (m_level.currentFloor > 1) break;
            ItemSlot slot = m_itemDefs[item.defId].slot;
            if (slot == ItemSlot::OFFHAND) break;
        }
        if (!isItemEmpty(item)) {
            // (L8) Reserve the drop to whoever landed the kill for a few seconds (their kills,
            // their loot), then it falls back to free-for-all. killerSlot is 0xFF for
            // environmental/AoE kills, which spawn() treats as free-for-all. Globes stay
            // free-for-all auto-pickup below.
            u8 killer = pool.entities[idx].killerSlot;
            WorldItemSystem::spawn(m_worldItems, item,
                                   pos + Vec3{0, 0.5f, 0}, &m_level.grid, killer);
        }

        // Chance to drop a globe (restores both HP and energy on pickup)
        if ((std::rand() % 100) < static_cast<int>(GameConst::GLOBE_DROP_CHANCE * 100.0f)) {
            ItemInstance globe;
            globe.defId = GLOBE_HEALTH_ID; // single globe type
            globe.uid   = m_worldItems.nextUid++;
            WorldItemSystem::spawn(m_worldItems, globe,
                                   pos + Vec3{0.2f, 0.5f, 0.0f});
        }
    }
}

// Ring on-kill passives — Soul Harvest, Phase Strike, Void Kill.
// Only fires for hostile kills and when a ring passive is active.
// Void return — no early exit in original.
void Engine::handleOnKillRingPassives(EntityPool& pool, u16 idx, Vec3 pos) {
    if (m_ringPassive != SkillId::NONE && !(pool.entities[idx].flags & ENT_FRIENDLY)) {
        // Soul Harvest: +5% speed, +3% damage per stack for 10s (max 5)
        if (m_ringPassive == SkillId::SOUL_HARVEST) {
            Player& p = m_localPlayer;
            if (p.soulHarvestStacks < 5) p.soulHarvestStacks++;
            p.soulHarvestTimer = 5.0f; // 5s window to get next kill or stacks reset
            // Speed bonus applied via moveSpeed multiplier
        }
        // Phase Strike (Shadow Ring): 20% on kill, drop smoke bomb — 0.5s stealth + aggro reset
        if (m_ringPassive == SkillId::PHASE_STRIKE && (std::rand() % 100) < 20) {
            Player& p = m_localPlayer;
            p.smokeTimer = 0.5f;
            // Reset aggro on all enemies within 6m
            for (u32 si = 0; si < MAX_ENTITIES; si++) {
                Entity& se = pool.entities[si];
                if (!(se.flags & ENT_ACTIVE) || (se.flags & ENT_DEAD)) continue;
                if (se.flags & ENT_FRIENDLY) continue;
                Vec3 diff = se.position - pos;
                if (diff.x * diff.x + diff.z * diff.z < 6.0f * 6.0f) {
                    se.aiState = AIState::IDLE;
                    se.velocity = {0, 0, 0};
                }
            }
            // Dark grey smoke nova at kill position
            for (u32 ni = 0; ni < Engine::MAX_NOVA_FX; ni++) {
                if (!m_fx.novaFX[ni].active) {
                    m_fx.novaFX[ni] = {pos, 4.0f, 1.2f, true, {0.3f, 0.3f, 0.35f}};
                    break;
                }
            }
        }
        // Void Kill: 15% chance to spawn void zone on corpse
        if (m_ringPassive == SkillId::VOID_KILL && (std::rand() % 100) < 15) {
            // AoE void damage to nearby enemies
            EntityHandle aoeHits[MAX_ENTITIES];
            f32 aoeDists[MAX_ENTITIES];
            u32 aoeCount = CombatQuery::queryConeSorted(
                pool, pos, {0,-1,0}, -1.0f, 3.0f,
                aoeHits, aoeDists, MAX_ENTITIES);
            for (u32 h = 0; h < aoeCount; h++) {
                Entity* ve = handleGet(pool, aoeHits[h]);
                if (!ve || (ve->flags & ENT_DEAD) || (ve->flags & ENT_FRIENDLY)) continue;
                f32 missingHp = ve->maxHealth - ve->health;
                Combat::applyDamage(pool, aoeHits[h], 10.0f + missingHp * 0.6f);
            }
            // Dark purple nova visual
            for (u32 ni = 0; ni < Engine::MAX_NOVA_FX; ni++) {
                if (!m_fx.novaFX[ni].active) {
                    m_fx.novaFX[ni] = {pos, 3.0f, 1.0f, true, {0.4f, 0.1f, 0.6f}};
                    break;
                }
            }
        }
    }
}
