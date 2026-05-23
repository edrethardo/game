// Engine per-player tick helpers — extracted from gameUpdate for readability.
// Contains all sections that are tightly coupled to m_localPlayer state:
// Wanderer class timers, status-effect DoTs, FX decay, visual feedback,
// misc timer drains, and debug key toggles.
// All methods are Engine:: members called in the exact order they appeared inline.

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
// tickWandererTimers — adrenaline counter stack decay, deflect burst window,
// deflect speed buff, Exploit Weakness mark, mark speed stacks, Death's Dance
// duration, and floor-gated unlock/upgrade flags.
// These all live on m_localPlayer.* and are only meaningful for WANDERER;
// the non-WANDERER fields are zeroed at character creation so ticking them
// for other classes is a no-op.
// ---------------------------------------------------------------------------
void Engine::tickWandererTimers(f32 dt) {
    // --- Wanderer: tick adrenaline counter stack decay ---
    // Each stack persists for 4s independently; remove expired stacks by compacting.
    {
        DodgeState& ds = m_localPlayer.dodgeState;
        for (u8 i = 0; i < ds.counterStacks; ) {
            ds.counterTimers[i] -= dt;
            if (ds.counterTimers[i] <= 0.0f) {
                // Remove this stack, shift remaining down to fill the gap
                for (u8 j = i; j + 1 < ds.counterStacks; j++) {
                    ds.counterTimers[j] = ds.counterTimers[j + 1];
                }
                ds.counterStacks--;
            } else {
                i++;
            }
        }
    }

    // --- Wanderer: tick deflect absorb window ---
    if (m_localPlayer.deflectTimer > 0.0f) {
        f32 prev = m_localPlayer.deflectTimer;
        m_localPlayer.deflectTimer -= dt;
        if (m_localPlayer.deflectTimer <= 0.0f) {
            m_localPlayer.deflectTimer = 0.0f;
            // Window expired — burst release: 8 projectiles per absorbed hit
            u8 hits = m_localPlayer.deflectHitCount;
            f32 absorbed = m_localPlayer.deflectAbsorbed;
            if (hits > 0 && absorbed > 0.0f) {
                Vec3 eyePos = m_localPlayer.position + Vec3{0, m_localPlayer.eyeHeight, 0};

                // 1. Melee nova first: hit everything within 5.5m for full absorbed damage
                {
                    EntityHandle novaHits[MAX_ENTITIES];
                    f32 novaDists[MAX_ENTITIES];
                    u32 novaCount = CombatQuery::queryConeSorted(
                        m_entities, eyePos, m_localPlayer.forward, -1.0f, 5.5f,
                        novaHits, novaDists, MAX_ENTITIES);
                    for (u32 ni = 0; ni < novaCount; ni++) {
                        Combat::applyDamage(m_entities, novaHits[ni], absorbed, &eyePos);
                    }
                }
                // Nova visual — double ring burst for a bigger impact feel
                for (u32 ni = 0; ni < MAX_NOVA_FX; ni++) {
                    if (!m_fx.novaFX[ni].active) {
                        m_fx.novaFX[ni] = {m_localPlayer.position, 5.5f, 0.6f, true, Vec3{1.0f, 0.5f, 0.1f}};
                        break;
                    }
                }
                // Second ring slightly delayed and larger for a shockwave look
                for (u32 ni = 0; ni < MAX_NOVA_FX; ni++) {
                    if (!m_fx.novaFX[ni].active) {
                        m_fx.novaFX[ni] = {m_localPlayer.position, 7.0f, 0.8f, true, Vec3{1.0f, 0.8f, 0.3f}};
                        break;
                    }
                }
                // Stronger screen shake
                m_camera.shake.trigger(0.08f, 0.35f);

                // 2. Projectile burst aimed at surviving enemies, split evenly
                {
                    // Query surviving enemies after the nova (dead ones are filtered out)
                    EntityHandle targets[MAX_ENTITIES];
                    f32 targetDists[MAX_ENTITIES];
                    u32 targetCount = CombatQuery::queryConeSorted(
                        m_entities, eyePos, m_localPlayer.forward, -1.0f, 30.0f,
                        targets, targetDists, MAX_ENTITIES);
                    u32 totalProj = static_cast<u32>(hits) * 8u;
                    if (targetCount > 0) {
                        // Split projectiles evenly across surviving enemies
                        u32 projPerTarget = totalProj / targetCount;
                        if (projPerTarget < 1) projPerTarget = 1;
                        u32 spawned = 0;
                        for (u32 ti = 0; ti < targetCount && spawned < totalProj; ti++) {
                            Entity* tgt = handleGet(m_entities, targets[ti]);
                            if (!tgt) continue;
                            Vec3 targetPos = tgt->position + Vec3{0, tgt->halfExtents.y, 0};
                            Vec3 dir = normalize(targetPos - eyePos);
                            u32 count = (ti < targetCount - 1) ? projPerTarget : (totalProj - spawned);
                            for (u32 pi = 0; pi < count; pi++) {
                                for (u32 si = 0; si < MAX_PROJECTILES; si++) {
                                    Projectile& proj = m_projectiles.projectiles[si];
                                    if (!proj.active) {
                                        proj = {};
                                        proj.active = true;
                                        proj.fromPlayer = true;
                                        proj.position = eyePos;
                                        proj.velocity = dir * 15.0f;
                                        proj.damage = absorbed;
                                        proj.radius = 0.12f;
                                        proj.lifetime = 2.0f;
                                        proj.lightColor = {1.0f, 0.6f, 0.2f};
                                        m_projectiles.activeCount++;
                                        spawned++;
                                        break;
                                    }
                                }
                            }
                        }
                    }
                    // No surviving enemies = no projectiles (nova was enough)
                }

                m_localPlayer.deflectAbsorbed = 0.0f;
                m_localPlayer.deflectHitCount = 0;
                // 8% move speed buff for 3 seconds after burst
                m_localPlayer.deflectSpeedTimer = 3.0f;
                // Screen shake feedback
                m_camera.shake.trigger(0.08f, 0.35f);
            } else {
                m_localPlayer.deflectAbsorbed = 0.0f;
                m_localPlayer.deflectHitCount = 0;
            }
        }
    }
    // --- Wanderer: tick deflect speed buff ---
    if (m_localPlayer.deflectSpeedTimer > 0.0f) {
        m_localPlayer.deflectSpeedTimer -= dt;
        if (m_localPlayer.deflectSpeedTimer < 0.0f) m_localPlayer.deflectSpeedTimer = 0.0f;
    }

    // --- Wanderer: tick Exploit Weakness mark duration ---
    if (m_localPlayer.markTimer > 0.0f) {
        m_localPlayer.markTimer -= dt;
        if (m_localPlayer.markTimer <= 0.0f) {
            m_localPlayer.markTimer = 0.0f;
            m_localPlayer.markedEntityIdx = 0xFFFF;
        }
    }
    // --- Wanderer: tick Exploit Weakness speed stacks (non-refreshing 3s each) ---
    {
        u8& stacks = m_localPlayer.markSpeedStacks;
        for (u8 i = 0; i < stacks; ) {
            m_localPlayer.markSpeedTimers[i] -= dt;
            if (m_localPlayer.markSpeedTimers[i] <= 0.0f) {
                for (u8 j = i; j + 1 < stacks; j++) {
                    m_localPlayer.markSpeedTimers[j] = m_localPlayer.markSpeedTimers[j + 1];
                }
                stacks--;
            } else {
                i++;
            }
        }
    }

    // --- Wanderer: tick Death's Dance ultimate duration ---
    if (m_localPlayer.deathsDanceTimer > 0.0f) {
        m_localPlayer.deathsDanceTimer -= dt;
        if (m_localPlayer.deathsDanceTimer < 0.0f) m_localPlayer.deathsDanceTimer = 0.0f;
    }

    // --- Wanderer: unlock and upgrade adrenaline based on current floor ---
    // Unlocked at floor 20 (skill becomes available), upgraded at floor 30 (move speed bonus)
    if (m_playerClass == PlayerClass::WANDERER) {
        m_localPlayer.adrenalineUnlocked = (m_level.currentFloor >= 20);
        m_localPlayer.adrenalineUpgraded = (m_level.currentFloor >= 30);
    }
}

// ---------------------------------------------------------------------------
// tickPlayerStatusEffects — poison/burn DoT damage per tick, freeze countdown,
// and clearing DoTs while invulnerable.
// ---------------------------------------------------------------------------
void Engine::tickPlayerStatusEffects(f32 dt) {
    // Tick player status effects (poison, burn, freeze) — blocked by invulnerability
    if (m_localPlayer.invulnTimer <= 0.0f) {
        if (m_localPlayer.poisonTimer > 0.0f) {
            m_localPlayer.poisonTimer -= dt;
            m_localPlayer.health -= m_localPlayer.poisonDps * dt;
        }
        if (m_localPlayer.burnTimer > 0.0f) {
            m_localPlayer.burnTimer -= dt;
            m_localPlayer.health -= m_localPlayer.burnDps * dt;
        }
    } else {
        // Clear DoT effects during invulnerability
        m_localPlayer.poisonTimer = 0.0f;
        m_localPlayer.burnTimer = 0.0f;
        m_localPlayer.freezeTimer = 0.0f;
        m_localPlayer.slowTimer = 0.0f;
    }
    if (m_localPlayer.freezeTimer > 0.0f) {
        m_localPlayer.freezeTimer -= dt;
    }
}

// ---------------------------------------------------------------------------
// handleDebugKeys — F1-F6 toggles (debug draw, noclip, profiler, stress
// spawners, Switch constraint mode). F7 debug item lives after inventory
// interaction in gameUpdate so it is left inline there.
// ---------------------------------------------------------------------------
void Engine::handleDebugKeys() {
    // Toggle debug overlay
    if (Input::isKeyPressed(SDL_SCANCODE_F1)) {
        DebugDraw::setEnabled(!DebugDraw::isEnabled());
    }

    // Toggle noclip
    if (Input::isKeyPressed(SDL_SCANCODE_F2)) {
        m_localPlayer.noclip = !m_localPlayer.noclip;
        LOG_INFO("Noclip: %s", m_localPlayer.noclip ? "ON" : "OFF");
    }

    // Toggle profiler overlay
    if (Input::isKeyPressed(SDL_SCANCODE_F3)) {
        Profiler& prof = getProfiler();
        prof.enabled = !prof.enabled;
        LOG_INFO("Profiler: %s", prof.enabled ? "ON" : "OFF");
    }

    // Stress spawner: F4 = 10 enemies, F5 = 50 enemies
    if (Input::isKeyPressed(SDL_SCANCODE_F4)) {
        u32 spawned = 0;
        for (u32 s = 0; s < 10 && m_entities.freeCount > 0; s++) {
            f32 angle = (s / 10.0f) * 6.28f;
            Vec3 pos = m_localPlayer.position + Vec3{cosf(angle) * 5.0f, 0.5f, sinf(angle) * 5.0f};
            bool flying = (s % 3 == 0);
            Vec3 half = flying ? Vec3{0.3f, 0.3f, 0.3f} : Vec3{0.4f, 0.5f, 0.4f};
            EntityHandle h = EntitySystem::spawn(m_entities, pos, half, flying,
                flying ? 30.0f : 50.0f, flying ? 4.0f : 2.5f,
                15.0f, flying ? 8.0f : 2.5f, flying ? 1.5f : 1.0f, flying ? 8.0f : 10.0f);
            Entity* ent = handleGet(m_entities, h);
            if (ent) { ent->baseMoveSpeed = ent->moveSpeed; ent->baseAttackCooldown = ent->attackCooldown; }
            spawned++;
        }
        LOG_INFO("Spawned %u enemies (total: %u)", spawned, EntitySystem::activeCount(m_entities));
    }

    if (Input::isKeyPressed(SDL_SCANCODE_F5)) {
        u32 spawned = 0;
        for (u32 s = 0; s < 50 && m_entities.freeCount > 0; s++) {
            f32 angle = (s / 50.0f) * 6.28f;
            f32 radius = 4.0f + (s % 5) * 2.0f;
            Vec3 pos = m_localPlayer.position + Vec3{cosf(angle) * radius, 0.5f, sinf(angle) * radius};
            bool flying = (s % 4 == 0);
            Vec3 half = flying ? Vec3{0.3f, 0.3f, 0.3f} : Vec3{0.4f, 0.5f, 0.4f};
            EntityHandle h = EntitySystem::spawn(m_entities, pos, half, flying,
                flying ? 30.0f : 50.0f, flying ? 4.0f : 2.5f,
                15.0f, flying ? 8.0f : 2.5f, flying ? 1.5f : 1.0f, flying ? 8.0f : 10.0f);
            Entity* ent = handleGet(m_entities, h);
            if (ent) { ent->baseMoveSpeed = ent->moveSpeed; ent->baseAttackCooldown = ent->attackCooldown; }
            spawned++;
        }
        LOG_INFO("Spawned %u enemies (total: %u)", spawned, EntitySystem::activeCount(m_entities));
    }

    // Switch constraint mode (F6)
    if (Input::isKeyPressed(SDL_SCANCODE_F6)) {
        m_switchMode = !m_switchMode;
        if (m_switchMode) {
            m_camera.farPlane = SWITCH_FAR_PLANE;
            LOG_INFO("[SWITCH] Mode ON — far=%.0f, res=%ux%u", SWITCH_FAR_PLANE, SWITCH_RES_W, SWITCH_RES_H);
        } else {
            m_camera.farPlane = 200.0f;
            LOG_INFO("[SWITCH] Mode OFF");
        }
    }
}

// ---------------------------------------------------------------------------
// tickFXDecay — expire timed visual effects pools: impact sparks, fire bursts,
// nova rings, dash trails, beam FX, overcharge buff, dynamic weapon lights,
// chain lightning FX, scorch ground-fire AoE DoT, and the herald aura burn.
// ---------------------------------------------------------------------------
void Engine::tickFXDecay(f32 dt) {
    // Decay visual effects (impact, fire, nova, dash)
    for (u32 i = 0; i < MAX_IMPACT_FX; i++) {
        if (m_fx.impactFX[i].active) {
            m_fx.impactFX[i].timer -= dt;
            if (m_fx.impactFX[i].timer <= 0.0f) m_fx.impactFX[i].active = false;
        }
    }
    for (u32 i = 0; i < MAX_FIRE_FX; i++) {
        if (m_fx.fireFX[i].active) {
            m_fx.fireFX[i].timer -= dt;
            if (m_fx.fireFX[i].timer <= 0.0f) m_fx.fireFX[i].active = false;
        }
    }
    for (u32 i = 0; i < MAX_NOVA_FX; i++) {
        if (m_fx.novaFX[i].active) {
            m_fx.novaFX[i].timer -= dt;
            if (m_fx.novaFX[i].timer <= 0.0f) m_fx.novaFX[i].active = false;
        }
    }
    for (u32 i = 0; i < MAX_DASH_FX; i++) {
        if (m_fx.dashFX[i].active) {
            m_fx.dashFX[i].timer -= dt;
            if (m_fx.dashFX[i].timer <= 0.0f) m_fx.dashFX[i].active = false;
        }
    }
    for (u32 i = 0; i < MAX_BEAM_FX; i++) {
        if (m_fx.beamFX[i].active) {
            m_fx.beamFX[i].timer -= dt;
            if (m_fx.beamFX[i].timer <= 0.0f) m_fx.beamFX[i].active = false;
        }
    }
    // Tick overcharge buff (Marksman)
    SkillSystem::tickOvercharge(dt);
    // Tick dynamic lights (weapon muzzle flashes)
    for (u32 i = 0; i < MAX_DYNAMIC_LIGHTS; i++) {
        if (m_dynamicLights[i].timer > 0.0f) {
            m_dynamicLights[i].timer -= dt;
        }
    }
    for (u32 i = 0; i < MAX_CHAIN_FX; i++) {
        if (m_fx.chainFX[i].active) {
            m_fx.chainFX[i].timer -= dt;
            if (m_fx.chainFX[i].timer <= 0.0f) m_fx.chainFX[i].active = false;
        }
    }
    // Scorch zones — persistent ground fire dealing AoE DoT each tick
    for (u32 i = 0; i < MAX_SCORCH; i++) {
        if (!m_fx.scorchZones[i].active) continue;
        ScorchZone& sz = m_fx.scorchZones[i];
        sz.timer -= dt;
        if (sz.timer <= 0.0f) { sz.active = false; continue; }
        // Damage all hostile entities standing in the scorch zone
        for (u32 a = 0; a < m_entities.activeCount; a++) {
            u32 idx = m_entities.activeList[a];
            Entity& ent = m_entities.entities[idx];
            if (ent.flags & ENT_DEAD) continue;
            if (ent.flags & ENT_FRIENDLY) continue;
            if (ent.enemyType == EnemyType::PROP) continue;
            f32 distSq = lengthSq(ent.position - sz.pos);
            if (distSq < sz.radius * sz.radius) {
                ent.health -= sz.dps * dt;
                // Route death through killEntity so scorch kills still drop loot / fire procs.
                if (ent.health <= 0.0f)
                    Combat::killEntity(m_entities, {static_cast<u16>(idx), ent.generation});
            }
        }
    }

    // Herald aura — staggered across 30 frames to avoid full entity scan every frame
    static u32 s_heraldFrame = 0;
    s_heraldFrame++;
    for (u32 a = s_heraldFrame % 30; a < m_entities.activeCount; a += 30) {
        u32 idx = m_entities.activeList[a];
        Entity& e = m_entities.entities[idx];
        if (!(e.flags & ENT_ACTIVE) || (e.flags & ENT_DEAD)) continue;
        if (e.flags & ENT_FRIENDLY) continue;
        if (!(e.enemyRole & EnemyRole::AURA)) continue;
        // Check distance to local player
        Vec3 diff = m_localPlayer.position - e.position;
        f32 dist2 = diff.x * diff.x + diff.z * diff.z;
        if (dist2 < 3.0f * 3.0f) {
            m_localPlayer.burnTimer = fmaxf(m_localPlayer.burnTimer, 0.5f);
            m_localPlayer.burnDps = 4.0f;
        }
    }
}

// ---------------------------------------------------------------------------
// tickVisualFeedback — damage flash (first tick triggers hit sound + rumble),
// hurt vignette decay, and low-HP visual/rumble danger pulse.
// ---------------------------------------------------------------------------
void Engine::tickVisualFeedback(f32 dt) {
    // Damage flash decay — play hit sound on the first tick of a fresh flash
    if (m_localPlayer.damageFlashTimer > 0.0f) {
        if (m_localPlayer.damageFlashTimer >= 0.15f - 0.001f) {
            AudioSystem::play(SfxId::PLAYER_HIT, 0.7f);
            m_camera.shake.trigger(0.03f, 0.2f);
            // Gamepad rumble scaled by how hard we were hit.
            f32 r = 0.3f + m_localPlayer.hurtVignette * 0.5f;
            Input::rumble(m_localPlayerIndex, r, 120 + static_cast<u32>(m_localPlayer.hurtVignette * 180.0f));
        }
        m_localPlayer.damageFlashTimer -= dt;
    }

    // Hurt vignette decay (~0.4s fade to clear after each hit).
    if (m_localPlayer.hurtVignette > 0.0f) {
        m_localPlayer.hurtVignette -= dt * 2.5f;
        if (m_localPlayer.hurtVignette < 0.0f) m_localPlayer.hurtVignette = 0.0f;
    }
    // Low-HP danger feedback. Visual red pulse under 25% HP; a LIGHT periodic rumble
    // "nag" under 40% HP — a short pulse roughly once a second, never a constant rumble.
    {
        f32 hpFrac = (m_localPlayer.maxHealth > 0.0f)
                   ? (m_localPlayer.health / m_localPlayer.maxHealth) : 1.0f;
        bool alive = hpFrac > 0.0f;
        if (alive && hpFrac < 0.25f) {
            // sinf oscillates at 5 Hz for an urgent heartbeat feel.
            f32 pulse = 0.12f + 0.06f * sinf(static_cast<f32>(Clock::getElapsedSeconds()) * 5.0f);
            if (pulse > m_localPlayer.hurtVignette) m_localPlayer.hurtVignette = pulse;
        }
        if (alive && hpFrac < 0.40f) {
            m_lowHpRumbleTimer -= dt;
            if (m_lowHpRumbleTimer <= 0.0f) {
                Input::rumble(m_localPlayerIndex, 0.25f, 110);  // light, short nag pulse
                m_lowHpRumbleTimer = 1.1f;                       // ~1 pulse/sec, not constant
            }
        } else {
            m_lowHpRumbleTimer = 0.0f;  // reset so it fires promptly on re-entering danger
        }
    }
}

// ---------------------------------------------------------------------------
// tickMiscTimers — drains smoke/overdrive/shadowDance/curse/hitMarker/tooltip
// timers, advances the tutorial pulse clock, applies the camera transform,
// applies the view-bob lateral sway to camera yaw, and applies hit-shake.
// ---------------------------------------------------------------------------
void Engine::tickMiscTimers(f32 dt) {
    if (m_localPlayer.smokeTimer > 0.0f)
        m_localPlayer.smokeTimer -= dt;
    if (m_localPlayer.overdriveTimer > 0.0f)
        m_localPlayer.overdriveTimer -= dt;
    // Shadow Dance: tick timer, keep smokeTimer synced, apply speed bonus
    if (m_localPlayer.shadowDanceTimer > 0.0f) {
        m_localPlayer.shadowDanceTimer -= dt;
        if (m_localPlayer.shadowDanceTimer > 0.0f) {
            // Keep stealth active for the full Shadow Dance duration
            if (m_localPlayer.smokeTimer < m_localPlayer.shadowDanceTimer)
                m_localPlayer.smokeTimer = m_localPlayer.shadowDanceTimer;
        } else {
            m_localPlayer.shadowDanceTimer = 0.0f;
        }
    }
    if (m_localPlayer.curseTimer > 0.0f) {
        m_localPlayer.curseTimer -= dt;
        if (m_localPlayer.curseTimer <= 0.0f) m_localPlayer.curseStacks = 0;
    }
    if (m_hitMarkerTimer > 0.0f)
        m_hitMarkerTimer -= dt;
    if (m_fullBackpackNotifyTimer > 0.0f) m_fullBackpackNotifyTimer -= dt;
    if (m_controlsTooltipTimer > 0.0f) m_controlsTooltipTimer -= dt;
    m_tutorialPulseTimer += dt; // shared pulse clock for tutorial tooltips

    // Save previous camera state for render interpolation
    m_camera.prevPosition = m_camera.position;
    m_camera.prevYaw      = m_camera.yaw;
    m_camera.prevPitch    = m_camera.pitch;
    PlayerController::applyToCamera(m_localPlayer, m_camera);

    // View bob: lateral head sway (figure-8 horizontal component)
    // Applied to camera yaw so it doesn't accumulate on the player
    {
        f32 vxB = m_localPlayer.velocity.x;
        f32 vzB = m_localPlayer.velocity.z;
        f32 sSq = vxB * vxB + vzB * vzB;
        f32 bobAmp = sSq * 0.028f;
        if (bobAmp > 1.0f) bobAmp = 1.0f;
        f32 angle = m_viewmodelState.bobTimer * 5.5f;
        // Horizontal: single-freq sway (half the 8's width)
        m_camera.yaw += bobAmp * 0.008f * sinf(angle);
        // Slight roll for that heavy-footed head tilt
        // (roll isn't in our Camera struct, so we tilt pitch slightly at ¼ freq
        //  to give a subtle forward lean at each step peak)
    }

    // Screen shake — vertical pitch wobble + subtle horizontal position offset
    if (m_localPlayer.hitShakeTimer > 0.0f) {
        m_localPlayer.hitShakeTimer -= dt;
        f32 shake = m_localPlayer.hitShakeTimer * 0.08f;
        m_camera.pitch += sinf(m_localPlayer.hitShakeTimer * 60.0f) * shake;
        m_camera.position.x += sinf(m_localPlayer.hitShakeTimer * 47.0f) * shake * 0.3f;
    }
}
