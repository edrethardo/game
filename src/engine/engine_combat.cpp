// Engine module — see engine.h for class definition.
// Split from engine.cpp for manageability. All methods are Engine:: members.

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
#include "world/raycast.h"
#include "renderer/particles.h"
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
// Shared statics defined in engine.cpp
extern Engine* s_engine;
extern FrameAllocator s_frameAllocator;
extern bool s_firstKillDropGiven;


// ---------------------------------------------------------------------------
// Weapon fire (singleplayer — unchanged from Phase 3)
// ---------------------------------------------------------------------------
void Engine::handleWeaponFire(f32 dt) {
    // (L8) Credit this player's melee/hitscan kills (and stamp the slot onto any projectile
    // fired this pass). tickSharedSystems resets it to 0xFF before AI/projectiles/skills, so
    // only this player's direct, synchronous weapon kills are attributed here.
    Combat::setAttackingPlayer(activeNetSlot());                // local player's net slot — host=0, client=its assigned slot
    WeaponState& ws = m_players[activeNetSlot()].weaponState;   // local player's net slot
    ws.cooldownTimer -= dt;
    if (ws.cooldownTimer < 0.0f) ws.cooldownTimer = 0.0f;

    // Build effective weapon stats from equipped item
    const ItemInstance& eqWpn = m_inventories[m_localPlayerIndex].equipped[static_cast<u32>(ItemSlot::WEAPON)];
    WeaponDef wpn;
    if (!isItemEmpty(eqWpn)) {
        wpn = Inventory::getWeaponFromItem(m_inventories[m_localPlayerIndex],
                                            m_itemDefs, eqWpn);
    } else {
        wpn = m_weaponDefs[ws.currentWeapon];
    }

    // Detect weapon switch — reset clip on change
    u16 curDefId = isItemEmpty(eqWpn) ? 0xFFFF : eqWpn.defId;
    if (curDefId != ws.lastWeaponDef) {
        ws.lastWeaponDef = curDefId;
        ws.currentClip = wpn.clipSize;
        ws.reloading = false;
        ws.reloadTimer = 0.0f;
    }

    // Tick reload timer (must tick every frame, not just on fire)
    if (ws.reloading) {
        ws.reloadTimer -= dt;
        if (ws.reloadTimer <= 0.0f) {
            ws.currentClip = wpn.clipSize;
            ws.reloading = false;
        }
    }

    // Throwaway legendary: throw weapon as projectile on reload
    auto throwWeaponOnReload = [&]() {
        if (isItemEmpty(eqWpn)) return;
        if (m_itemDefs[eqWpn.defId].legendarySkillId != SkillId::THROWAWAY) return;
        Vec3 eyePos = m_localPlayer.position + Vec3{0, m_localPlayer.eyeHeight, 0};
        Vec3 forward = m_localPlayer.forward;
        Vec3 right = normalize(Vec3{-forward.z, 0, forward.x});
        Vec3 spawnPos = eyePos + forward * 0.5f + right * 0.3f + Vec3{0, -0.15f, 0};
        f32 throwDmg = wpn.damage * ws.currentClip * 0.5f; // damage scales with remaining ammo
        if (throwDmg < wpn.damage) throwDmg = wpn.damage;  // minimum 1 shot worth
        u16 projIdx = ProjectileSystem::spawn(m_projectiles, spawnPos,
            forward, 20.0f, throwDmg, 0.2f, 3.0f, true, PROJ_SPLASH);
        if (projIdx != 0xFFFF) {
            m_projectiles.projectiles[projIdx].meshId = m_itemDefs[eqWpn.defId].meshId;
            m_projectiles.projectiles[projIdx].splashRadius = 2.0f;
            m_projectiles.projectiles[projIdx].splashDamage = throwDmg * 0.5f;
        }
        m_viewmodelState.attackAnimT = 0.3f;
    };

    // Manual reload — reload if clip not full and not already reloading
    if (Input::isActionPressed(GameAction::RELOAD) && wpn.clipSize > 0 &&
        !ws.reloading && ws.currentClip < wpn.clipSize) {
        ws.reloading = true;
        ws.reloadTimer = wpn.reloadTime;
        AudioSystem::play(SfxId::RELOAD);
        throwWeaponOnReload();
    }

    // Auto-reload when clip is empty (triggers immediately, no need to click)
    if (wpn.clipSize > 0 && ws.currentClip == 0 && !ws.reloading) {
        ws.reloading = true;
        ws.reloadTimer = wpn.reloadTime;
        AudioSystem::play(SfxId::RELOAD);
        throwWeaponOnReload();
    }

    // Can't fire while reloading
    if (ws.reloading) return;

    if (!Input::isActionDown(GameAction::FIRE)) return;
    if (ws.cooldownTimer > 0.0f) return;

    // A shot is committed — attacking ends the floor-start calm window immediately
    // so the companions stop waiting and the world springs to life with the player.
    m_spawnCalmTimer = 0.0f;

    // Track subtype for projectile flags (molotov/wand detection)
    const ItemInstance* qbItem = &eqWpn;
    ws.cooldownTimer = wpn.cooldown;

    // Wanderer adrenaline attack speed bonus: -10% cooldown per stack, capped at -50%
    if (m_localPlayer.dodgeState.counterStacks > 0) {
        f32 atkSpeedMult = 1.0f - (m_localPlayer.dodgeState.counterStacks * 0.10f);
        if (atkSpeedMult < 0.5f) atkSpeedMult = 0.5f;
        ws.cooldownTimer *= atkSpeedMult;
    }

    // Consume ammo — auto-reload will kick in next frame if this empties the clip
    if (wpn.clipSize > 0 && ws.currentClip > 0) {
        ws.currentClip--;
    }

    // Class passive: +20% damage with preferred weapon type
    {
        const ClassDef& cls = kClassDefs[static_cast<u32>(m_playerClass)];
        if (wpn.type == cls.preferredWeapon) {
            wpn.damage *= 1.2f;
        }
    }
    // Shadow Dance: 2× damage on all weapon attacks while active
    if (m_localPlayer.shadowDanceTimer > 0.0f) {
        wpn.damage *= 2.0f;
    }
    // Berserker ring: +1% damage per 1% missing HP
    if (m_ringPassive == SkillId::BERSERKER) {
        f32 missingPct = 1.0f - m_localPlayer.health / m_localPlayer.maxHealth;
        wpn.damage *= (1.0f + missingPct);
    }
    // Soul Harvest ring: +3% damage per stack
    if (m_localPlayer.soulHarvestStacks > 0) {
        wpn.damage *= (1.0f + m_localPlayer.soulHarvestStacks * 0.03f);
    }

    Vec3 eyePos = m_localPlayer.position + Vec3{0, m_localPlayer.eyeHeight, 0};
    Vec3 forward = m_localPlayer.forward;

    AttackResult result;
    switch (wpn.type) {
    case WeaponType::MELEE: {
        // Crit is now rolled inside Combat::fireMelee from weapon.critChance (set in
        // buildWeaponDef in item.cpp). No per-subtype crit logic needed here.
        WeaponSubtype melSub = WeaponSubtype::NONE;
        if (qbItem && !isItemEmpty(*qbItem))
            melSub = m_itemDefs[qbItem->defId].weaponSubtype;
        result = Combat::fireMelee(wpn, eyePos, forward, m_entities);
        m_localPlayer.hitShakeTimer = fmaxf(m_localPlayer.hitShakeTimer, 0.03f);

        // Non-dagger cleave: 5% chance to hit all enemies in a wide 360° arc
        if (melSub != WeaponSubtype::DAGGER && melSub != WeaponSubtype::NONE &&
            result.hitEntity && (std::rand() % 100) < 5) {
            WeaponDef cleaveWpn = wpn;
            cleaveWpn.coneAngleDeg = 360.0f;
            cleaveWpn.damage *= 0.5f; // cleave deals half damage
            Combat::fireMelee(cleaveWpn, eyePos, forward, m_entities); // cleave is never a crit
        }
    } break;
    case WeaponType::HITSCAN:
        // Overcharged Magazine: 3× damage + penetrating shot with beam trail.
        // Key by NET slot so a remote Marksman's overcharge buffs THEIR gun, not the host's (H5).
        if (SkillSystem::isOvercharged(activeNetSlot())) {
            wpn.damage *= 3.0f;
            ws.cooldownTimer *= 0.7f; // 30% attack speed bonus during overcharge
            SkillSystem::consumeOverchargeShot(activeNetSlot());
            // Penetrating hitscan — hit ALL enemies in narrow cone
            EntityHandle oHits[MAX_ENTITIES];
            f32          oDists[MAX_ENTITIES];
            u32 oCnt = CombatQuery::queryConeSorted(
                m_entities, eyePos, forward, cosf(radians(1.0f)), wpn.range,
                oHits, oDists, MAX_ENTITIES);
            bool gotKill = false;
            for (u32 oi = 0; oi < oCnt; oi++) {
                Entity* oe = handleGet(m_entities, oHits[oi]);
                if (!oe || (oe->flags & ENT_DEAD) || (oe->flags & ENT_FRIENDLY)) continue;
                f32 hpBefore = oe->health;
                Combat::applyDamage(m_entities, oHits[oi], wpn.damage);
                if (hpBefore > 0.0f && oe->health <= 0.0f) gotKill = true;
                ParticleSystem::spawnDebris(m_particles, oe->position, 3);
            }
            // Instant full reload on kill
            if (gotKill) { ws.currentClip = wpn.clipSize; ws.reloading = false; }
            // Beam trail
            RayHit bWall = Raycast::cast(m_level.grid, eyePos, forward, wpn.range);
            Vec3 bEnd = bWall.hit ? (eyePos + forward * bWall.distance)
                                  : (eyePos + forward * wpn.range);
            for (u32 bfx = 0; bfx < MAX_BEAM_FX; bfx++) {
                if (!m_fx.beamFX[bfx].active) {
                    m_fx.beamFX[bfx] = {eyePos, bEnd, {1.0f, 0.7f, 0.2f}, 0.3f, true};
                    break;
                }
            }
            m_localPlayer.hitShakeTimer = fmaxf(m_localPlayer.hitShakeTimer, 0.05f);
            result.hitEntity = (oCnt > 0);
        } else {
            result = Combat::fireHitscan(wpn, eyePos, forward, m_level.grid, m_entities);
            m_localPlayer.hitShakeTimer = fmaxf(m_localPlayer.hitShakeTimer, 0.05f);
            if (result.hitEntity || result.hitWorld) {
                m_lastCombatHit.hit      = true;
                m_lastCombatHit.position = result.hitPosition;
                m_lastCombatHit.normal   = result.hitNormal;
                m_lastCombatHit.distance = result.hitDistance;
                m_lastCombatHit.type     = result.hitEntity ? CombatHit::ENTITY : CombatHit::WORLD;
                // Spawn impact spark at hit position
                for (u32 fx = 0; fx < MAX_IMPACT_FX; fx++) {
                    if (!m_fx.impactFX[fx].active) {
                        m_fx.impactFX[fx] = {result.hitPosition, result.hitNormal,
                                          0.3f, true, result.hitEntity};
                        break;
                    }
                }
                // R7-5: replicate the host's own hitscan impact to clients so they see
                // the spark (mirrors the remote-player path in handleWeaponFireForPlayer).
                // SERVER-gated: singleplayer/split-screen (NONE) has no peers to notify.
                if (m_netRole == NetRole::SERVER) {
                    u8 evBuf[sizeof(PacketHeader) + 26]; // eventType(1) + pos(12) + normal(12) + hitEntity(1)
                    PacketHeader* evHdr = reinterpret_cast<PacketHeader*>(evBuf);
                    evHdr->type = NetPacketType::SV_EVENT;
                    evHdr->flags = 0;
                    evHdr->seq = 0;
                    u32 off = sizeof(PacketHeader);
                    evBuf[off++] = static_cast<u8>(NetEventType::HITSCAN_IMPACT);
                    std::memcpy(evBuf + off, &result.hitPosition.x, 4); off += 4;
                    std::memcpy(evBuf + off, &result.hitPosition.y, 4); off += 4;
                    std::memcpy(evBuf + off, &result.hitPosition.z, 4); off += 4;
                    std::memcpy(evBuf + off, &result.hitNormal.x, 4);   off += 4;
                    std::memcpy(evBuf + off, &result.hitNormal.y, 4);   off += 4;
                    std::memcpy(evBuf + off, &result.hitNormal.z, 4);   off += 4;
                    evBuf[off++] = result.hitEntity ? 1 : 0;
                    Net::broadcastReliable(evBuf, off);
                }
            }
        }
        break;
    case WeaponType::PROJECTILE: {
        bool isMolotov = qbItem && !isItemEmpty(*qbItem) &&
                         m_itemDefs[qbItem->defId].weaponSubtype == WeaponSubtype::MOLOTOV;
        bool isWand = qbItem && !isItemEmpty(*qbItem) &&
                      m_itemDefs[qbItem->defId].weaponSubtype == WeaponSubtype::WAND;
        bool isBow = qbItem && !isItemEmpty(*qbItem) &&
                     m_itemDefs[qbItem->defId].weaponSubtype == WeaponSubtype::BOW;
        bool isCrossbow = qbItem && !isItemEmpty(*qbItem) &&
                          m_itemDefs[qbItem->defId].weaponSubtype == WeaponSubtype::CROSSBOW;

        // Spawn projectile at the weapon tip position (matches viewmodel)
        Vec3 right = normalize(Vec3{-forward.z, 0, forward.x});
        Vec3 spawnPos = eyePos + forward * 0.8f;
        if (isWand) {
            // Wand/staff tip: offset right and down, not too far forward for close hits
            spawnPos = eyePos + forward * 0.7f + right * 0.25f + Vec3{0, -0.2f, 0};
        } else if (isBow || isCrossbow || isMolotov) {
            // Offset right and slightly down to match weapon hand position
            spawnPos = eyePos + forward * 0.5f + right * 0.3f + Vec3{0, -0.15f, 0};
        }

        u16 projIdx;
        if (isMolotov) {
            projIdx = Combat::fireProjectile(wpn, spawnPos, forward, m_projectiles,
                                              9.8f, 3.0f, wpn.damage * 0.6f);
        } else {
            // Wands get spark visual; void weapons get purple tint via PROJ_VOID flag
            bool isVoidWand = isWand && m_weaponProc == SkillId::VOID_ZONE;
            u8 flags = isVoidWand ? PROJ_VOID : (isWand ? PROJ_SPARK : 0);
            projIdx = Combat::fireProjectile(wpn, spawnPos, forward, m_projectiles, flags);
        }
        // Assign correct mesh to projectile based on weapon subtype
        if (projIdx != 0xFFFF && qbItem && !isItemEmpty(*qbItem)) {
            WeaponSubtype sub = m_itemDefs[qbItem->defId].weaponSubtype;
            if (sub == WeaponSubtype::BOW) {
                m_projectiles.projectiles[projIdx].meshId = findMeshByName("arrow");
            } else if (sub == WeaponSubtype::CROSSBOW) {
                m_projectiles.projectiles[projIdx].meshId = findMeshByName("bolt");
            } else if (sub == WeaponSubtype::THROWING_KNIFE || sub == WeaponSubtype::MOLOTOV) {
                u8 wpnMesh = m_itemDefs[qbItem->defId].meshId;
                if (wpnMesh > 0) m_projectiles.projectiles[projIdx].meshId = wpnMesh;
            }
        }
        // Assign projectile light color based on weapon subtype
        if (projIdx != 0xFFFF) {
            Projectile& proj = m_projectiles.projectiles[projIdx];
            if (isMolotov)             proj.lightColor = {1.0f, 0.5f, 0.1f}; // fire
            else if (isWand)           proj.lightColor = {0.4f, 0.6f, 1.0f}; // arcane blue
            else if (proj.projFlags & PROJ_VOID) proj.lightColor = {0.4f, 0.0f, 0.8f}; // purple
        }
        result.didFire = true;
        m_localPlayer.hitShakeTimer = fmaxf(m_localPlayer.hitShakeTimer, 0.02f);
    } break;
    }

    // Viewmodel animation per weapon type
    if (wpn.type == WeaponType::MELEE) {
        m_viewmodelState.attackAnimT = 0.3f;
    } else if (wpn.type == WeaponType::HITSCAN) {
        m_viewmodelState.attackAnimT = 0.2f; // shorter recoil snap
        m_viewmodelState.fireShakeTimer = 0.1f;
    } else {
        // Projectile: throw arc animation + slight shake on release
        m_viewmodelState.attackAnimT = 0.3f;
        m_viewmodelState.fireShakeTimer = 0.08f;
    }
    m_viewmodelState.recoilKick += wpn.recoilKick * 1.5f;

    // Muzzle flash dynamic light — color depends on weapon type
    {
        Vec3 eyePos = m_localPlayer.position + Vec3{0, m_localPlayer.eyeHeight, 0};
        Vec3 flashCol = {0, 0, 0};
        if (wpn.type == WeaponType::MELEE)
            flashCol = {1.0f, 0.9f, 0.7f};   // warm white
        else if (wpn.type == WeaponType::HITSCAN)
            flashCol = {1.0f, 0.7f, 0.3f};   // orange muzzle flash
        else if (wpn.type == WeaponType::PROJECTILE) {
            // Projectile color depends on subtype/skill
            if (!isItemEmpty(eqWpn)) {
                WeaponSubtype st = m_itemDefs[eqWpn.defId].weaponSubtype;
                if (st == WeaponSubtype::WAND)       flashCol = {0.4f, 0.6f, 1.0f}; // blue arcane
                else if (st == WeaponSubtype::MOLOTOV) flashCol = {1.0f, 0.5f, 0.1f}; // fire
                else flashCol = {0.8f, 0.8f, 0.6f}; // dim warm for arrows/knives
            }
        }
        if (flashCol.x > 0.0f || flashCol.y > 0.0f || flashCol.z > 0.0f)
            spawnDynamicLight(eyePos + m_localPlayer.forward * 0.5f, flashCol, 0.1f);
    }

    // Play weapon fire sound based on subtype
    if (!isItemEmpty(eqWpn)) {
        switch (m_itemDefs[eqWpn.defId].weaponSubtype) {
            case WeaponSubtype::SWORD:   AudioSystem::play(SfxId::WEAPON_SWORD, 0.5f); break;
            case WeaponSubtype::DAGGER:  AudioSystem::play(SfxId::WEAPON_DAGGER, 0.5f); break;
            case WeaponSubtype::AXE:     AudioSystem::play(SfxId::WEAPON_AXE, 0.5f); break;
            case WeaponSubtype::CLAYMORE:AudioSystem::play(SfxId::WEAPON_CLAYMORE, 0.5f); break;
            case WeaponSubtype::PISTOL:  AudioSystem::play(SfxId::WEAPON_PISTOL, 0.5f); break;
            case WeaponSubtype::SMG:     AudioSystem::play(SfxId::WEAPON_SMG, 0.5f); break;
            case WeaponSubtype::CARBINE: AudioSystem::play(SfxId::WEAPON_CARBINE, 0.5f); break;
            case WeaponSubtype::REVOLVER:AudioSystem::play(SfxId::WEAPON_REVOLVER, 0.5f); break;
            case WeaponSubtype::BOW:     AudioSystem::play(SfxId::WEAPON_BOW, 0.5f); break;
            case WeaponSubtype::CROSSBOW:AudioSystem::play(SfxId::WEAPON_CROSSBOW, 0.5f); break;
            case WeaponSubtype::THROWING_KNIFE: AudioSystem::play(SfxId::WEAPON_THROW, 0.5f); break;
            case WeaponSubtype::MOLOTOV: AudioSystem::play(SfxId::WEAPON_MOLOTOV, 0.5f); break;
            case WeaponSubtype::WAND:    AudioSystem::play(SfxId::WEAPON_WAND, 0.5f); break;
            default: break;
        }
    }
    // Subtle screen shake on weapon fire
    f32 fireShake = (wpn.type == WeaponType::HITSCAN) ? 0.05f :
                    (wpn.type == WeaponType::MELEE)   ? 0.03f : 0.02f;
    m_localPlayer.hitShakeTimer = fmaxf(m_localPlayer.hitShakeTimer, fireShake);
    if (result.hitEntity) {
        m_hitMarkerTimer = 0.2f;
        AudioSystem::play(SfxId::HIT_MELEE); // generic hit sound — subtype doesn't matter for impact
    }

    // --- Ring on-hit passives ---
    if (result.hitEntity && m_ringPassive != SkillId::NONE) {
        // Life Steal: heal 5% of damage dealt
        if (m_ringPassive == SkillId::LIFE_STEAL) {
            f32 heal = wpn.damage * 0.05f;
            m_localPlayer.health += heal;
            if (m_localPlayer.health > m_localPlayer.maxHealth)
                m_localPlayer.health = m_localPlayer.maxHealth;
        }
        // Phase Strike is now on-kill (smoke bomb) — see death callback
    }

    // Affix life-on-hit: heal percentage of damage dealt (works independently of ring passive)
    if (result.hitEntity) {
        f32 loh = m_inventories[m_localPlayerIndex].bonusLifeOnHit;
        if (loh > 0.0f) {
            f32 heal = wpn.damage * loh;
            m_localPlayer.health = fminf(m_localPlayer.health + heal, m_localPlayer.maxHealth);
        }
    }

    // Weapon legendary on-hit proc — % chance to trigger skill at hit position
    if (result.hitEntity && m_weaponProc != SkillId::NONE) {
        u32 procRoll = static_cast<u32>(std::rand()) % 100;
        u32 procChance = 20; // default 20%
        if (m_weaponProc == SkillId::FROZEN_ORB)    procChance = 15;
        if (m_weaponProc == SkillId::CHAIN_LIGHTNING) procChance = 25;
        if (m_weaponProc == SkillId::METEOR_STRIKE)  procChance = 10;
        if (m_weaponProc == SkillId::BLOOD_NOVA)     procChance = 20;
        if (m_weaponProc == SkillId::VOID_ZONE)      procChance = 5;
        if (m_weaponProc == SkillId::ARC_FIRE)       procChance = 20;

        if (procRoll < procChance) {
            Vec3 procPos = result.hitPosition;
            const SkillDef* sd = SkillSystem::findSkillDef(m_skillDefs, m_skillDefCount, m_weaponProc);
            // ARC_FIRE uses weapon stats directly, no SkillDef needed
            if (sd || m_weaponProc == SkillId::ARC_FIRE) {
                // Fire the skill effect at the hit position
                switch (m_weaponProc) {
                    case SkillId::FROZEN_ORB: {
                        Vec3 dir = m_localPlayer.forward;
                        u16 orbIdx = ProjectileSystem::spawn(m_projectiles, procPos, dir,
                            sd->projectileSpeed, sd->damage, sd->radius, sd->duration, true);
                        if (orbIdx != 0xFFFF) m_projectiles.projectiles[orbIdx].projFlags = PROJ_ORB;
                    } break;
                    case SkillId::CHAIN_LIGHTNING: {
                        // Use the real chain lightning with item-level-scaled bounces.
                        // Bounces scale from 3 (level 1) to 20 (level 50).
                        const ItemInstance& wpn2 = m_inventories[m_localPlayerIndex].equipped[static_cast<u32>(ItemSlot::WEAPON)];
                        u8 itemLvl = wpn2.itemLevel > 0 ? wpn2.itemLevel : 1;
                        // Temporarily override SkillDef bounces for this proc
                        SkillDef procDef = *sd;
                        procDef.bounces = static_cast<u8>(3 + (itemLvl - 1) * 17 / 49);
                        // Fire from hit position toward nearest enemy
                        SkillState tempSS;
                        tempSS.activeSkill = SkillId::CHAIN_LIGHTNING;
                        tempSS.cooldownTimer = 0.0f;
                        tempSS.energy = 999.0f;
                        tempSS.maxEnergy = 999.0f;
                        // Scale by weapon item level — proc skills use base class damage (1.0)
                        { u8 lvl = m_inventories[m_localPlayerIndex].equipped[static_cast<u32>(ItemSlot::WEAPON)].itemLevel;
                          SkillSystem::setSkillPower(lvl > 1 ? static_cast<f32>(lvl - 1) / 149.0f : 0.0f); }
                        SkillSystem::setClassDamageMult(1.0f);
                        Vec3 dir = m_localPlayer.forward;
                        SkillSystem::tryActivate(tempSS, &procDef, 1,
                            procPos, dir, m_localPlayer.yaw,
                            m_projectiles, m_entities, m_level.grid, m_localPlayer);
                    } break;
                    case SkillId::METEOR_STRIKE: {
                        // Drop a meteor on the hit position
                        extern PendingMeteor s_meteors[MAX_PENDING_METEORS];
                        for (u32 mi = 0; mi < MAX_PENDING_METEORS; mi++) {
                            if (!s_meteors[mi].active) {
                                s_meteors[mi] = {procPos, sd->damage, sd->radius, sd->delay, true};
                                break;
                            }
                        }
                    } break;
                    case SkillId::BLOOD_NOVA: {
                        // Nova centered on hit target
                        EntityHandle hits[MAX_ENTITIES];
                        f32 dists[MAX_ENTITIES];
                        u32 hitCount = CombatQuery::queryConeSorted(
                            m_entities, procPos, {0,0,-1}, -1.0f, sd->radius,
                            hits, dists, MAX_ENTITIES);
                        for (u32 h = 0; h < hitCount; h++) {
                            Combat::applyDamage(m_entities, hits[h], sd->damage * 0.5f);
                        }
                        // Visual
                        for (u32 ni = 0; ni < MAX_NOVA_FX; ni++) {
                            if (!m_fx.novaFX[ni].active) {
                                m_fx.novaFX[ni] = {procPos, sd->radius, 0.6f, true, {1.0f, 0.15f, 0.1f}};
                                break;
                            }
                        }
                    } break;
                    case SkillId::VOID_ZONE: {
                        // Void zone: flat damage + 60% of target's missing HP
                        if (m_lastCombatHit.type == CombatHit::ENTITY) {
                            Entity* ve = handleGet(m_entities, m_lastCombatHit.entityHandle);
                            if (ve && !(ve->flags & ENT_DEAD)) {
                                f32 missingHp = ve->maxHealth - ve->health;
                                f32 voidDmg = sd->damage + missingHp * 0.6f;
                                Combat::applyDamage(m_entities, m_lastCombatHit.entityHandle, voidDmg);
                            }
                        }
                        // Dark purple void nova visual
                        for (u32 ni = 0; ni < MAX_NOVA_FX; ni++) {
                            if (!m_fx.novaFX[ni].active) {
                                m_fx.novaFX[ni] = {procPos, sd->radius, 0.8f, true, {0.3f, 0.1f, 0.5f}};
                                break;
                            }
                        }
                    } break;
                    case SkillId::ARC_FIRE: {
                        // Blazing Arc: spawn fire scorch zones across the melee swing arc
                        f32 arcDps = wpn.damage * 0.3f; // 30% weapon damage as burn
                        f32 halfAngle = wpn.coneAngleDeg * 0.5f * 3.14159f / 180.0f;
                        Vec3 playerPos = m_localPlayer.position;
                        f32 yaw = m_localPlayer.yaw;
                        // Spawn 5 scorch zones in a fan from -halfAngle to +halfAngle
                        for (u32 fi = 0; fi < 5; fi++) {
                            f32 t = (fi / 4.0f) * 2.0f - 1.0f; // -1 to +1
                            f32 angle = yaw + t * halfAngle;
                            Vec3 dir = {-sinf(angle), 0.0f, -cosf(angle)};
                            Vec3 zonePos = playerPos + dir * wpn.range * 0.8f;
                            // Find a free scorch slot
                            for (u32 si = 0; si < MAX_SCORCH; si++) {
                                if (!m_fx.scorchZones[si].active) {
                                    m_fx.scorchZones[si] = {zonePos, 1.0f, 1.5f, arcDps, true};
                                    break;
                                }
                            }
                            // Fire visual at each zone
                            for (u32 fxi = 0; fxi < MAX_FIRE_FX; fxi++) {
                                if (!m_fx.fireFX[fxi].active) {
                                    m_fx.fireFX[fxi] = {zonePos, 1.5f, true};
                                    break;
                                }
                            }
                        }
                    } break;
                    default: break;
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Weapon fire for any NetPlayer (server-authoritative)
// ---------------------------------------------------------------------------
void Engine::handleWeaponFireForPlayer(NetPlayer& np, f32 dt) {
    Combat::setAttackingPlayer(np.slotIndex); // (L8) credit this remote player's kills
    WeaponState& ws = np.weaponState;
    ws.cooldownTimer -= dt;
    if (ws.cooldownTimer < 0.0f) ws.cooldownTimer = 0.0f;

    const NetInput* input = Server::getInputBuffer(np.slotIndex).getLatest();
    if (!input) return;

    // Build effective weapon from equipped item
    const ItemInstance& eqWpn = m_inventories[np.slotIndex].equipped[static_cast<u32>(ItemSlot::WEAPON)];
    WeaponDef wpn;
    if (!isItemEmpty(eqWpn)) {
        wpn = Inventory::getWeaponFromItem(m_inventories[np.slotIndex], m_itemDefs, eqWpn);
    } else {
        wpn = m_weaponDefs[ws.currentWeapon];
    }

    // Detect weapon switch — reset clip
    u16 curDefId = isItemEmpty(eqWpn) ? 0xFFFF : eqWpn.defId;
    if (curDefId != ws.lastWeaponDef) {
        ws.lastWeaponDef = curDefId;
        ws.currentClip = wpn.clipSize;
        ws.reloading = false;
        ws.reloadTimer = 0.0f;
    }

    // Tick reload timer
    if (ws.reloading) {
        ws.reloadTimer -= dt;
        if (ws.reloadTimer <= 0.0f) {
            ws.currentClip = wpn.clipSize;
            ws.reloading = false;
        }
    }

    // Manual reload (R key)
    if ((input->extFlags & INPUT_EX_RELOAD) && wpn.clipSize > 0 &&
        !ws.reloading && ws.currentClip < wpn.clipSize) {
        ws.reloading = true;
        ws.reloadTimer = wpn.reloadTime;
    }

    // Auto-reload on empty clip
    if (wpn.clipSize > 0 && ws.currentClip == 0 && !ws.reloading) {
        ws.reloading = true;
        ws.reloadTimer = wpn.reloadTime;
    }

    // Can't fire while reloading
    if (ws.reloading) return;

    // Potion: handled by the caller (serverNetPre in engine_net.cpp) with the remote's own
    // per-player cooldown (NetPlayer::potionCooldown, decremented in serverNetPost), right
    // after this call — so there's nothing to do here (L7: the old per-player-cooldown TODO
    // was already implemented there; this stub is removed to avoid double-applying).

    if (!(input->moveFlags & INPUT_FIRE)) return;
    if (ws.cooldownTimer > 0.0f) return;

    ws.cooldownTimer = wpn.cooldown;

    // Consume ammo
    if (wpn.clipSize > 0 && ws.currentClip > 0) {
        ws.currentClip--;
    }

    // Class damage bonus — use remote player's class, not host's
    PlayerClass remoteClass = np.playerClass;
    if (static_cast<u32>(remoteClass) < static_cast<u32>(PlayerClass::CLASS_COUNT)) {
        const ClassDef& cls = kClassDefs[static_cast<u32>(remoteClass)];
        if (wpn.type == cls.preferredWeapon) {
            wpn.damage *= 1.2f;
        }
    }

    // (M-3) Soul Harvest ring: +3% damage per stack (mirrors the host's gun bonus at engine_combat.cpp:182).
    // Without this the M8 stack credit on a remote was wired but had no offensive effect.
    if (np.soulHarvestStacks > 0 && np.soulHarvestTimer > 0.0f) {
        wpn.damage *= (1.0f + np.soulHarvestStacks * 0.03f);
    }
    // Shadow Dance: 2× damage on all weapon attacks while active (mirrors host at :173).
    // Without this the Wanderer death-preamble credit landed on np.shadowDanceTimer but the
    // remote's gun fire had no damage bonus — same M-3 class of bug.
    if (np.shadowDanceTimer > 0.0f) {
        wpn.damage *= 2.0f;
    }

    // Berserker ring: +1% damage per 1% missing HP
    if (np.ringPassive == SkillId::BERSERKER) {
        f32 missingPct = 1.0f - np.health / np.maxHealth;
        wpn.damage *= (1.0f + missingPct);
    }

    Vec3 eyePos = np.eyePos();
    Vec3 forward = normalize(Vec3{
        -sinf(np.yaw) * cosf(np.pitch),
         sinf(np.pitch),
        -cosf(np.yaw) * cosf(np.pitch)
    });

    AttackResult result;
    switch (wpn.type) {
    case WeaponType::MELEE: {
        // Crit is now rolled inside Combat::fireMelee from weapon.critChance (set in
        // buildWeaponDef in item.cpp). No per-subtype crit logic needed here.
        WeaponSubtype sub = WeaponSubtype::NONE;
        if (!isItemEmpty(eqWpn)) sub = m_itemDefs[eqWpn.defId].weaponSubtype;
        result = Combat::fireMelee(wpn, eyePos, forward, m_entities);
        if (sub != WeaponSubtype::DAGGER && sub != WeaponSubtype::NONE &&
            result.hitEntity && (std::rand() % 100) < 5) {
            WeaponDef cleaveWpn = wpn;
            cleaveWpn.coneAngleDeg = 360.0f;
            cleaveWpn.damage *= 0.5f;
            Combat::fireMelee(cleaveWpn, eyePos, forward, m_entities); // cleave is never a crit
        }
    } break;
    case WeaponType::HITSCAN: {
        // (H5) Overcharged Magazine for a remote Marksman — mirror the host's host-side branch
        // so a remote's overcharge actually buffs their own gun (3× damage, +30% attack speed,
        // penetrate all enemies in a narrow cone, instant reload on kill). Without this the
        // remote's ult silently empowered slot 0 (the host) instead.
        if (SkillSystem::isOvercharged(np.slotIndex)) {
            wpn.damage *= 3.0f;
            np.weaponState.cooldownTimer *= 0.7f;
            SkillSystem::consumeOverchargeShot(np.slotIndex);
            EntityHandle oHits[MAX_ENTITIES];
            f32          oDists[MAX_ENTITIES];
            u32 oCnt = CombatQuery::queryConeSorted(
                m_entities, eyePos, forward, cosf(radians(1.0f)), wpn.range,
                oHits, oDists, MAX_ENTITIES);
            bool gotKill = false;
            for (u32 oi = 0; oi < oCnt; oi++) {
                Entity* oe = handleGet(m_entities, oHits[oi]);
                if (!oe || (oe->flags & ENT_DEAD) || (oe->flags & ENT_FRIENDLY)) continue;
                f32 hpBefore = oe->health;
                Combat::applyDamage(m_entities, oHits[oi], wpn.damage);
                if (hpBefore > 0.0f && oe->health <= 0.0f) gotKill = true;
            }
            if (gotKill) { np.weaponState.currentClip = wpn.clipSize; np.weaponState.reloading = false; }
            break;
        }
        result = Combat::fireHitscan(wpn, eyePos, forward, m_level.grid, m_entities);
        if (result.hitEntity || result.hitWorld) {
            for (u32 fx = 0; fx < MAX_IMPACT_FX; fx++) {
                if (!m_fx.impactFX[fx].active) {
                    m_fx.impactFX[fx] = {result.hitPosition, result.hitNormal,
                                      0.3f, true, result.hitEntity};
                    break;
                }
            }
            if (result.hitEntity) m_hitMarkerTimer = 0.2f;

            // Broadcast impact position + normal to clients so they see the sparks
            u8 evBuf[sizeof(PacketHeader) + 26]; // eventType(1) + pos(12) + normal(12) + hitEntity(1)
            PacketHeader* evHdr = reinterpret_cast<PacketHeader*>(evBuf);
            evHdr->type = NetPacketType::SV_EVENT;
            evHdr->flags = 0;
            evHdr->seq = 0;
            u32 off = sizeof(PacketHeader);
            evBuf[off++] = static_cast<u8>(NetEventType::HITSCAN_IMPACT);
            std::memcpy(evBuf + off, &result.hitPosition.x, 4); off += 4;
            std::memcpy(evBuf + off, &result.hitPosition.y, 4); off += 4;
            std::memcpy(evBuf + off, &result.hitPosition.z, 4); off += 4;
            std::memcpy(evBuf + off, &result.hitNormal.x, 4);   off += 4;
            std::memcpy(evBuf + off, &result.hitNormal.y, 4);   off += 4;
            std::memcpy(evBuf + off, &result.hitNormal.z, 4);   off += 4;
            evBuf[off++] = result.hitEntity ? 1 : 0;
            Net::broadcastReliable(evBuf, off);
        }
    } break;
    case WeaponType::PROJECTILE: {
        // Mirror the host's SP fire path (engine_combat.cpp:286-334) so a remote-fired
        // projectile carries the right mesh/light/gravity over the wire. Without this,
        // ProjectileSystem::spawn defaulted meshId=0 and a remote bow shot rendered as
        // the fallback energy-bolt instead of an arrow ("bow not an arrow on client").
        // Subtype comes from the equipped item; falls back to the bare wpn.type path
        // if the slot is empty (no item → use default fireProjectile, no special mesh).
        WeaponSubtype sub = WeaponSubtype::NONE;
        if (!isItemEmpty(eqWpn)) sub = m_itemDefs[eqWpn.defId].weaponSubtype;
        bool isMolotov = (sub == WeaponSubtype::MOLOTOV);
        bool isWand    = (sub == WeaponSubtype::WAND);

        u16 projIdx;
        if (isMolotov) {
            // Gravity + splash overload — same magic numbers as the host path
            projIdx = Combat::fireProjectile(wpn, eyePos, forward, m_projectiles,
                                              9.8f, 3.0f, wpn.damage * 0.6f);
        } else {
            // Wand gets a spark flag for visual; remote-fired wands don't have the
            // m_weaponProc void-zone state (that's host-local), so PROJ_VOID is omitted.
            u8 flags = isWand ? PROJ_SPARK : 0;
            projIdx = Combat::fireProjectile(wpn, eyePos, forward, m_projectiles, flags);
        }
        if (projIdx != 0xFFFF) {
            Projectile& proj = m_projectiles.projectiles[projIdx];
            if (sub == WeaponSubtype::BOW) {
                proj.meshId = m_meshIdArrow;
            } else if (sub == WeaponSubtype::CROSSBOW) {
                proj.meshId = m_meshIdBolt;
            } else if (sub == WeaponSubtype::THROWING_KNIFE || sub == WeaponSubtype::MOLOTOV) {
                u8 wpnMesh = m_itemDefs[eqWpn.defId].meshId;
                if (wpnMesh > 0) proj.meshId = wpnMesh;
            }
            if (isMolotov)   proj.lightColor = {1.0f, 0.5f, 0.1f}; // fire
            else if (isWand) proj.lightColor = {0.4f, 0.6f, 1.0f}; // arcane blue
        }
        result.hitEntity = false; // procs handled by projectile hit callback
    } break;
    }

    // --- Life steal ring passive for remote player ---
    if (np.ringPassive == SkillId::LIFE_STEAL && result.hitEntity) {
        f32 heal = wpn.damage * 0.05f;
        np.health += heal;
        if (np.health > np.maxHealth) np.health = np.maxHealth;
    }

    // Affix life-on-hit for remote player
    if (result.hitEntity) {
        f32 loh = m_inventories[np.slotIndex].bonusLifeOnHit;
        if (loh > 0.0f) {
            f32 heal = wpn.damage * loh;
            np.health = fminf(np.health + heal, np.maxHealth);
        }
    }

    // --- Weapon legendary on-hit proc for remote player ---
    if (result.hitEntity && np.weaponProc != SkillId::NONE) {
        u32 procRoll = static_cast<u32>(std::rand()) % 100;
        u32 procChance = 20;
        if (np.weaponProc == SkillId::FROZEN_ORB)      procChance = 15;
        if (np.weaponProc == SkillId::CHAIN_LIGHTNING)  procChance = 25;
        if (np.weaponProc == SkillId::METEOR_STRIKE)    procChance = 10;
        if (np.weaponProc == SkillId::BLOOD_NOVA)       procChance = 20;
        if (np.weaponProc == SkillId::VOID_ZONE)        procChance = 5;
        if (np.weaponProc == SkillId::ARC_FIRE)         procChance = 20;

        if (procRoll < procChance) {
            Vec3 procPos = result.hitPosition;
            const SkillDef* sd = SkillSystem::findSkillDef(m_skillDefs, m_skillDefCount, np.weaponProc);
            if (sd || np.weaponProc == SkillId::ARC_FIRE) {
                switch (np.weaponProc) {
                    case SkillId::FROZEN_ORB: {
                        u16 orbIdx = ProjectileSystem::spawn(m_projectiles, procPos, forward,
                            sd->projectileSpeed, sd->damage, sd->radius, sd->duration, true);
                        if (orbIdx != 0xFFFF) m_projectiles.projectiles[orbIdx].projFlags = PROJ_ORB;
                    } break;
                    case SkillId::CHAIN_LIGHTNING: {
                        SkillDef procDef = *sd;
                        procDef.bounces = static_cast<u8>(3 + (eqWpn.itemLevel - 1) * 17 / 49);
                        SkillState tempSS;
                        tempSS.activeSkill = SkillId::CHAIN_LIGHTNING;
                        tempSS.cooldownTimer = 0.0f;
                        tempSS.energy = 999.0f;
                        tempSS.maxEnergy = 999.0f;
                        { u8 lvl = m_inventories[m_localPlayerIndex].equipped[static_cast<u32>(ItemSlot::WEAPON)].itemLevel;
                          SkillSystem::setSkillPower(lvl > 1 ? static_cast<f32>(lvl - 1) / 149.0f : 0.0f); }
                        SkillSystem::setClassDamageMult(1.0f);
                        SkillSystem::tryActivate(tempSS, &procDef, 1,
                            procPos, forward, np.yaw,
                            m_projectiles, m_entities, m_level.grid, m_localPlayer);
                    } break;
                    case SkillId::METEOR_STRIKE: {
                        extern PendingMeteor s_meteors[MAX_PENDING_METEORS];
                        for (u32 mi = 0; mi < MAX_PENDING_METEORS; mi++) {
                            if (!s_meteors[mi].active) {
                                s_meteors[mi] = {procPos, sd->damage, sd->radius, sd->delay, true};
                                break;
                            }
                        }
                    } break;
                    case SkillId::BLOOD_NOVA: {
                        EntityHandle hits[MAX_ENTITIES];
                        f32 dists[MAX_ENTITIES];
                        u32 hitCount = CombatQuery::queryConeSorted(
                            m_entities, procPos, {0,0,-1}, -1.0f, sd->radius,
                            hits, dists, MAX_ENTITIES);
                        for (u32 h = 0; h < hitCount; h++) {
                            Combat::applyDamage(m_entities, hits[h], sd->damage * 0.5f);
                        }
                        for (u32 ni = 0; ni < MAX_NOVA_FX; ni++) {
                            if (!m_fx.novaFX[ni].active) {
                                m_fx.novaFX[ni] = {procPos, sd->radius, 0.6f, true, {1.0f, 0.15f, 0.1f}};
                                break;
                            }
                        }
                    } break;
                    case SkillId::ARC_FIRE: {
                        f32 arcDps = wpn.damage * 0.3f;
                        f32 halfAngle = wpn.coneAngleDeg * 0.5f * 3.14159f / 180.0f;
                        for (u32 fi = 0; fi < 5; fi++) {
                            f32 t = (fi / 4.0f) * 2.0f - 1.0f;
                            f32 angle = np.yaw + t * halfAngle;
                            Vec3 dir = {-sinf(angle), 0.0f, -cosf(angle)};
                            Vec3 zonePos = np.position + dir * wpn.range * 0.8f;
                            for (u32 si = 0; si < MAX_SCORCH; si++) {
                                if (!m_fx.scorchZones[si].active) {
                                    m_fx.scorchZones[si] = {zonePos, 1.0f, 1.5f, arcDps, true};
                                    break;
                                }
                            }
                            for (u32 fxi = 0; fxi < MAX_FIRE_FX; fxi++) {
                                if (!m_fx.fireFX[fxi].active) {
                                    m_fx.fireFX[fxi] = {zonePos, 1.5f, true};
                                    break;
                                }
                            }
                        }
                    } break;
                    default: break;
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Soft target lock (singleplayer). Lock-on itself is currently inert — lockActive
// is never set true, so this only handles the TARGET_LOCK action's quickbar-use
// behaviour. The trailing lockActive=false keeps the (unused) state pinned off (R7-6).
// ---------------------------------------------------------------------------
void Engine::updateTargetLock(f32 dt) {
    (void)dt;
    // Middle-click / quickbar-use outside inventory: equip active quickbar item (keeps ref in quickbar)
    if (Input::isActionPressed(GameAction::TARGET_LOCK)) {
        u8 slot = m_quickbars[m_localPlayerIndex].activeSlot;
        QuickbarSlot& qs = m_quickbars[m_localPlayerIndex].slots[slot];
        if (qs.type == QuickbarSlot::BACKPACK_REF &&
            qs.sourceIndex < MAX_INVENTORY_ITEMS &&
            !isItemEmpty(m_inventories[m_localPlayerIndex].backpack[qs.sourceIndex])) {
            u32 uid = qs.itemUid;
            ItemSlot itemSlot = m_itemDefs[m_inventories[m_localPlayerIndex].backpack[qs.sourceIndex].defId].slot;
            Inventory::equip(m_inventories[m_localPlayerIndex], qs.sourceIndex, m_itemDefs);
            // Convert quickbar ref from backpack to equipment so it stays valid
            qs.type = QuickbarSlot::EQUIPPED_REF;
            qs.sourceIndex = static_cast<u8>(itemSlot);
            qs.itemUid = uid;
            Quickbar::syncWeaponSlot(m_quickbars[m_localPlayerIndex], m_inventories[m_localPlayerIndex]);
        }
    }
    m_localPlayer.lockActive = false;
}

// ---------------------------------------------------------------------------
// Viewmodel — renders first-person hand + equipped weapon over everything
