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
#include "game/skill_internal.h"   // fireChainLightning — Static Charge / Thunderwall proc
#include "game/static_charge.h"    // Capacitor Mail stack accumulator (pure, tested)
#include "game/shrine.h"           // Shrine of Sorcery % — applySpellScaling call sites
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
// tickSkillCooldowns — energy regen (via SkillSystem::update), then
// re-derives cooldownTimer for class/boot/helm from the authoritative
// lastActivationTick (R17). cooldownTimer stays as a HUD-only value; the
// real cooldown gate lives in tryActivate via tick comparison.
// ---------------------------------------------------------------------------
void Engine::tickSkillCooldowns(f32 dt) {
    // Energy regen + any non-cooldown skill state (SkillSystem::update no longer
    // drains cooldownTimer post-R17, but keeps energy regen / projectile housekeeping).
    SkillSystem::update(m_skillStates[m_localPlayerIndex], dt);

    // R17: HUD-derive cooldownTimer from authoritative lastActivationTick. The gate
    // doesn't read cooldownTimer — only HUD does — so any small drift between f32
    // accumulation and the integer-derived value is invisible to gameplay logic.
    const u32 nowTick = currentLocalTick();
    auto deriveCooldownTimer = [&](SkillState& ss, f32 cdr) {
        if (ss.activeSkill == SkillId::NONE || ss.lastActivationTick == 0) {
            ss.cooldownTimer = 0.0f;
            return;
        }
        const SkillDef* def = SkillSystem::findSkillDef(m_skillDefs, m_skillDefCount, ss.activeSkill);
        if (!def) { ss.cooldownTimer = 0.0f; return; }
        const u32 cooldownTicks = SkillSystem::computeCooldownTicks(def->cooldown, cdr);
        const u32 elapsed = nowTick - ss.lastActivationTick;
        ss.cooldownTimer = (elapsed < cooldownTicks)
                           ? static_cast<f32>(cooldownTicks - elapsed) * (1.0f / 60.0f)
                           : 0.0f;
    };
    const f32 itemCdr = m_inventories[m_localPlayerIndex].bonusCooldownReduction;
    for (u32 s = 0; s < 4; s++) deriveCooldownTimer(m_classSkillStates[s], itemCdr);
    deriveCooldownTimer(m_bootSkillStates[m_localPlayerIndex],   itemCdr);
    deriveCooldownTimer(m_helmetSkillStates[m_localPlayerIndex], itemCdr);
}

// Thunderclap's floor upgrade. fireThunderclap reads its stun straight off the shared SkillDef, so
// the upgrade is applied by scaling that def around the cast and restoring it immediately after.
// Call begin* before SkillSystem::tryActivate and end* after, ALWAYS as a pair.
//
// Three things were wrong with the version this replaces, all silent:
//   1. It ASSIGNED a hardcoded 0.5 s. That was written when the def's stun was 0.2 s; skills.json
//      now says 0.8 s, so "upgrading" past the upgrade floor SHORTENED the stun. A multiplier can't
//      invert like that whatever the def says.
//   2. It gated on the RAW floor, while the HUD paints its gold "upgraded" border using
//      effectiveFloor (floor + difficulty*50) — so on Nightmare/Hell the skill bar showed the skill
//      upgraded while the code never applied it.
//   3. It lived only in the LOCAL cast path, so in co-op a GUEST's Warrior never got the upgrade at
//      all. The server's remote-cast path (engine_net.cpp processRemoteActivation) now calls this
//      too, which is the whole reason it is a shared helper rather than an inline block.
SkillDef* Engine::beginThunderclapUpgrade(SkillId skill, u8 upgradeFloor, u32 effectiveFloor,
                                          f32& outOrigDuration) {
    constexpr f32 THUNDERCLAP_UPGRADE_MULT = 1.5f;   // 0.8 s base -> 1.2 s upgraded
    outOrigDuration = 0.0f;
    if (skill != SkillId::THUNDERCLAP || effectiveFloor < upgradeFloor) return nullptr;
    SkillDef* def = const_cast<SkillDef*>(
        SkillSystem::findSkillDef(m_skillDefs, m_skillDefCount, SkillId::THUNDERCLAP));
    if (!def) return nullptr;
    outOrigDuration = def->duration;
    def->duration   = outOrigDuration * THUNDERCLAP_UPGRADE_MULT;
    return def;
}

void Engine::endThunderclapUpgrade(SkillDef* def, f32 origDuration) {
    if (def) def->duration = origDuration;
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
    // Offhand passive (legendary shield identity). Stamped onto the Player like ringPassive,
    // because the perfect-block callback and the projectile parry receive only a Player& —
    // the local player here; remote views get it via serverNetPost + seedRemoteView.
    {
        const ItemInstance& off = m_inventories[m_localPlayerIndex].equipped[static_cast<u32>(ItemSlot::OFFHAND)];
        m_localPlayer.offhandSkill = static_cast<u8>((!isItemEmpty(off) && off.rarity == Rarity::LEGENDARY)
            ? m_itemDefs[off.defId].legendarySkillId : SkillId::NONE);
    }
    // Gloves on-hit passive (Frenzy)
    {
        const ItemInstance& gl = m_inventories[m_localPlayerIndex].equipped[static_cast<u32>(ItemSlot::GLOVES)];
        m_glovesPassive = (!isItemEmpty(gl) && gl.rarity == Rarity::LEGENDARY)
            ? m_itemDefs[gl.defId].legendarySkillId : SkillId::NONE;
    }
    // Defensive-pack affix cache (armor/regen/thorns). Summed on demand from equipped affixes —
    // PlayerInventory holds no bonus* field for these (keeps the save format unchanged), so we
    // stamp them into the Player's transient combat cache here, once per frame, for the damage
    // and regen paths to read cheaply (mirrors how damageReduction is read in applyDamageToPlayer).
    {
        const PlayerInventory& inv = m_inventories[m_localPlayerIndex];
        m_localPlayer.armorRating    = Inventory::armorRating(inv);
        m_localPlayer.healthRegen    = Inventory::healthRegenRate(inv);
        m_localPlayer.thornsPctBonus = Inventory::thornsPct(inv);
        m_localPlayer.ccResist       = Inventory::ccResist(inv);   // rolled CC_RESIST affixes, capped
        // ccDodgeImmune: true iff the equipped boots are the Steadfast Greaves — their legendary
        // skill is BREAK_FREE, the single marker for the anti-CC boots (Task 5 defines BREAK_FREE).
        {
            const ItemInstance& boots = inv.equipped[(u32)ItemSlot::BOOTS];
            m_localPlayer.ccDodgeImmune = !isItemEmpty(boots) &&
                m_itemDefs[boots.defId].legendarySkillId == SkillId::BREAK_FREE;
        }
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
    // botMayAct() carve-out: the Autoplay bot selects its class-skill slot with its own inventory open.
    if (!gameplayInputFrozen() || botMayAct()) {
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
    // Stunned players can't cast (action-lock). BOOT_SKILL (Break Free) is deliberately NOT gated
    // this way below — pressing F is how you escape the stun.
    if (Input::isActionPressed(GameAction::CLASS_SKILL) && (!gameplayInputFrozen() || botMayAct())
        && m_localPlayer.stunTimer <= 0.0f) {
        const ClassDef& cls = kClassDefs[static_cast<u32>(m_playerClass)];
        u8 slot = m_activeClassSkill;
        u32 effectiveFloor = m_level.currentFloor + m_difficulty * 50;
        if (effectiveFloor >= cls.skillUnlockFloor[slot]) {
            // Use the class skill state for cooldown tracking, shared energy pool
            m_classSkillStates[slot].activeSkill = cls.skills[slot];
            m_classSkillStates[slot].energy = m_skillStates[m_localPlayerIndex].energy;
            m_classSkillStates[slot].maxEnergy = m_skillStates[m_localPlayerIndex].maxEnergy;

            // Thunderclap's floor upgrade. Shared with the SERVER's remote-cast path so a guest's
            // Warrior gets the same upgrade the host's does (it previously did not — see
            // beginThunderclapUpgrade).
            f32 origDuration = 0.0f;
            SkillDef* tcDef = beginThunderclapUpgrade(cls.skills[slot],
                                                      cls.skillUpgradeFloor[slot],
                                                      effectiveFloor, origDuration);
            {
            }

            SkillSystem::setSkillPower(0.0f);  // class skills use base power
            // Class skill damage scales at 6% per effective floor (slower than enemy 10%);
            // gear spell damage (flat + %) rides on top via applySpellScaling.
            { u32 effFloor = m_level.currentFloor + m_difficulty * 50;
              applySpellScaling(m_inventories[m_localPlayerIndex],
                                1.0f + (effFloor - 1) * 0.06f,
                                Shrine::spellShrinePct(m_localPlayer.shrineBuff,
                                                       m_localPlayer.shrineBuffTimer)); }
            // Set weapon damage for Marksman skills that scale off equipped weapon
            { const ItemInstance& wpn = m_inventories[m_localPlayerIndex].equipped[static_cast<u32>(ItemSlot::WEAPON)];
              WeaponDef wd = !isItemEmpty(wpn)
                  ? Inventory::getWeaponFromItem(m_inventories[m_localPlayerIndex], m_itemDefs, wpn)
                  : m_weaponDefs[0];
              SkillSystem::setWeaponDamage(wd.damage);
              // What Barrage fires: bolts from a crossbow, arrows from everything else.
              SkillSystem::setWeaponProjectileMesh(
                  (!isItemEmpty(wpn) && m_itemDefs[wpn.defId].weaponSubtype == WeaponSubtype::CROSSBOW)
                      ? m_meshIdBolt : m_meshIdArrow); }
            // Credit this caster for kills the skill lands — direct hits AND DoT (poison/burn) the
            // skill applies, which kill later in EntitySystem::tickTimers via the stamped src slot.
            Combat::setAttackingPlayer(activeNetSlot());
            if (SkillSystem::tryActivate(m_classSkillStates[slot], m_skillDefs, m_skillDefCount,
                                          eyePos, m_localPlayer.forward, m_localPlayer.yaw,
                                          m_projectiles, m_entities, m_level.grid, m_localPlayer,
                                          currentLocalTick(),
                                          m_inventories[m_localPlayerIndex].bonusCooldownReduction)) {
                m_skillStates[m_localPlayerIndex].energy = m_classSkillStates[slot].energy;
                // R17: tryActivate set m_classSkillStates[slot].lastActivationTick =
                // currentLocalTick() on success. Server-side gate for the matching
                // INPUT_EX_SKILL will evaluate the same tick comparison and agree.
                // M9: record the predicted class skill activation so M10 can ack/reject
                // it via SV_SKILL_RESULT. CLIENT only.
                if (m_netRole == NetRole::CLIENT) {
                    PendingSkillRingOps::record(m_pendingSkills, m_clientTick, slot);
                }
            }

            endThunderclapUpgrade(tcDef, origDuration);
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
        m_bootSkillStates[m_localPlayerIndex].activeSkill = bootSkill;
    }
    // Helmet legendary → G key
    {
        const ItemInstance& helm = m_inventories[m_localPlayerIndex].equipped[static_cast<u32>(ItemSlot::HELMET)];
        SkillId helmSkill = (!isItemEmpty(helm) && helm.rarity == Rarity::LEGENDARY)
            ? m_itemDefs[helm.defId].legendarySkillId : SkillId::NONE;
        m_helmetSkillStates[m_localPlayerIndex].activeSkill = helmSkill;
    }

    // --- Boot skill activation (F key) ---
    // Equipment legendary skills DO cost energy — they draw from the shared pool like class
    // skills (see below), so the clientNetPre wire-mask must also strip the boot/helm ext bits
    // when the pool can't afford them (the server casts with energy=999 and won't re-check).
    if (Input::isActionPressed(GameAction::BOOT_SKILL) && (!gameplayInputFrozen() || botMayAct()) &&
        m_bootSkillStates[m_localPlayerIndex].activeSkill != SkillId::NONE) {
        // Item skills draw from the player's shared energy pool (cost mana like class
        // skills): copy the pool in, let tryActivate spend energyCost, copy back on success.
        m_bootSkillStates[m_localPlayerIndex].energy    = m_skillStates[m_localPlayerIndex].energy;
        m_bootSkillStates[m_localPlayerIndex].maxEnergy = m_skillStates[m_localPlayerIndex].maxEnergy;
        // Scale by boots item level — item skills use base class damage (1.0)
        { u8 lvl = m_inventories[m_localPlayerIndex].equipped[static_cast<u32>(ItemSlot::BOOTS)].itemLevel;
          SkillSystem::setSkillPower(lvl > 1 ? static_cast<f32>(lvl - 1) / 149.0f : 0.0f); }
        applySpellScaling(m_inventories[m_localPlayerIndex], 1.0f,
                          Shrine::spellShrinePct(m_localPlayer.shrineBuff,
                                                 m_localPlayer.shrineBuffTimer));  // item skills
        if (SkillSystem::tryActivate(m_bootSkillStates[m_localPlayerIndex], m_skillDefs, m_skillDefCount,
                                      eyePos, m_localPlayer.forward, m_localPlayer.yaw,
                                      m_projectiles, m_entities, m_level.grid, m_localPlayer,
                                      currentLocalTick(),
                                      m_inventories[m_localPlayerIndex].bonusCooldownReduction)) {
            m_skillStates[m_localPlayerIndex].energy = m_bootSkillStates[m_localPlayerIndex].energy; // deduct spent mana
            // R17: tryActivate stamped lastActivationTick. Server's same-tick gate agrees by construction.
            if (m_netRole == NetRole::CLIENT) {
                PendingSkillRingOps::record(m_pendingSkills, m_clientTick, PENDING_SKILL_SLOT_BOOT);
            }
        }
    }

    // --- Helmet skill activation (G key) ---
    if (Input::isActionPressed(GameAction::HELMET_SKILL) && (!gameplayInputFrozen() || botMayAct()) &&
        m_localPlayer.stunTimer <= 0.0f &&    // stunned: no helmet cast (BOOT_SKILL/Break Free is exempt)
        m_helmetSkillStates[m_localPlayerIndex].activeSkill != SkillId::NONE) {
        // Item skills draw from the player's shared energy pool (cost mana like class skills).
        m_helmetSkillStates[m_localPlayerIndex].energy    = m_skillStates[m_localPlayerIndex].energy;
        m_helmetSkillStates[m_localPlayerIndex].maxEnergy = m_skillStates[m_localPlayerIndex].maxEnergy;
        // Scale by helmet item level — item skills use base class damage (1.0)
        { u8 lvl = m_inventories[m_localPlayerIndex].equipped[static_cast<u32>(ItemSlot::HELMET)].itemLevel;
          SkillSystem::setSkillPower(lvl > 1 ? static_cast<f32>(lvl - 1) / 149.0f : 0.0f); }
        applySpellScaling(m_inventories[m_localPlayerIndex], 1.0f,
                          Shrine::spellShrinePct(m_localPlayer.shrineBuff,
                                                 m_localPlayer.shrineBuffTimer));  // item skills
        if (SkillSystem::tryActivate(m_helmetSkillStates[m_localPlayerIndex], m_skillDefs, m_skillDefCount,
                                      eyePos, m_localPlayer.forward, m_localPlayer.yaw,
                                      m_projectiles, m_entities, m_level.grid, m_localPlayer,
                                      currentLocalTick(),
                                      m_inventories[m_localPlayerIndex].bonusCooldownReduction)) {
            m_skillStates[m_localPlayerIndex].energy = m_helmetSkillStates[m_localPlayerIndex].energy; // deduct spent mana
            // R17: tryActivate stamped lastActivationTick. Server's same-tick gate agrees by construction.
            if (m_netRole == NetRole::CLIENT) {
                PendingSkillRingOps::record(m_pendingSkills, m_clientTick, PENDING_SKILL_SLOT_HELMET);
            }
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
    // Frenzy (glove passive) buff decay — single shared timer, all stacks drop together.
    // Ticked unconditionally (not gated on m_glovesPassive) so stacks still fade if the
    // gloves are unequipped mid-buff.
    if (m_localPlayer.frenzyTimer > 0.0f) {
        m_localPlayer.frenzyTimer -= dt;
        if (m_localPlayer.frenzyTimer <= 0.0f) m_localPlayer.frenzyStacks = 0;
    }

    if (m_ringPassive != SkillId::NONE) {
        if (m_localPlayer.soulHarvestTimer > 0.0f) {
            m_localPlayer.soulHarvestTimer -= dt;
            if (m_localPlayer.soulHarvestTimer <= 0.0f) m_localPlayer.soulHarvestStacks = 0;
        }
        if (m_localPlayer.secondWindCooldown > 0.0f)
            m_localPlayer.secondWindCooldown -= dt;

        // Second Wind: at <20% HP, heal 30% + 1.5s invuln (60s cooldown)
        if (m_ringPassive == SkillId::SECOND_WIND &&
            m_localPlayer.health > 0.0f &&
            m_localPlayer.health < m_localPlayer.maxHealth * 0.2f &&
            m_localPlayer.secondWindCooldown <= 0.0f) {
            m_localPlayer.health += m_localPlayer.maxHealth * 0.3f;
            if (m_localPlayer.health > m_localPlayer.maxHealth)
                m_localPlayer.health = m_localPlayer.maxHealth;
            m_localPlayer.invulnTimer = 1.5f;
            m_localPlayer.secondWindCooldown = 60.0f;
            for (u32 ni = 0; ni < MAX_NOVA_FX; ni++) {
                if (!m_fx.novaFX[ni].active) {
                    m_fx.novaFX[ni] = {m_localPlayer.position, 2.0f, 0.8f, true, {1.0f, 0.9f, 0.3f}};
                    break;
                }
            }
            LOG_INFO("SECOND WIND triggered! Healed 30%%, 1.5s invuln");
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

    // Blood Nova ARMOR aura (Demonhide Cuirass) — retaliate when STRUCK: sacrifice 5% of current
    // health and erupt in the full Blood Nova (def damage + radius + red ring), on the def's 5 s
    // internal cooldown. Reads lastDamageTaken, exactly like thorns below — and like thorns it is
    // therefore inert on a CLIENT, whose lastDamageTaken is never set (Combat::applyDamageToPlayer
    // runs inside the N4-gated EnemyAI/projectile passes). The guest's nova is fired
    // authoritatively by the server (serverNetPost) and its ring arrives as a NOVA_FX event.
    if (m_localPlayer.bloodNovaCooldown > 0.0f) m_localPlayer.bloodNovaCooldown -= dt;
    if (m_armorAura == SkillId::BLOOD_NOVA && m_localPlayer.lastDamageTaken > 0.0f) {
        detonateBloodNova(m_localPlayer.position, activeNetSlot(),
                          m_localPlayer.health, m_localPlayer.bloodNovaCooldown);
    }

    // Static Charge (Capacitor Mail): being struck charges the armor; the 5th stack discharges
    // chain lightning into the attacker. Same lastDamageTaken source as Blood Nova above — and
    // like it, inert on a CLIENT (a guest's stacks tick server-side in serverNetPost; the HUD
    // adopts them from SnapPlayer.flags in clientNetPost).
    if (m_armorAura == SkillId::STATIC_CHARGE) {
        if (StaticCharge::accumulate(m_localPlayer.chargeStacks, m_localPlayer.chargeTimer,
                                     m_localPlayer.lastDamageTaken > 0.0f, dt))
            staticDischarge(m_localPlayer.position, activeNetSlot(),
                            m_localPlayer.lastDamageAttackerIdx);
    }

    // Hemophage (Hemophage Shroud): 4m life-drain aura. Damaging — NOT idempotent like the
    // burn/freeze aura timers — so host lanes run it here and remotes ONLY in serverNetPost.
    if (m_armorAura == SkillId::HEMOPHAGE)
        hemophageAuraTick(m_localPlayer.position, activeNetSlot(), m_localPlayer.hemoTickTimer,
                          m_localPlayer.health, m_localPlayer.maxHealth, dt);

    // Total thorns reflect fraction: legendary THORNS ring (20%) + the thorns_pct affix sum
    // (stored as percentage points, e.g. 15.0 → 0.15), so a thorns ring and thorns gear stack.
    // Only meaningful on a frame where the player actually took damage (lastDamageTaken > 0).
    f32 thornsFrac = (m_ringPassive == SkillId::THORNS ? 0.20f : 0.0f)
                   + m_localPlayer.thornsPctBonus * 0.01f;

    // Single pass over entities for armor aura + gravity pull + thorns
    bool needEntityPass = (m_armorAura != SkillId::NONE) ||
                          (m_ringPassive == SkillId::GRAVITY_PULL) ||
                          (thornsFrac > 0.0f && m_localPlayer.lastDamageTaken > 0.0f);
    if (needEntityPass) {
        Vec3 pPos = m_localPlayer.position;
        bool doGravity = (m_ringPassive == SkillId::GRAVITY_PULL);
        bool doThorns  = (thornsFrac > 0.0f && m_localPlayer.lastDamageTaken > 0.0f);
        f32  reflectDmg = doThorns ? m_localPlayer.lastDamageTaken * thornsFrac : 0.0f;
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
                        if (distSq < 9.0f) { ent.burnTimer = 0.5f; ent.burnDps = 2.0f; ent.burnSrcSlot = activeNetSlot(); }
                        break;
                    case SkillId::FROZEN_ORB:
                        if (distSq < 16.0f) { ent.freezeTimer = 0.5f; }
                        break;
                    // BLOOD_NOVA is deliberately absent here — it is no longer a per-entity
                    // proximity aura. It retaliates when the wearer is STRUCK (below), which is
                    // what its tooltip has always claimed. The old case applied a 1 dps / 0.5 s
                    // poison within 3 m: not a weakened Blood Nova, just a different effect
                    // wearing its name.
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

        if (doThorns && reflectDmg > 0.0f) {
            // Prefer the actual attacker (melee/boss hits stamp an entity index into
            // lastDamageAttackerIdx); fall back to the nearest enemy within 5m for sources that
            // carry no attacker entity (e.g. enemy projectiles → idx 0xFFFF). Validation mirrors
            // the dodge-through riposte (engine_init_callbacks.cpp).
            u16 atk = m_localPlayer.lastDamageAttackerIdx;
            bool hitAttacker = false;
            if (atk < MAX_ENTITIES) {
                Entity& ae = m_entities.entities[atk];
                if ((ae.flags & ENT_ACTIVE) && !(ae.flags & ENT_DEAD) && !(ae.flags & ENT_FRIENDLY)) {
                    EntityHandle h; h.index = atk; h.generation = ae.generation;
                    Combat::applyDamage(m_entities, h, reflectDmg, &pPos);
                    hitAttacker = true;
                }
            }
            if (!hitAttacker && bestThornsDist2 < 25.0f)
                Combat::applyDamage(m_entities, bestThornsH, reflectDmg, &pPos);
        }
    }
    m_localPlayer.lastDamageTaken      = 0.0f;
    m_localPlayer.lastDamageAttackerIdx = 0xFFFF;
}

// ---------------------------------------------------------------------------
// applySpellScaling — see engine.h. The one place gear spell damage becomes SkillSystem state;
// call sites: local class/boot/helmet activation, both weapon-proc paths, and both remote-cast
// paths (engine_net.cpp) — always with the CASTER's own inventory (co-op: the server's synced
// copy of the guest's gear).
// ---------------------------------------------------------------------------
void Engine::applySpellScaling(const PlayerInventory& inv, f32 baseMult, f32 shrinePct) {
    // Gear % and the Shrine of Sorcery's % stack additively, then fold into the one multiplier
    // every skill damage site reads.
    SkillSystem::setClassDamageMult(baseMult *
        (1.0f + (Inventory::spellDamagePct(inv) + shrinePct) / 100.0f));
    SkillSystem::setSpellDamageFlat(Inventory::spellDamageFlat(inv));
}

// ---------------------------------------------------------------------------
// staticDischarge — fire a full chain lightning at whoever hit the wearer (Capacitor Mail's
// 5-stack discharge; Thunderwall's perfect-block riposte reuses it). Server/SP only: damage
// is authoritative (N5). Falls back to the nearest living hostile within 5m when the hit had
// no source entity (projectiles/AoE stamp attackerIdx 0xFFFF), thorns-style.
// ---------------------------------------------------------------------------
void Engine::staticDischarge(Vec3 pos, u8 wearerSlot, u16 attackerIdx) {
    if (m_netRole == NetRole::CLIENT) return;

    Vec3 target{};
    bool found = false;
    if (attackerIdx < MAX_ENTITIES) {
        const Entity& ae = m_entities.entities[attackerIdx];
        if ((ae.flags & ENT_ACTIVE) && !(ae.flags & ENT_DEAD) && !(ae.flags & ENT_FRIENDLY)) {
            target = ae.position;
            found = true;
        }
    }
    if (!found) {
        // Sourceless hits (projectiles/AoE stamp attackerIdx 0xFFFF) are THE ranged case, and
        // the shooter is far by definition — a 5m fallback made Thunderwall/Capacitor fizzle
        // against every archer/caster. Scan out to chain lightning's own first-hop range (15m)
        // so a perfect-blocked arrow answers the archer that fired it (nearest hostile is the
        // shooter often enough, and the bolt chains onward regardless).
        f32 best = 225.0f;  // 15m squared
        for (u32 a = 0; a < m_entities.activeCount; a++) {
            const Entity& e = m_entities.entities[m_entities.activeList[a]];
            if ((e.flags & ENT_DEAD) || (e.flags & ENT_FRIENDLY)) continue;
            if (e.enemyType == EnemyType::PROP) continue;
            Vec3 d = e.position - pos;
            f32 d2 = d.x * d.x + d.z * d.z;
            if (d2 < best) { best = d2; target = e.position; found = true; }
        }
    }
    if (!found) return;   // nothing within lightning range to answer — the charge is spent

    const SkillDef* def = SkillSystem::findSkillDef(m_skillDefs, m_skillDefCount, SkillId::CHAIN_LIGHTNING);
    if (!def) return;
    Vec3 origin = pos + Vec3{0, 1.2f, 0};   // chest height, so the first-hop cone sees the attacker
    Vec3 dir    = normalize(target - origin);
    // Neutral scaling: a proc must not ride whatever class-damage/skill-power multipliers the
    // last class cast left in the SkillSystem statics — and it must RESTORE them afterwards,
    // because s_skillPower is not per-cast-only state: updateOrbProjectiles reads it every tick
    // for live Frozen Orb shards, so leaving it zeroed would rescale someone's in-flight orb.
    // Kill credit rides setAttackingPlayer (the projectile.cpp save/restore pattern).
    const f32 prevPower = SkillSystem::getSkillPower();
    const f32 prevMult  = SkillSystem::getClassDamageMult();
    const f32 prevFlat  = SkillSystem::getSpellDamageFlat();
    SkillSystem::setClassDamageMult(1.0f);
    SkillSystem::setSkillPower(0.0f);
    SkillSystem::setSpellDamageFlat(0.0f);   // procs don't ride the wearer's spell-damage gear
    u8 prev = Combat::getAttackingPlayer();
    Combat::setAttackingPlayer(wearerSlot);
    fireChainLightning(origin, dir, def, m_level.grid, m_entities);
    Combat::setAttackingPlayer(prev);
    SkillSystem::setSpellDamageFlat(prevFlat);
    SkillSystem::setSkillPower(prevPower);
    SkillSystem::setClassDamageMult(prevMult);
}

// ---------------------------------------------------------------------------
// hemophageAuraTick — Hemophage Shroud: enemies within 4m bleed 3 damage every 0.5s (6 dps)
// to the wearer, who heals for the total. Ticked (not per-frame) to bound applyDamage calls;
// heals are ratio-safe on the wire (health up, maxHealth untouched — no HP-bar lurch).
// Server/SP only: the client's entity pool is a discarded ghost (N5).
// ---------------------------------------------------------------------------
void Engine::hemophageAuraTick(Vec3 pos, u8 wearerSlot, f32& tickTimer, f32& health,
                               f32 maxHealth, f32 dt) {
    if (m_netRole == NetRole::CLIENT) return;
    tickTimer -= dt;
    if (tickTimer > 0.0f) return;
    tickTimer = 0.5f;

    constexpr f32 DRAIN_PER_TICK = 3.0f;   // x2 ticks/s = 6 dps per enemy in the aura
    u8 prev = Combat::getAttackingPlayer();
    Combat::setAttackingPlayer(wearerSlot);   // drain kills credit (and reserve loot to) the wearer
    f32 drained = 0.0f;
    for (u32 a = 0; a < m_entities.activeCount; a++) {
        u32 idx = m_entities.activeList[a];
        Entity& ent = m_entities.entities[idx];
        if ((ent.flags & ENT_DEAD) || (ent.flags & ENT_FRIENDLY)) continue;
        if (ent.enemyType == EnemyType::PROP) continue;
        Vec3 d = ent.position - pos;
        if (d.x * d.x + d.z * d.z > 16.0f) continue;   // 4m XZ radius, squared
        EntityHandle h;
        h.index      = static_cast<u16>(idx);
        h.generation = ent.generation;
        Combat::applyDamage(m_entities, h, DRAIN_PER_TICK, &pos);
        drained += DRAIN_PER_TICK;
    }
    Combat::setAttackingPlayer(prev);
    if (drained > 0.0f && health > 0.0f) health = fminf(health + drained, maxHealth);
}
