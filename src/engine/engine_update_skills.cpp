// Engine skill and passive equipment tick helpers — extracted from gameUpdate.
// Contains: skill cooldown draining, passive equipment binding (weapon proc /
// armor aura / ring passive), class skill selection + activation, boot/helmet
// equipment skill activation, and the combined armor-aura + ring-passive entity pass.
// All methods are Engine:: members called in the exact order they appeared inline.
// IMPORTANT ordering invariant: tickPassiveEquipment() MUST run before
// tickArmorRingPassives() — the latter reads m_armorAura/m_ringPassive/m_weaponProc
// that the former sets.

#define SDL_MAIN_HANDLED
#include <SDL.h>

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
#include "audio/audio.h"

#include <glad/glad.h>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>

// Shared statics defined in engine.cpp
extern Engine* s_engine;
extern FrameAllocator s_frameAllocator;
extern bool s_firstKillDropGiven;

// ---------------------------------------------------------------------------
// tickSkillCooldowns — drains the main skill energy/cooldown pool via
// SkillSystem::update, then individually drains all four class skill cooldowns
// and both equipment (boot/helmet) skill cooldowns.
// ---------------------------------------------------------------------------
void Engine::tickSkillCooldowns(f32 dt) {
    // Update skill state (energy regen, cooldowns)
    SkillSystem::update(m_skillStates[m_localPlayerIndex], dt);
    // Tick class skill cooldowns (shared energy synced from main pool)
    for (u32 s = 0; s < 4; s++) {
        if (m_classSkillStates[s].cooldownTimer > 0.0f) {
            m_classSkillStates[s].cooldownTimer -= dt;
            if (m_classSkillStates[s].cooldownTimer < 0.0f) m_classSkillStates[s].cooldownTimer = 0.0f;
        }
    }
    // Tick equipment skill cooldowns (boots F, helmet G)
    if (m_bootSkillStates[0].cooldownTimer > 0.0f) {
        m_bootSkillStates[0].cooldownTimer -= dt;
        if (m_bootSkillStates[0].cooldownTimer < 0.0f) m_bootSkillStates[0].cooldownTimer = 0.0f;
    }
    if (m_helmetSkillStates[0].cooldownTimer > 0.0f) {
        m_helmetSkillStates[0].cooldownTimer -= dt;
        if (m_helmetSkillStates[0].cooldownTimer < 0.0f) m_helmetSkillStates[0].cooldownTimer = 0.0f;
    }
}

// ---------------------------------------------------------------------------
// tickPassiveEquipment — reads equipped weapon/armor/ring legendary skills and
// caches them into m_weaponProc / m_armorAura / m_ringPassive each tick.
// Must run before tickArmorRingPassives() which consumes these values.
// ---------------------------------------------------------------------------
void Engine::tickPassiveEquipment() {
    // --- Weapon on-hit proc (legendary weapon passive) ---
    {
        const ItemInstance& wpn = m_inventories[m_localPlayerIndex].equipped[static_cast<u32>(ItemSlot::WEAPON)];
        m_weaponProc = (!isItemEmpty(wpn) && wpn.rarity == Rarity::LEGENDARY)
            ? m_itemDefs[wpn.defId].legendarySkillId : SkillId::NONE;
    }
    // Armor passive aura
    {
        const ItemInstance& armor = m_inventories[m_localPlayerIndex].equipped[static_cast<u32>(ItemSlot::ARMOR)];
        m_armorAura = (!isItemEmpty(armor) && armor.rarity == Rarity::LEGENDARY)
            ? m_itemDefs[armor.defId].legendarySkillId : SkillId::NONE;
    }
    // Ring passive effect
    {
        const ItemInstance& ring = m_inventories[m_localPlayerIndex].equipped[static_cast<u32>(ItemSlot::RING)];
        m_ringPassive = (!isItemEmpty(ring) && ring.rarity == Rarity::LEGENDARY)
            ? m_itemDefs[ring.defId].legendarySkillId : SkillId::NONE;
        m_localPlayer.ringPassive = static_cast<u8>(m_ringPassive);
    }
}

// ---------------------------------------------------------------------------
// handleClassSkillActivation — keys 1-4 select the active class skill slot;
// right-click (CLASS_SKILL action) fires it using the class skill state for
// cooldown tracking. eyePos is computed in gameUpdate right before this call
// so it reflects the current eyeHeight (which may have been modified by
// view-bob earlier in the same tick).
// ---------------------------------------------------------------------------
void Engine::handleClassSkillActivation(f32 dt, Vec3 eyePos) {
    // --- Class skill selection (keys 1-4) ---
    if (!m_inventoryOpen) {
        const ClassDef& cls = kClassDefs[static_cast<u32>(m_playerClass)];
        for (u8 s = 0; s < 4; s++) {
            if (Input::isActionPressed(static_cast<GameAction>(static_cast<u8>(GameAction::SKILL_1) + s))) {
                // Effective floor accounts for difficulty (Nightmare=+50, Hell=+100)
                u32 effectiveFloor = m_level.currentFloor + m_difficulty * 50;
                if (effectiveFloor >= cls.skillUnlockFloor[s]) {
                    m_activeClassSkill = s;
                }
            }
        }
    }

    // --- Class skill activation (right-click) ---
    if (Input::isActionPressed(GameAction::CLASS_SKILL) && !m_inventoryOpen) {
        const ClassDef& cls = kClassDefs[static_cast<u32>(m_playerClass)];
        u8 slot = m_activeClassSkill;
        u32 effectiveFloor = m_level.currentFloor + m_difficulty * 50;
        if (effectiveFloor >= cls.skillUnlockFloor[slot]) {
            // Use the class skill state for cooldown tracking, shared energy pool
            m_classSkillStates[slot].activeSkill = cls.skills[slot];
            m_classSkillStates[slot].energy = m_skillStates[m_localPlayerIndex].energy;
            m_classSkillStates[slot].maxEnergy = m_skillStates[m_localPlayerIndex].maxEnergy;

            // Thunderclap upgrade: increase stun from 0.2s to 0.5s past upgrade floor
            SkillDef* tcDef = nullptr;
            f32 origDuration = 0.0f;
            if (cls.skills[slot] == SkillId::THUNDERCLAP &&
                m_level.currentFloor >= cls.skillUpgradeFloor[slot]) {
                tcDef = const_cast<SkillDef*>(SkillSystem::findSkillDef(m_skillDefs, m_skillDefCount,
                                                                         SkillId::THUNDERCLAP));
                if (tcDef) { origDuration = tcDef->duration; tcDef->duration = 0.5f; }
            }

            SkillSystem::setSkillPower(0.0f);  // class skills use base power
            // Class skill damage scales at 6% per effective floor (slower than enemy 10%)
            { u32 effFloor = m_level.currentFloor + m_difficulty * 50;
              SkillSystem::setClassDamageMult(1.0f + (effFloor - 1) * 0.06f); }
            // Set weapon damage for Marksman skills that scale off equipped weapon
            { const ItemInstance& wpn = m_inventories[m_localPlayerIndex].equipped[static_cast<u32>(ItemSlot::WEAPON)];
              WeaponDef wd = !isItemEmpty(wpn)
                  ? Inventory::getWeaponFromItem(m_inventories[m_localPlayerIndex], m_itemDefs, wpn)
                  : m_weaponDefs[0];
              SkillSystem::setWeaponDamage(wd.damage); }
            if (SkillSystem::tryActivate(m_classSkillStates[slot], m_skillDefs, m_skillDefCount,
                                          eyePos, m_localPlayer.forward, m_localPlayer.yaw,
                                          m_projectiles, m_entities, m_level.grid, m_localPlayer,
                                          m_inventories[m_localPlayerIndex].bonusCooldownReduction)) {
                m_skillStates[m_localPlayerIndex].energy = m_classSkillStates[slot].energy;
            }

            // Restore original duration
            if (tcDef) tcDef->duration = origDuration;
        }
    }
}

// ---------------------------------------------------------------------------
// handleEquipmentSkillActivation — binds boot/helmet legendary skills each
// tick and activates them on F/G key press. Equipment skills are gated like
// class skills: they cost mana from the player's shared energy pool (and use the
// skill's own cooldown). eyePos is threaded from gameUpdate as for class skills.
// ---------------------------------------------------------------------------
void Engine::handleEquipmentSkillActivation(f32 dt, Vec3 eyePos) {
    // --- Equipment legendary skill binding (boots/helmet/ring) ---
    // Boots legendary → F key
    {
        const ItemInstance& boots = m_inventories[m_localPlayerIndex].equipped[static_cast<u32>(ItemSlot::BOOTS)];
        SkillId bootSkill = (!isItemEmpty(boots) && boots.rarity == Rarity::LEGENDARY)
            ? m_itemDefs[boots.defId].legendarySkillId : SkillId::NONE;
        m_bootSkillStates[0].activeSkill = bootSkill;
    }
    // Helmet legendary → G key
    {
        const ItemInstance& helm = m_inventories[m_localPlayerIndex].equipped[static_cast<u32>(ItemSlot::HELMET)];
        SkillId helmSkill = (!isItemEmpty(helm) && helm.rarity == Rarity::LEGENDARY)
            ? m_itemDefs[helm.defId].legendarySkillId : SkillId::NONE;
        m_helmetSkillStates[0].activeSkill = helmSkill;
    }

    // --- Boot skill activation (F key) ---
    // Equipment legendary skills are cooldown-only (no energy cost deducted from player)
    if (Input::isActionPressed(GameAction::BOOT_SKILL) && !m_inventoryOpen &&
        m_bootSkillStates[0].activeSkill != SkillId::NONE) {
        // Item skills draw from the player's shared energy pool (cost mana like class
        // skills): copy the pool in, let tryActivate spend energyCost, copy back on success.
        m_bootSkillStates[0].energy    = m_skillStates[m_localPlayerIndex].energy;
        m_bootSkillStates[0].maxEnergy = m_skillStates[m_localPlayerIndex].maxEnergy;
        // Scale by boots item level — item skills use base class damage (1.0)
        { u8 lvl = m_inventories[m_localPlayerIndex].equipped[static_cast<u32>(ItemSlot::BOOTS)].itemLevel;
          SkillSystem::setSkillPower(lvl > 1 ? static_cast<f32>(lvl - 1) / 149.0f : 0.0f); }
        SkillSystem::setClassDamageMult(1.0f);
        if (SkillSystem::tryActivate(m_bootSkillStates[0], m_skillDefs, m_skillDefCount,
                                      eyePos, m_localPlayer.forward, m_localPlayer.yaw,
                                      m_projectiles, m_entities, m_level.grid, m_localPlayer,
                                      m_inventories[m_localPlayerIndex].bonusCooldownReduction)) {
            m_skillStates[m_localPlayerIndex].energy = m_bootSkillStates[0].energy; // deduct spent mana
        }
    }

    // --- Helmet skill activation (G key) ---
    if (Input::isActionPressed(GameAction::HELMET_SKILL) && !m_inventoryOpen &&
        m_helmetSkillStates[0].activeSkill != SkillId::NONE) {
        // Item skills draw from the player's shared energy pool (cost mana like class skills).
        m_helmetSkillStates[0].energy    = m_skillStates[m_localPlayerIndex].energy;
        m_helmetSkillStates[0].maxEnergy = m_skillStates[m_localPlayerIndex].maxEnergy;
        // Scale by helmet item level — item skills use base class damage (1.0)
        { u8 lvl = m_inventories[m_localPlayerIndex].equipped[static_cast<u32>(ItemSlot::HELMET)].itemLevel;
          SkillSystem::setSkillPower(lvl > 1 ? static_cast<f32>(lvl - 1) / 149.0f : 0.0f); }
        SkillSystem::setClassDamageMult(1.0f);
        if (SkillSystem::tryActivate(m_helmetSkillStates[0], m_skillDefs, m_skillDefCount,
                                      eyePos, m_localPlayer.forward, m_localPlayer.yaw,
                                      m_projectiles, m_entities, m_level.grid, m_localPlayer,
                                      m_inventories[m_localPlayerIndex].bonusCooldownReduction)) {
            m_skillStates[m_localPlayerIndex].energy = m_helmetSkillStates[0].energy; // deduct spent mana
        }
    }
}

// ---------------------------------------------------------------------------
// tickArmorRingPassives — ring-specific timer drains (soul harvest, Second
// Wind cooldown), Second Wind and Divine Judgment emergency triggers, and a
// merged entity pass for armor aura effects (burn/freeze/poison near-aura),
// gravity-pull ring, and thorns reflect damage.
// Must run AFTER tickPassiveEquipment() sets m_armorAura / m_ringPassive.
// ---------------------------------------------------------------------------
void Engine::tickArmorRingPassives(f32 dt) {
    // --- Armor aura + ring passives: single merged entity pass ---
    // Ring-specific timers (no entity iteration needed)
    if (m_ringPassive != SkillId::NONE) {
        if (m_localPlayer.soulHarvestTimer > 0.0f) {
            m_localPlayer.soulHarvestTimer -= dt;
            if (m_localPlayer.soulHarvestTimer <= 0.0f) m_localPlayer.soulHarvestStacks = 0;
        }
        if (m_localPlayer.secondWindCooldown > 0.0f)
            m_localPlayer.secondWindCooldown -= dt;

        // Second Wind: at <20% HP, heal 30% + 2s invuln (60s cooldown)
        if (m_ringPassive == SkillId::SECOND_WIND &&
            m_localPlayer.health > 0.0f &&
            m_localPlayer.health < m_localPlayer.maxHealth * 0.2f &&
            m_localPlayer.secondWindCooldown <= 0.0f) {
            m_localPlayer.health += m_localPlayer.maxHealth * 0.3f;
            if (m_localPlayer.health > m_localPlayer.maxHealth)
                m_localPlayer.health = m_localPlayer.maxHealth;
            m_localPlayer.invulnTimer = 2.0f;
            m_localPlayer.secondWindCooldown = 60.0f;
            for (u32 ni = 0; ni < MAX_NOVA_FX; ni++) {
                if (!m_fx.novaFX[ni].active) {
                    m_fx.novaFX[ni] = {m_localPlayer.position, 2.0f, 0.8f, true, {1.0f, 0.9f, 0.3f}};
                    break;
                }
            }
            LOG_INFO("SECOND WIND triggered! Healed 30%%, 2s invuln");
        }

        // Divine Judgment: at <25% HP, full heal + cleanse + AoE stun (45s cooldown)
        if (m_ringPassive == SkillId::DIVINE_JUDGMENT &&
            m_localPlayer.health > 0.0f &&
            m_localPlayer.health < m_localPlayer.maxHealth * 0.25f &&
            m_localPlayer.secondWindCooldown <= 0.0f) {
            // Full heal + cleanse all debuffs
            m_localPlayer.health = m_localPlayer.maxHealth;
            m_localPlayer.slowTimer   = 0.0f;
            m_localPlayer.poisonTimer = 0.0f;
            m_localPlayer.poisonDps   = 0.0f;
            m_localPlayer.burnTimer   = 0.0f;
            m_localPlayer.burnDps     = 0.0f;
            m_localPlayer.freezeTimer = 0.0f;
            m_localPlayer.invulnTimer = 1.5f;
            m_localPlayer.secondWindCooldown = 45.0f;
            // AoE stun nearby enemies
            for (u32 a = 0; a < m_entities.activeCount; a++) {
                Entity& ent = m_entities.entities[m_entities.activeList[a]];
                if (ent.flags & ENT_FRIENDLY || ent.flags & ENT_DEAD) continue;
                if (lengthSq(ent.position - m_localPlayer.position) < 25.0f) { // 5m
                    ent.stunTimer = 1.5f;
                }
            }
            for (u32 ni = 0; ni < MAX_NOVA_FX; ni++) {
                if (!m_fx.novaFX[ni].active) {
                    m_fx.novaFX[ni] = {m_localPlayer.position, 5.0f, 0.8f, true, {1.0f, 0.95f, 0.4f}};
                    break;
                }
            }
            LOG_INFO("DIVINE JUDGMENT triggered! Full heal, cleanse, AoE stun");
        }
    }

    // Single pass over entities for armor aura + gravity pull + thorns
    bool needEntityPass = (m_armorAura != SkillId::NONE) ||
                          (m_ringPassive == SkillId::GRAVITY_PULL) ||
                          (m_ringPassive == SkillId::THORNS && m_localPlayer.lastDamageTaken > 0.0f);
    if (needEntityPass) {
        Vec3 pPos = m_localPlayer.position;
        bool doGravity = (m_ringPassive == SkillId::GRAVITY_PULL);
        bool doThorns  = (m_ringPassive == SkillId::THORNS && m_localPlayer.lastDamageTaken > 0.0f);
        f32  reflectDmg = doThorns ? m_localPlayer.lastDamageTaken * 0.2f : 0.0f;
        f32  bestThornsDist2 = 25.0f; // 5m squared
        EntityHandle bestThornsH = {};

        for (u32 a = 0; a < m_entities.activeCount; a++) {
            u32 idx = m_entities.activeList[a];
            Entity& ent = m_entities.entities[idx];
            if (ent.flags & ENT_DEAD) continue;
            if (ent.flags & ENT_FRIENDLY) continue;
            if (ent.enemyType == EnemyType::PROP) continue;

            Vec3 delta = ent.position - pPos;
            f32 distSq = delta.x*delta.x + delta.z*delta.z; // XZ distance (no sqrt)

            // Armor aura effects (use squared distance thresholds)
            if (m_armorAura != SkillId::NONE) {
                switch (m_armorAura) {
                    case SkillId::METEOR_STRIKE:
                        if (distSq < 9.0f) { ent.burnTimer = 0.5f; ent.burnDps = 2.0f; }
                        break;
                    case SkillId::FROZEN_ORB:
                        if (distSq < 16.0f) { ent.freezeTimer = 0.5f; }
                        break;
                    case SkillId::BLOOD_NOVA:
                        if (distSq < 9.0f) { ent.poisonTimer = 0.5f; ent.poisonDps = 1.0f; }
                        break;
                    case SkillId::CHAIN_LIGHTNING:
                        if (distSq < 9.0f) { ent.freezeTimer = 0.3f; }
                        break;
                    case SkillId::PHASE_DASH:
                        if (distSq < 9.0f) { ent.freezeTimer = 0.4f; }
                        break;
                    default: break;
                }
            }

            // Gravity pull: within 5m (25 sq), pull toward player
            if (doGravity && distSq > 0.25f && distSq < 25.0f) {
                f32 dist = sqrtf(distSq); // sqrt only for entities in range
                f32 pullStrength = (1.0f - dist / 5.0f) * 2.0f * dt;
                Vec3 toPlayer = pPos - ent.position;
                ent.position = ent.position + normalize(toPlayer) * pullStrength;
            }

            // Thorns: track nearest enemy
            if (doThorns && distSq < bestThornsDist2) {
                bestThornsDist2 = distSq;
                bestThornsH = {static_cast<u16>(idx), ent.generation};
            }
        }

        if (doThorns && bestThornsDist2 < 25.0f) {
            Combat::applyDamage(m_entities, bestThornsH, reflectDmg);
        }
    }
    m_localPlayer.lastDamageTaken = 0.0f;
}
