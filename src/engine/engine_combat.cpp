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
#include "net/pending_hit_ring.h"
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

// Per-slot pending fire request, queued by Engine::handleFireWeaponRequest (CL_FIRE_WEAPON
// handler) and consumed by the next handleWeaponFireForPlayer pass. The server fires from
// THESE claimed origin/yaw/pitch values rather than the drain-derived np.yaw — which was
// the bug source the user described as "fires from aim a few seconds ago" under input
// queue lag. Single slot per player; a second request before the first is consumed
// overwrites the first (rare in practice — client enforces cooldown locally too).
struct PendingFire {
    bool valid       = false;
    u32  clientTick  = 0;
    Vec3 origin      = {0,0,0};
    f32  yaw         = 0.0f;
    f32  pitch       = 0.0f;
};
static PendingFire s_pendingFires[MAX_PLAYERS];

// Phase 1.1 — Server-side dedup of CL_FIRE_WEAPON arrivals. The client now sends fires
// on the unreliable channel and retransmits the same packet for FIRE_TX_REPEATS client
// ticks to cover UDP loss without paying ENet's RTT-paced reliable retransmit latency.
// Without dedup, every retransmit would fire the weapon again. The ring keeps the last
// FIRE_DEDUP_SIZE client ticks we've already processed for each slot and short-circuits
// repeats. 32 × 4 slots × 4 B = 512 B, trivial. Cleared on floor descent (m_serverTick
// resets, so old ticks could spuriously match new ones) and on player join (next
// occupant of the slot starts fresh).
static constexpr u32 FIRE_DEDUP_SIZE = 32;
struct FireDedupRing {
    u32 recentTicks[FIRE_DEDUP_SIZE] = {};
    u8  head  = 0;   // next write index (LRU)
    u8  count = 0;
};
static FireDedupRing s_fireDedupRing[MAX_PLAYERS];

static bool fireWasSeen(u8 slot, u32 clientTick) {
    if (slot >= MAX_PLAYERS) return false;
    const FireDedupRing& r = s_fireDedupRing[slot];
    for (u8 i = 0; i < r.count; i++) {
        if (r.recentTicks[i] == clientTick) return true;
    }
    return false;
}
static void recordFire(u8 slot, u32 clientTick) {
    if (slot >= MAX_PLAYERS) return;
    FireDedupRing& r = s_fireDedupRing[slot];
    r.recentTicks[r.head] = clientTick;
    r.head = (r.head + 1) % FIRE_DEDUP_SIZE;
    if (r.count < FIRE_DEDUP_SIZE) r.count++;
}

// M10.1 — ClientFireTx / FIRE_TX_REPEATS / s_clientFireTx removed.
// CL_FIRE_WEAPON now uses ENet's reliable channel; manual retransmit is no longer
// needed. s_fireDedupRing is kept because a client might legally queue two distinct
// CL_FIRE_WEAPONs in rapid succession (e.g. auto-fire at max rate) and both could be
// in-flight simultaneously; the dedup ring prevents a delayed first arrival from
// double-firing after the second one already processed.

// =====================================================================
// Phase 3 — Server-side lag compensation for hitscan / melee
// =====================================================================
//
// Per-entity ring buffer of recent transforms. When a remote client fires a
// hitscan or melee, the server "rewinds" candidate entities to the pose the
// client was actually rendering at the moment of the click — RTT/2 + the
// client's interp delay (~50 ms) in the past — runs the hit query in that
// rewound frame, then restores present-time poses before applying damage.
//
// This means: "if my crosshair was on the enemy when I clicked, the server
// agrees". Without it, fast-moving enemies at 100 ms ping appear ~0.5–1 m
// further along their path on the server than where the client saw them →
// hits silently miss. Counter-Strike-style: ONLY entity poses are rewound,
// NEVER wall geometry, so you can't shoot through cover that the firer's
// view said wasn't there. Same goes for HP and death state — they stay at
// present-time so a corpse can't be hit retroactively.
//
// Sized to cover ~267 ms (16 ticks at 60 Hz) — comfortably more than any
// realistic (RTT/2 + interp_delay) combination we'd lag-comp for. Larger
// gaps fall back to present-time resolution; the firer's high-ping bullets
// just behave like the un-lag-compensated baseline.
constexpr u32 LAG_COMP_HISTORY_TICKS = 16;
constexpr u32 LAG_COMP_MAX_TICKS     = LAG_COMP_HISTORY_TICKS - 1;
struct EntPoseSnap {
    Vec3 position;
    f32  yaw;
    Vec3 halfExtents;
    u32  tickStamp;   // serverTick at which this entry was written; 0 = unused
};
static EntPoseSnap s_entHistory[MAX_ENTITIES][LAG_COMP_HISTORY_TICKS];
static u8          s_entHistoryHead[MAX_ENTITIES];   // next write index per entity
// Scratch saved-current pose during a beginLagComp / endLagComp window so we can
// restore the live entity pool to present time after the rewound hit query runs.
static EntPoseSnap s_scratchPose[MAX_ENTITIES];
static bool        s_scratchValid[MAX_ENTITIES];

static const EntPoseSnap* findHistoryAt(u32 entIdx, u32 targetTick) {
    if (entIdx >= MAX_ENTITIES) return nullptr;
    const EntPoseSnap* best = nullptr;
    u32 bestDelta = UINT32_MAX;
    for (u32 k = 0; k < LAG_COMP_HISTORY_TICKS; k++) {
        const EntPoseSnap& e = s_entHistory[entIdx][k];
        if (e.tickStamp == 0) continue;
        u32 delta = (e.tickStamp >= targetTick)
            ? (e.tickStamp - targetTick) : (targetTick - e.tickStamp);
        if (delta < bestDelta) { bestDelta = delta; best = &e; }
    }
    return best;
}


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

    // CLIENT-side prediction: the local fire path runs (cooldown/ammo/visuals), but the
    // AUTHORITATIVE fire happens server-side. Send CL_FIRE_WEAPON with the eye position
    // and aim THIS frame — the server fires from those exact values, eliminating the
    // prior bug where the server used drain-derived np.yaw and could fire from a yaw
    // that was seconds stale under input queue lag.
    if (m_netRole == NetRole::CLIENT) {
        sendFireWeapon(eyePos, m_localPlayer.yaw, m_localPlayer.pitch);
    }

    AttackResult result;
    switch (wpn.type) {
    case WeaponType::MELEE: {
        // Crit is now rolled inside Combat::fireMelee from weapon.critChance (set in
        // buildWeaponDef in item.cpp). No per-subtype crit logic needed here.
        WeaponSubtype melSub = WeaponSubtype::NONE;
        if (qbItem && !isItemEmpty(*qbItem))
            melSub = m_itemDefs[qbItem->defId].weaponSubtype;
        if (m_netRole == NetRole::CLIENT) {
            // Phase 2.2 — Client-side melee hit prediction. Cone-query against the
            // INTERPOLATED entity pool (what the player actually sees) and spawn impact
            // FX immediately for each hit. Skip Combat::fireMelee — it would call
            // applyDamage against the local ghost pool (stale, since the client's
            // entity AI doesn't tick) and produce blood at the wrong position. The
            // server resolves the hit authoritatively and ships the real damage
            // number via SV_EVENT::DAMAGE_NUMBER (now reliable, Phase 2.1).
            f32 cosCone = cosf(radians(wpn.coneAngleDeg * 0.5f));
            EntityHandle hits[MAX_ENTITIES];
            f32 distances[MAX_ENTITIES];
            u32 hitCount = CombatQuery::queryConeSorted(
                m_renderInterp.entities, eyePos, forward, cosCone, wpn.range,
                hits, distances, MAX_ENTITIES, /*horizontalCone=*/true);
            for (u32 i = 0; i < hitCount; i++) {
                Entity* e = handleGet(m_renderInterp.entities, hits[i]);
                if (!e) continue;
                for (u32 fx = 0; fx < MAX_IMPACT_FX; fx++) {
                    if (!m_fx.impactFX[fx].active) {
                        Vec3 nrm = eyePos - e->position; nrm.y = 0;
                        f32 l = lengthSq(nrm);
                        nrm = (l > 1e-6f) ? normalize(nrm) : Vec3{0,0,1};
                        m_fx.impactFX[fx] = {e->position, nrm, 0.3f, true, true};
                        break;
                    }
                }
                // M6 — Record this predicted melee hit so M10 can ack or roll back the FX.
                // hits[i].index is the entity slot in the interpolated pool (matches the
                // snapshot entity index the server will reference in SV_DAMAGE_DONE).
                PendingHitRingOps::record(m_pendingHits, m_clientTick, hits[i].index, 0);
            }
            result.didFire     = true;
            result.entitiesHit = hitCount;
            result.hitEntity   = (hitCount > 0);
            m_localPlayer.hitShakeTimer = fmaxf(m_localPlayer.hitShakeTimer, 0.03f);
            // Skip cleave prediction: cleave is 5% RNG and the server's own roll won't
            // match the client's, so a predicted cleave would frequently mispredict.
        } else {
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
            // Phase 2.2 — On CLIENT, predict the hitscan against the INTERPOLATED entity
            // pool (what the player sees) so impact sparks and crosshair "hit" feedback
            // appear within one frame instead of waiting for the server's HITSCAN_IMPACT
            // event (~RTT/2 + snapshot skew). The server still resolves the authoritative
            // hit and ships the real damage number; this is purely a visual prediction.
            // Host / singleplayer still use the canonical Combat::fireHitscan against the
            // authoritative m_entities pool.
            if (m_netRole == NetRole::CLIENT) {
                CombatHit hit = CombatQuery::raycast(m_level.grid, m_renderInterp.entities,
                                                     eyePos, forward, wpn.range);
                result.didFire = true;
                if (hit.hit) {
                    result.hitEntity   = (hit.type == CombatHit::ENTITY);
                    result.hitWorld    = (hit.type == CombatHit::WORLD);
                    result.hitPosition = hit.position;
                    result.hitNormal   = hit.normal;
                    result.hitDistance = hit.distance;
                    result.entitiesHit = (hit.type == CombatHit::ENTITY) ? 1 : 0;
                    // M6 — Record predicted entity hits so M10 can ack or roll them back.
                    // Only record ENTITY hits (WORLD hits have no server-side damage ack).
                    if (hit.type == CombatHit::ENTITY) {
                        PendingHitRingOps::record(m_pendingHits, m_clientTick,
                                                  hit.entityHandle.index, 0);
                    }
                }
            } else {
                result = Combat::fireHitscan(wpn, eyePos, forward, m_level.grid, m_entities);
            }
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
            // V2 fire prediction: on CLIENT, mark this as a locally-predicted ghost so the
            // renderer merges it into m_renderInterp.projectiles and the matching authoritative
            // snapshot projectile despawns it on arrival. clientTick = m_clientTick (the
            // client's monotonic sim counter — M1.8) so the server-stored counterpart will
            // carry the same low-16-bit value back. Host/SP skip this — their fire IS the
            // authoritative one.
            if (m_netRole == NetRole::CLIENT) {
                proj.predicted     = true;
                proj.clientTick    = m_clientTick;  // M1.8: was m_serverTick; use client-local tick
                proj.predictedLife = 0.0f;
            }
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
                        // R17: tempSS.lastActivationTick=0 makes the gate always pass; currentTick is unused
                        // for the gate but threaded for consistency.
                        SkillSystem::tryActivate(tempSS, &procDef, 1,
                            procPos, dir, m_localPlayer.yaw,
                            m_projectiles, m_entities, m_level.grid, m_localPlayer,
                            currentLocalTick());
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
// CL_FIRE_WEAPON server-side intake. Validates the request (cooldown, origin
// sanity) and queues a per-slot pending fire that handleWeaponFireForPlayer
// will consume on the next tick. The yaw/pitch are stored verbatim; the
// origin is clamped to within 1 m of np.eyePos() so a slightly mis-synced
// client position doesn't fire from an unreachable point.
// ---------------------------------------------------------------------------
void Engine::handleFireWeaponRequest(u8 playerSlot, u32 clientTick,
                                     Vec3 claimedOrigin, f32 claimedYaw, f32 claimedPitch) {
    if (playerSlot >= MAX_PLAYERS) return;
    // Phase 1.1 — Squash unreliable-channel retransmits. The client sends the same
    // CL_FIRE_WEAPON packet for ~3 ticks after the original fire; without this,
    // every redundant copy would queue another PendingFire and the weapon would
    // fire 2-4 times for a single trigger. Dedup before any side effect.
    if (fireWasSeen(playerSlot, clientTick)) return;
    recordFire(playerSlot, clientTick);
    NetPlayer& np = m_players[playerSlot];
    if (!np.active || np.isDead) return;            // dead/inactive players don't fire
    // Loose anti-cheat: clamp claimed origin to within 1 m of the server's authoritative
    // eye position. A normal client running at 60 Hz will be within a few cm; clamping
    // (rather than rejecting) keeps the shot landing even if a peer is slightly out of
    // sync, which is preferable to a missed shot in co-op.
    Vec3 svOrigin = np.eyePos();
    Vec3 delta    = claimedOrigin - svOrigin;
    f32  distSq   = delta.x * delta.x + delta.y * delta.y + delta.z * delta.z;
    if (distSq > 1.0f) claimedOrigin = svOrigin;    // > 1 m → snap to server origin
    PendingFire& pending = s_pendingFires[playerSlot];
    pending.valid      = true;
    pending.clientTick = clientTick;
    pending.origin     = claimedOrigin;
    pending.yaw        = claimedYaw;
    pending.pitch      = claimedPitch;
    // Cooldown is checked when handleWeaponFireForPlayer consumes the pending fire — the
    // server's cooldownTimer is decremented continuously by that function, so we don't
    // pre-screen here (would require a redundant cooldown copy).
}

// ---------------------------------------------------------------------------
// M10.1 — Client send path: build and ship a CL_FIRE_WEAPON packet on every local
// fire trigger. Now sent reliable — ENet guarantees delivery so the old manual
// retransmit ring (s_clientFireTx / resendPendingFire) is gone. The server's
// per-(slot,clientTick) dedup ring (s_fireDedupRing) is kept as a cheap guard
// against rapid-fire scenarios where a delayed earlier reliable can still arrive.
// Layout (unchanged):
//   header(4) + clientTick(4) + posX/Y/Z(6) + yawQ(2) + pitchQ(2) = 18 B.
// ---------------------------------------------------------------------------
void Engine::sendFireWeapon(Vec3 origin, f32 yaw, f32 pitch) {
    if (m_netRole != NetRole::CLIENT) return;       // host fires directly via handleWeaponFire
    u8 buf[sizeof(PacketHeader) + 14];
    PacketHeader* hdr = reinterpret_cast<PacketHeader*>(buf);
    hdr->type  = NetPacketType::CL_FIRE_WEAPON;
    hdr->flags = 0;
    hdr->seq   = 0;
    u32 off = sizeof(PacketHeader);
    u32 tick = m_clientTick;                        // M1.8: client-local monotonic tick — dedup key on the server; was m_serverTick
    std::memcpy(buf + off, &tick, 4); off += 4;
    u16 posXQ = Quantize::packPos(origin.x);
    u16 posYQ = Quantize::packPos(origin.y);
    u16 posZQ = Quantize::packPos(origin.z);
    std::memcpy(buf + off, &posXQ, 2); off += 2;
    std::memcpy(buf + off, &posYQ, 2); off += 2;
    std::memcpy(buf + off, &posZQ, 2); off += 2;
    u16 yawQ   = Quantize::packAngle(yaw);
    u16 pitchQ = Quantize::packAngle(pitch);
    std::memcpy(buf + off, &yawQ,   2); off += 2;
    std::memcpy(buf + off, &pitchQ, 2); off += 2;
    // M10.1: reliable — ENet guarantees delivery, manual retransmit removed.
    Net::sendToServer(buf, off, /*reliable=*/true);
}

// Phase 1.1 — Reset server-side CL_FIRE_WEAPON dedup state for a slot. Called by
// onPlayerJoin when a previously-occupied slot is reused (would otherwise carry
// stale ticks from the previous occupant into the new joiner's reused buffer).
void Engine::resetFireDedup(u8 slot) {
    if (slot < MAX_PLAYERS) s_fireDedupRing[slot] = FireDedupRing{};
}

// Phase 1.1 — Reset all dedup rings. Called after a floor descent resets m_serverTick
// to 0 — without this, the post-descent client tick 0/1/2 would collide with whatever
// was in the ring at the end of the prior floor and the first few shots on the new
// floor would be silently squashed as "duplicates".
void Engine::resetAllFireDedup() {
    for (u32 i = 0; i < MAX_PLAYERS; i++) s_fireDedupRing[i] = FireDedupRing{};
    // M10.1: no client-side retransmit state to clear (s_clientFireTx removed).
}

// Phase 3.1 — Push every active entity's current pose into its per-entity history ring.
// Called from serverNetPost AFTER the snapshot is built, so the entry for serverTick T
// represents "what the snapshot for tick T contains" — which is what the client renders
// from. Cheap: a couple of writes per active entity, no allocations.
void Engine::pushEntityHistory() {
    if (m_netRole != NetRole::SERVER) return;
    for (u32 i = 0; i < MAX_ENTITIES; i++) {
        Entity& e = m_entities.entities[i];
        if (!(e.flags & ENT_ACTIVE)) continue;
        u8 h = s_entHistoryHead[i];
        s_entHistory[i][h].position    = e.position;
        s_entHistory[i][h].yaw         = e.yaw;
        s_entHistory[i][h].halfExtents = e.halfExtents;
        s_entHistory[i][h].tickStamp   = (m_serverTick == 0) ? 1u : m_serverTick;
        s_entHistoryHead[i] = (h + 1) % LAG_COMP_HISTORY_TICKS;
    }
}

// Phase 3.1 — Wipe the entire history ring (and any in-flight scratch). Called from
// startGame so a new floor doesn't see entries from the prior floor's entities at the
// same pool indices (which would be in geometrically unrelated rooms). Also wipes
// scratch state defensively.
void Engine::resetEntityHistory() {
    for (u32 i = 0; i < MAX_ENTITIES; i++) {
        for (u32 k = 0; k < LAG_COMP_HISTORY_TICKS; k++) s_entHistory[i][k] = EntPoseSnap{};
        s_entHistoryHead[i] = 0;
        s_scratchValid[i] = false;
    }
}

// Phase 3.1 — Compute how many ticks to rewind for a given firing slot. Uses ENet's
// smoothed RTT (already populated in NetPlayerSlot::rttMs by Net::poll) + the client's
// interp delay. Clamped to LAG_COMP_MAX_TICKS so out-of-window RTTs gracefully fall
// back to present-time hit detection rather than reading uninitialized history.
u32 Engine::computeLagCompTicks(u8 slot) const {
    if (slot >= MAX_PLAYERS) return 0;
    if (slot == 0) return 0;                // host: no network delay to compensate
    f32 rttMs = 0.0f;
    const NetPlayerSlot* slots = Net::getSlots();
    if (slots) rttMs = slots[slot].rttMs;
    // Client renders at server snapshot time - INTERP_DELAY_SEC (50 ms = 3 ticks at 60 Hz).
    // Server processes the fire RTT/2 after the client clicked, so we rewind by
    // RTT/2 in ticks PLUS the client's interp delay.
    constexpr f32 TICK_MS    = 1000.0f / NET_TICK_RATE;     // 16.67 ms
    constexpr f32 INTERP_TICKS = 0.05f * NET_TICK_RATE;     // 3
    f32 rttHalfTicks = (rttMs * 0.5f) / TICK_MS;
    f32 ticks = rttHalfTicks + INTERP_TICKS;
    if (ticks < 0.0f) ticks = 0.0f;
    u32 t = static_cast<u32>(ticks + 0.5f);
    if (t > LAG_COMP_MAX_TICKS) t = 0;       // out-of-range: don't lag-comp this fire
    return t;
}

// Phase 3.2 — Swap every active entity's transform to its pose `ticksAgo` ticks in
// the past, saving the present pose to scratch. Combat queries that follow read the
// historical poses; applyDamage / death checks read the live HP/flags (which we
// intentionally do NOT rewind — corpses can't be hit retroactively, and the server
// is still authoritative for damage). Call endLagComp before any state observable
// outside the fire handler (snapshot build, AI tick, etc.).
void Engine::beginLagComp(u32 ticksAgo) {
    if (m_netRole != NetRole::SERVER) return;
    if (ticksAgo == 0) return;               // no rewind needed
    u32 targetTick = (m_serverTick > ticksAgo) ? (m_serverTick - ticksAgo) : 0;
    for (u32 i = 0; i < MAX_ENTITIES; i++) {
        Entity& e = m_entities.entities[i];
        s_scratchValid[i] = false;
        if (!(e.flags & ENT_ACTIVE)) continue;
        if (e.flags & ENT_DEAD) continue;    // skip corpses — present-time dead means dead
        const EntPoseSnap* hist = findHistoryAt(i, targetTick);
        if (!hist) continue;
        s_scratchPose[i].position    = e.position;
        s_scratchPose[i].yaw         = e.yaw;
        s_scratchPose[i].halfExtents = e.halfExtents;
        s_scratchValid[i]            = true;
        e.position    = hist->position;
        e.yaw         = hist->yaw;
        e.halfExtents = hist->halfExtents;
    }
}

void Engine::endLagComp() {
    if (m_netRole != NetRole::SERVER) return;
    for (u32 i = 0; i < MAX_ENTITIES; i++) {
        if (!s_scratchValid[i]) continue;
        Entity& e = m_entities.entities[i];
        e.position    = s_scratchPose[i].position;
        e.yaw         = s_scratchPose[i].yaw;
        e.halfExtents = s_scratchPose[i].halfExtents;
        s_scratchValid[i] = false;
    }
}

// R6: lag-comp the obstacle list for player movement so server's per-input moveAndSlide
// sees the same entity positions the client interpolated against when capturing the input.
// Without this, the client's interp pool (33 ms / 2 ticks behind live) disagrees with
// the server's live entity pool whenever an enemy is moving near the player, producing
// occasional ~1-tick position divergence that fires the M3.2 reconcile path and
// produces visible "intermittent jitter". Pure read of the s_entHistory ring — no
// mutation, no scratch save/restore — so call sites don't need begin/end bracketing.
void Engine::buildLagCompPlayerObstacles(u32 targetSnapTick,
                                         CollisionObstacle* out,
                                         u32& outCount) const {
    outCount = 0;
    // Live-entity filter mirrors the host's gameUpdate pass: skip inactive, dead,
    // friendly NPCs, and props (none of which should block player movement).
    for (u32 ei = 0; ei < MAX_ENTITIES; ei++) {
        const Entity& e = m_entities.entities[ei];
        if (!(e.flags & ENT_ACTIVE) || (e.flags & ENT_DEAD)) continue;
        if (e.flags & ENT_FRIENDLY) continue;
        if (e.enemyType == EnemyType::PROP) continue;
        const EntPoseSnap* hist = findHistoryAt(ei, targetSnapTick);
        if (hist && hist->tickStamp != 0) {
            // Closest historical pose to the client's interp tick — both sides now
            // see the same world for collision purposes.
            out[outCount++] = {hist->position, hist->halfExtents};
        } else {
            // Ring not populated for any tick yet (first ~2 ticks after a join, or
            // immediately after resetEntityHistory). Falling back to live pose keeps
            // pre-fix behavior in that narrow window; subsequent inputs use the
            // correct rewound pose once pushEntityHistory has run.
            out[outCount++] = {e.position, e.halfExtents};
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

    // Fire trigger: CL_FIRE_WEAPON queued a pending request with the client's CLAIMED origin
    // and aim. The drain-derived np.yaw is no longer used to fire (it could lag by seconds
    // under input queue jitter — see PendingFire comment at top of this file). If no pending
    // fire is queued OR cooldown is still active, return early. Cooldown drift between client
    // and server is normally sub-frame; rare over-the-cooldown fires get dropped here.
    PendingFire& pending = s_pendingFires[np.slotIndex];
    if (!pending.valid) return;
    // Phase 1.2 — Small cooldown grace window. The local client predicts its own
    // cooldown identically to the server, but network jitter (especially on the
    // CL_FIRE_WEAPON for the PREVIOUS shot taking a longer path than this one) can
    // leave the server's cooldown countdown lagging by tens of milliseconds. Without
    // the grace, a perfectly-timed follow-up shot is silently dropped here and the
    // client's predicted-ghost projectile auto-despawns at 0.5 s with no match,
    // showing as a vanishing projectile. The grace is far smaller than any weapon's
    // cooldown so it can't be exploited to cheat fire rate.
    constexpr f32 COOLDOWN_GRACE = 0.05f;
    if (ws.cooldownTimer > COOLDOWN_GRACE) { pending.valid = false; return; }
    // Snapshot the request and immediately clear so a second concurrent request can queue.
    Vec3  claimedOrigin     = pending.origin;
    f32   claimedYaw        = pending.yaw;
    f32   claimedPitch      = pending.pitch;
    u32   pendingClientTick = pending.clientTick;
    pending.valid = false;

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

    // Fire from the CLIENT'S claimed origin + aim (queued via CL_FIRE_WEAPON). The origin
    // is already clamped to within 1 m of np.position by handleFireWeaponRequest, so it's
    // safe to trust. The aim is whatever the player's crosshair was pointing at when they
    // clicked — eliminates the prior np.yaw staleness under input queue lag.
    Vec3 eyePos = claimedOrigin;
    Vec3 forward = normalize(Vec3{
        -sinf(claimedYaw) * cosf(claimedPitch),
         sinf(claimedPitch),
        -cosf(claimedYaw) * cosf(claimedPitch)
    });

    // Phase 3.2 / M5 — Lag-comp now covers ALL weapon types including PROJECTILE.
    // For melee/hitscan, rewinding entity poses is what makes hit detection match
    // what the firing client saw. For projectiles, the spawn position is
    // claimedOrigin (already the client's lag-adjusted eye position clamped to
    // within 1 m of np.position), so eyePos is correct regardless of rewind order.
    // Wrapping PROJECTILE in the same window establishes a consistent pattern for
    // future AOE lag-comp (M9) and ensures any per-spawn proximity checks also see
    // rewound poses. endLagComp() below is already unconditional on lagCompTicks > 0.
    u32 lagCompTicks = 0;
    lagCompTicks = computeLagCompTicks(np.slotIndex);
    if (lagCompTicks > 0) beginLagComp(lagCompTicks);

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
            // V2 fire prediction: stamp the firing client's tick onto the authoritative
            // projectile. The snapshot's clientTickLow carries the low 16 bits back to
            // that client so it can match-and-despawn its local predicted ghost.
            proj.clientTick = pendingClientTick;
        }
        result.hitEntity = false; // procs handled by projectile hit callback
    } break;
    }

    // Phase 3.2 — Restore present-time entity poses if we rewound earlier. Must run
    // before any code that observes entity positions (procs below trigger on hits,
    // which are already determined; the post-fire on-hit / on-kill paths spawn things
    // at entity positions and we want those to use the present-time pose).
    if (lagCompTicks > 0) endLagComp();

    // M10.2 — Emit SV_DAMAGE_DONE to the firing client for each entity hit so the
    // client can ack its PendingHitRing entry and clean up the predicted hit-marker.
    // Only emitted on a SERVER for remote (non-host) clients — slot 0 is the host
    // and fires locally without a network round-trip. Uses Net::sendReliable so the
    // ack is guaranteed, matching the reliable CL_FIRE_WEAPON that triggered this fire.
    if (m_netRole == NetRole::SERVER && np.slotIndex != 0 && result.hitEntity) {
        u32 hitCount = result.entitiesHit < MAX_ATTACK_HITS ? result.entitiesHit : MAX_ATTACK_HITS;
        for (u32 hi = 0; hi < hitCount; hi++) {
            u16 targetIdx = result.hitHandles[hi].index;
            u8  svBuf[sizeof(PacketHeader) + 8]; // header(4) + clientTick(4) + targetIdx(2) + reserved(2)
            PacketHeader* svHdr = reinterpret_cast<PacketHeader*>(svBuf);
            svHdr->type  = NetPacketType::SV_DAMAGE_DONE;
            svHdr->flags = 0;
            svHdr->seq   = 0;
            u32 off = sizeof(PacketHeader);
            std::memcpy(svBuf + off, &pendingClientTick, 4); off += 4;
            std::memcpy(svBuf + off, &targetIdx,         2); off += 2;
            u16 reserved = 0;
            std::memcpy(svBuf + off, &reserved,          2); off += 2;
            Net::sendReliable(np.slotIndex, svBuf, off);
        }
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
                        // R17: tempSS.lastActivationTick=0 always-pass; tick threaded for consistency.
                        SkillSystem::tryActivate(tempSS, &procDef, 1,
                            procPos, forward, np.yaw,
                            m_projectiles, m_entities, m_level.grid, m_localPlayer,
                            currentLocalTick());
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
                            // Center the scorch fan on the CLIENT'S claimed aim, same as the
                            // fire direction above — keeping these in sync prevents the procs
                            // from drifting off the user's actual crosshair under input lag.
                            f32 angle = claimedYaw + t * halfAngle;
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
