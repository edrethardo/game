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
#include "game/shrine.h"
#include "game/skill.h"
#include "game/inventory_ui.h"
#include "game/game_constants.h"
#include "net/net.h"
#include "net/server.h"
#include "net/client.h"
#include "net/snapshot.h"
#include "net/packet.h"
#include "net/render_offset.h"
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

    // --- Wanderer: adrenaline available from floor 1; move-speed upgrade at floor 30 ---
    if (m_playerClass == PlayerClass::WANDERER) {
        m_localPlayer.adrenalineUnlocked = true;
        m_localPlayer.adrenalineUpgraded = (m_level.currentFloor >= 30);
        // Max adrenaline stacks: 3 on floors 1-4, raised to 5 from floor 5 onward.
        m_localPlayer.adrenalineMaxStacks = (m_level.currentFloor >= 5) ? 5 : 3;
    }
}

// ---------------------------------------------------------------------------
// tickPlayerStatusEffects — poison/burn DoT damage per tick, freeze countdown,
// and clearing DoTs while invulnerable.
// ---------------------------------------------------------------------------
void Engine::tickPlayerStatusEffects(f32 dt) {
    // CLIENT: status timers + HP are server-authoritative. Client::reconcile adopts
    // health/maxHealth/poisonTimer/burnTimer/freezeTimer/slowTimer from the snapshot every
    // tick (R7-4), and the host applies poison/burn DoT in serverNetPost. Decaying locally
    // would double-dip and the snapshot would clobber it anyway, so the entire block is a
    // no-op on CLIENT. (Was a partial gate around just the health subtraction; with the
    // N4 ghost sim now fully removed on CLIENT this becomes dead weight — skip it.)
    if (m_netRole == NetRole::CLIENT) return;
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

    // Passive health regen (HEALTH_REGEN affixes — defensive pack). Authoritative side only
    // (CLIENT returned above; remote players regen in serverNetPost). Additive + clamped to max,
    // and gated on being alive so it never revives a corpse. healthRegen is refreshed each frame
    // from equipped affixes in tickPassiveEquipment.
    if (m_localPlayer.healthRegen > 0.0f &&
        m_localPlayer.health > 0.0f && m_localPlayer.health < m_localPlayer.maxHealth) {
        m_localPlayer.health += m_localPlayer.healthRegen * dt;
        if (m_localPlayer.health > m_localPlayer.maxHealth)
            m_localPlayer.health = m_localPlayer.maxHealth;
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

    // Toggle profiler overlay (F3 on desktop; on Switch use a controller chord: hold L (left
    // shoulder) + click L3 (left stick). Both debug chords share L as the modifier — L+R3 toggles
    // Switch-mode, L+L3 toggles this profiler. The overlay's stacked per-scope bars
    // (Update/AI/Projectiles/Render/Flush) against the 16.67 ms marker reveal whether the frame is
    // CPU-bound: if the bars are SHORT but FPS is still ~40, the GPU is the limiter (fill/overdraw);
    // a long bar names the CPU system to chase.
    bool padProfilerToggle = Input::isGamepadConnected(0) &&
                             Input::isButtonDown(0, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) &&
                             Input::isButtonPressed(0, SDL_CONTROLLER_BUTTON_LEFTSTICK);
    if (Input::isKeyPressed(SDL_SCANCODE_F3) || padProfilerToggle) {
        Profiler& prof = getProfiler();
        prof.enabled = !prof.enabled;
        LOG_INFO("Profiler: %s", prof.enabled ? "ON" : "OFF");
    }

    // Stress spawner: F4 = 10 enemies, F5 = 50 enemies. Deliberately spawns fixed-stat,
    // low-HP dummies (NOT floor/difficulty-scaled) — these exist to stress entity count /
    // rendering / AI throughput, so they must stay quick to clear; scaling them to a Hell
    // enemy's HP would defeat the purpose. Production enemy scaling lives in engine_spawn.cpp.
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

    // Internal render-scale CYCLE (F6, desktop only): cycles m_renderScale 1.0 -> 0.8 -> 0.65 -> 0.5 ->
    // 1.0. The 3D scene renders to an FBO at that fraction and is upscaled to fill the screen, trading
    // sharpness for fill/overdraw savings on the fill-bound Switch GPU. 1.0 bypasses the FBO (native).
    // The Switch L+R3 controller chord was retired now that the scale is baked at 0.65 (engine.h): this
    // stays a desktop-only dev tuner for finding a new value, never reachable from a gamepad in play.
    if (Input::isKeyPressed(SDL_SCANCODE_F6)) {
        if      (m_renderScale > 0.95f) m_renderScale = 0.8f;
        else if (m_renderScale > 0.75f) m_renderScale = 0.65f;
        else if (m_renderScale > 0.60f) m_renderScale = 0.5f;
        else                            m_renderScale = 1.0f;
        LOG_INFO("[PERF] render scale: %.2f", m_renderScale);
    }

    // F9 toggles a network info overlay. CLIENT shows the D6 net-graph (RTT,
    // server-tick estimate, divergence count, fake-latency/loss). SERVER shows
    // the host info line (external IP from UPnP, or LAN-only fallback message)
    // so the host can read out the address friends should type into Join.
    if (Input::isKeyPressed(SDL_SCANCODE_F9)) {
        m_netGraphVisible = !m_netGraphVisible;
        LOG_INFO("Net info overlay: %s", m_netGraphVisible ? "ON" : "OFF");
    }

    // F8 — screenshot. Sets a one-shot flag; the actual glReadPixels must run AFTER the frame
    // is fully composited, so it is serviced just before the IN_GAME swapBuffers (engine_render.cpp).
    if (Input::isKeyPressed(SDL_SCANCODE_F8)) {
        m_screenshotPending = true;
    }

    // F10 — "cinematic" toggle: hides the HUD so screenshots capture clean key art (the 3D scene
    // still renders). Pair with F2 noclip for framing. Used to grab Steam/marketing hero shots.
    if (Input::isKeyPressed(SDL_SCANCODE_F10)) {
        m_hideHud = !m_hideHud;
        LOG_INFO("Cinematic mode (HUD hidden): %s", m_hideHud ? "ON" : "OFF");
    }
}

// ---------------------------------------------------------------------------
// tickSharedFX — ONCE-PER-FRAME decay of SHARED effect pools (impact/fire/nova/
// dash/beam/chain FX, dynamic weapon lights) and the scorch ground-fire AoE DoT.
// Must NOT run per local player, or split-screen ages/damages them at 2×.
// ---------------------------------------------------------------------------
void Engine::tickSharedFX(f32 dt) {
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
    for (u32 i = 0; i < MAX_SWING_FX; i++) {
        if (m_fx.swingFX[i].active) {
            m_fx.swingFX[i].timer -= dt;
            if (m_fx.swingFX[i].timer <= 0.0f) m_fx.swingFX[i].active = false;
        }
    }
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
                // The Dungeon Engine superboss is immune to scorch DoT while shielded, same as
                // direct hits and poison/burn — otherwise a scorch pool under it would bleed it
                // through the shield and could kill it mid-wave (firing VICTORY with adds alive).
                if (!(ent.isEngine && Combat::engineShieldActive(m_entities, static_cast<u16>(idx))))
                    ent.health -= sz.dps * dt;
                // Route death through killEntity so scorch kills still drop loot / fire procs.
                if (ent.health <= 0.0f)
                    Combat::killEntity(m_entities, {static_cast<u16>(idx), ent.generation});
            }
        }
    }
}

// ---------------------------------------------------------------------------
// tickPlayerFX — per-local-player feel/buff decay: Marksman overcharge timer and
// the herald aura burn applied to the swapped-in m_localPlayer. Runs once per
// local player (so each player's overcharge ticks once and each gets burned by
// auras they personally stand in).
// ---------------------------------------------------------------------------
void Engine::tickPlayerFX(f32 dt) {
    // Tick overcharge (Marksman) for ALL net slots, ONCE per frame: the array is MAX_PLAYERS
    // since H5, so a remote's overcharge has its own slot. Gate on lane 0 to avoid
    // double-decay in split-screen (the loop body iterates every slot independently of lane).
    if (m_localPlayerIndex == 0) {
        for (u32 s = 0; s < MAX_PLAYERS; s++) SkillSystem::tickOvercharge(dt, s);
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
    // M13: HP bar lerp — the visible bar value eases toward `health` at 4 Hz so a hit
    // doesn't look like an instant snap. The numeric readout keeps using `health` (raw).
    {
        const f32 HP_LERP_RATE = 4.0f;  // bar reaches ~95% of target in ~750 ms
        f32 hpDelta = m_localPlayer.health - m_localPlayer.renderedHealth;
        m_localPlayer.renderedHealth += hpDelta * dt * HP_LERP_RATE;
    }

    // M13: screen flash decay — counts down from 0.5 s after a large prediction divergence
    if (m_localPlayer.screenFlashTimer > 0.0f) {
        m_localPlayer.screenFlashTimer -= dt;
        if (m_localPlayer.screenFlashTimer < 0.0f) m_localPlayer.screenFlashTimer = 0.0f;
    }

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
    // Low-HP danger feedback — rumble only here. The low-HP VISUAL is a STEADY
    // (non-flashing) edge vignette computed at render time in renderPostOverlays;
    // we must never oscillate the red overlay (photosensitivity / WCAG 2.3.1).
    // The rumble is a LIGHT periodic "nag" under 40% HP — a short pulse roughly
    // once a second, never a constant rumble.
    {
        f32 hpFrac = (m_localPlayer.maxHealth > 0.0f)
                   ? (m_localPlayer.health / m_localPlayer.maxHealth) : 1.0f;
        bool alive = hpFrac > 0.0f;
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
// snapCameraToPlayer — place the render camera on the local player with ZERO
// interpolation delta (prev == current). Call after any teleport (floor change,
// respawn) so render() doesn't lerp the camera from the old position to the new
// for a frame or two — that smear showed the "old view" flickering as the
// fade-from-black cleared, which broke immersion on transitions/respawns.
// ---------------------------------------------------------------------------
void Engine::snapCameraToPlayer() {
    PlayerController::applyToCamera(m_localPlayer, m_camera);
    m_camera.prevPosition = m_camera.position;
    m_camera.prevYaw      = m_camera.yaw;
    m_camera.prevPitch    = m_camera.pitch;
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
    // Shrine buff expiry — AUTHORITATIVE SIM ONLY. A CLIENT adopts the whole buff (type, timer, and
    // the VITALITY max-HP bump via SnapPlayer.maxHealth) straight from the snapshot in clientNetPost,
    // so expiring it locally would fight the adoption and, worse, subtract the vitality bonus from a
    // maxHealth the server already owns.
    // VITALITY has to undo itself: it raised maxHealth, so on expiry the bonus must come back off,
    // and current HP must be clamped under the new (lower) cap or the player would sit above their
    // own maximum.
    if (m_netRole != NetRole::CLIENT && m_localPlayer.shrineBuffTimer > 0.0f) {
        m_localPlayer.shrineBuffTimer -= dt;
        if (m_localPlayer.shrineBuffTimer <= 0.0f) {
            // Unconditional: give back exactly what was granted, whatever the slot now says. The old
            // code gated this on `shrineBuff == VITALITY`, so a second shrine taken during the buff
            // silently made the max-HP permanent.
            if (m_localPlayer.shrineHealthBonus > 0.0f) {
                m_localPlayer.maxHealth -= m_localPlayer.shrineHealthBonus;
                if (m_localPlayer.maxHealth < 1.0f) m_localPlayer.maxHealth = 1.0f;
                if (m_localPlayer.health > m_localPlayer.maxHealth)
                    m_localPlayer.health = m_localPlayer.maxHealth;
                m_localPlayer.shrineHealthBonus = 0.0f;
            }
            m_localPlayer.shrineBuff      = ShrineBuff::NONE;
            m_localPlayer.shrineBuffValue = 0.0f;
            m_localPlayer.shrineBuffTimer = 0.0f;
        }
    }
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
    if (m_bossLockNotifyTimer > 0.0f) m_bossLockNotifyTimer -= dt;
    if (m_controlsTooltipTimer > 0.0f) m_controlsTooltipTimer -= dt;
    m_tutorialPulseTimer += dt; // shared pulse clock for tutorial tooltips

    // Save previous camera state for render interpolation
    m_camera.prevPosition = m_camera.position;
    m_camera.prevYaw      = m_camera.yaw;
    m_camera.prevPitch    = m_camera.pitch;

    // M4 smooth correction: on CLIENT, temporarily shift the player's visible position by
    // the decaying render offset so the camera eye smoothly slides toward the sim position
    // after a prediction correction. Sim state (m_localPlayer.position) is restored
    // immediately after so nothing downstream reads the offset-shifted value by accident.
    if (m_netRole == NetRole::CLIENT) {
        Vec3 savedPos = m_localPlayer.position;
        m_localPlayer.position = RenderOffsetOps::apply(m_renderOffset[m_localPlayerIndex], savedPos);
        PlayerController::applyToCamera(m_localPlayer, m_camera);
        m_localPlayer.position = savedPos;
    } else {
        PlayerController::applyToCamera(m_localPlayer, m_camera);
    }

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
