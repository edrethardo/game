// Engine module — see engine.h for class definition.
// Split from engine.cpp for manageability. All methods are Engine:: members.

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "engine/engine.h"
#include "platform/window.h"
#include "platform/clock.h"
#include "platform/input.h"
#include "platform/steam.h"
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
#include "game/static_charge.h"   // Capacitor Mail stacks for REMOTE lanes (serverNetPost)
#include "game/skill.h"
#include "game/inventory_ui.h"
#include "game/game_constants.h"
#include "audio/audio.h" // AudioSystem::stopMusic on host-left save-and-return-to-menu
#include "net/net.h"
#include "net/server.h"
#include "net/client.h"
#include "net/lag_comp.h"   // client-reported interp delay -> server rewind (shared contract)
#include "net/snapshot.h"
#include "net/packet.h"
#include "net/render_offset.h"
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
// Shared statics defined in engine.cpp
extern Engine* s_engine;
extern FrameAllocator s_frameAllocator;
extern bool s_firstKillDropGiven;


// ---------------------------------------------------------------------------
// Server networking — pre-gameplay: process remote inputs, weapon fire
// ---------------------------------------------------------------------------
void Engine::serverNetPre(f32 dt) {
    // D5/M14: sync the fake-latency + fake-loss cvars into the net layer, then flush any delayed
    // packets whose delivery timestamp has elapsed. Done first so a queued snapshot
    // from the prior frame arrives before we build + broadcast the new one.
    // (The loss push was MISSING until the --net-loss flag landed: Net::setFakeLossPct existed
    // with zero callers, so the whole M14 loss harness was unreachable at runtime.)
    Net::setFakeLatencyMs(m_netFakeLatencyMs);
    Net::setFakeJitterMs(m_netFakeJitterMs);
    Net::setFakeLossPct(m_netFakeLossPct);
    Net::pumpDelayQueue();

    m_serverTick++;

    // Capture local input and push into server's input buffer. Pass m_localPlayer
    // so captureLocalInput can pack absolute yaw/pitch/position (PROTOCOL_VERSION 2
    // wire layout); the host's own input still rides the same NetInput shape so the
    // server's snapshot lastProcessedInputTick is uniform across all slots.
    WeaponState& ws = m_players[m_localPlayerIndex].weaponState;
    NetInput localInput = PlayerController::captureLocalInput(m_localPlayer, m_serverTick, ws.currentWeapon);
    Server::getInputBuffer(m_localPlayerIndex).push(localInput);
    // Track the host's own ack too. The host moves in gameUpdate (not the remote loop
    // below), so without this its lastProcessedInputTick stays 0 forever on the wire — harmless
    // today (clients only reconcile their own slot) but a trap for any future feature
    // that reads slot 0's ack (spectate/lag-comp/debug).
    m_players[m_localPlayerIndex].lastProcessedInputTick = localInput.clientTick;

    // R17: per-tick cooldown drains removed. Cooldown gating is now tick-based —
    // both client and server evaluate `(input.clientTick - lastActivationTick) >=
    // cooldownTicks` using identical formulas on identical data. The f32
    // cooldownTimer fields on SkillState / NetPlayer.potionCooldown are now
    // HUD-derived only (re-computed each frame from lastActivationTick); no
    // f32-drift gate to maintain. (dt is still used below for moveAndSlide and
    // handleWeaponFireForPlayer.)

    // Process inputs for remote players only (host movement handled by gameUpdate).
    //
    // Drain ALL unprocessed inputs in tick order, not just `getLatest()`. The prior
    // code dropped every input that arrived between two server ticks except the
    // newest one — their mouse deltas (and one-shot fire/jump/dodge flags) were
    // silently discarded. That caused the server's NetPlayer.yaw to drift behind the
    // client's live camera yaw — visible as "shooting where I'm not aiming" — plus
    // position drift that fed reconcile snaps (felt as lag and jitter on the client).
    //
    // Walking the ring oldest→newest is correct because push() rejects stale ticks
    // (net_player.h:139-142), so the buffer is monotonically ordered by tick. The
    // buffer holds INPUT_BUFFER_SIZE=64 inputs (~1 s at 60 Hz), which exceeds any
    // realistic jitter window — overflow would silently overwrite the oldest, but
    // that's a packet-loss scenario already handled by the snapshot reconcile path.
    // R6: obstacle list is now built PER INPUT, lag-comp-rewound to the tick the
    // client interpolated against when it captured the input. The earlier "build
    // once per server tick from live entities" optimization (b524abe) used live
    // positions, which disagreed with the client's interp-pool view (33 ms behind)
    // whenever an enemy was moving — producing occasional ~1-tick reconcile diffs
    // and visible jitter. Per-input rebuild costs O(MAX_ENTITIES × inputs-this-tick);
    // negligible at our scale (≤MAX_ENTITIES × 6 per server tick under jitter).
    // The rewind amount is no longer a server-side constant: each input carries the interp delay
    // the client actually used (in.interpDelayMs), because that delay is ADAPTIVE and a hardcoded
    // guess is wrong for every client that isn't sitting exactly at the jitter floor. See lag_comp.h.

    for (u32 i = 0; i < MAX_PLAYERS; i++) {
        // Skip ALL host-local lanes — they move in gameUpdate, not from a network buffer. Slots
        // 0..m_splitPlayerCount-1 are the host's local players (slot 0 = host, slot 1 = couch
        // partner in online couch co-op). For a normal host (count 1) this is just slot 0.
        if (i < m_splitPlayerCount) continue;
        NetPlayer& np = m_players[i];
        if (!np.active) { m_starvedRepeats[i] = 0; continue; }
        InputRingBuffer& buf = Server::getInputBuffer(i);
        bool appliedRealInput = false;   // did any FRESH input drive this slot this tick?
        for (u32 k = 0; k < buf.count; k++) {
            // oldest→newest walk: head points one past the newest write; count entries trail back from there
            u32 idx = (buf.head + INPUT_BUFFER_SIZE - buf.count + k) % INPUT_BUFFER_SIZE;
            const NetInput& in = buf.inputs[idx];
            if (in.clientTick <= np.lastProcessedInputTick) {
                // Movement for this tick is already covered — either it was genuinely applied, or
                // the dry-out coast below CLAIMED it (advanced the watermark) while approximating
                // the lost input. A coast never fires activation edges though, so a late-arriving
                // press (potion, skill) must still fire — exactly once, which is what the separate
                // m_lastActivationTick watermark guarantees (each tick enters the ring once; this
                // watermark stops a re-walk of the ring from re-firing it).
                if (!np.isDead && in.clientTick > m_lastActivationTick[i]) {
                    processRemoteActivation(static_cast<u8>(i), in, dt);
                    m_lastActivationTick[i] = in.clientTick;
                }
                continue;
            }
            // Always advance the ack and mirror the weapon, even while dead, so the
            // client's reconcile keeps a fresh ack across the death window. But DON'T
            // drive movement/aim from input while dead — a corpse that died holding a
            // move key or aiming somewhere would keep updating each tick.
            np.lastProcessedInputTick = in.clientTick;
            // M11.2 — Track which snapshot this client has applied for delta-compression
            // baseline decisions. ackedSnapshotTick on the wire is u16 (low 16 bits of
            // serverTick); reconstruct the full u32 using the current m_serverTick high bits,
            // then correct for the wrap-around case where the client's ack belongs to the
            // previous 64 K window.
            {
                u16 lowAck = in.ackedSnapshotTick;
                if (lowAck == 0) {
                    // 0 is the client's "nothing decoded yet" sentinel (s_lastDecodedTick starts
                    // at 0). Reconstructing it against the high bits would fabricate tick 65536·k
                    // once a floor runs past ~18 min — and while that alias sits inside the
                    // 32-deep history ring, the server would delta against a baseline the client
                    // never decoded (dropped client-side until the ring ages it out). Honoring
                    // the sentinel costs one extra full snapshot in the opposite corner: a real
                    // ack landing exactly on a 65536 multiple (~once per 18 min per client).
                    m_clientAckedSnap[i] = 0;
                } else {
                    u32 fullAck = (m_serverTick & ~0xFFFFu) | lowAck;
                    if (fullAck > m_serverTick) fullAck -= 0x10000; // client ack is from prior window
                    m_clientAckedSnap[i] = fullAck;
                }
            }
            if (in.weaponId < m_weaponDefCount)
                np.weaponState.currentWeapon = in.weaponId;
            if (!np.isDead) {
                appliedRealInput = true;
                // SERVER-AUTHORITATIVE POSITION (M2+): server runs PlayerController on the
                // remote slot — updateNetPlayerFromInput computes yaw/pitch from the absolute
                // quantized values in the input, then drives applyMovement to produce the new
                // np.velocity. Position integration happens immediately below, per input.
                PlayerController::updateNetPlayerFromInput(np, in, dt);

                // Integrate np.position by THIS input's velocity for one client-tick (dt).
                // Per-input cadence matches the client's per-tick gameUpdate (R3, b524abe).
                // Obstacle list is lag-comp-rewound to the tick the client interpolated
                // against when capturing this input (R6) — without that rewind, server's
                // live entity pool disagreed with the client's 33ms-behind interp pool and
                // produced intermittent reconcile jitter near moving enemies.
                if (!np.noclip) {
                    // Fractional target tick from the delay THIS input reported — the exact
                    // instant of world-state the client's own moveAndSlide collided against.
                    const f32 targetSnapTickF =
                        LagComp::targetTick(m_clientAckedSnap[i], in.interpDelayMs);
                    CollisionObstacle obs[MAX_ENTITIES];
                    u32 obsCount = 0;
                    buildLagCompPlayerObstacles(targetSnapTickF, obs, obsCount);

                    Player tempP;
                    tempP.position = np.position;
                    tempP.velocity = np.velocity;
                    tempP.onGround = np.onGround;
                    tempP.noclip   = false;
                    Collision::moveAndSlide(tempP, m_level.grid, dt, obs, obsCount);
                    np.position = tempP.position;
                    np.velocity = tempP.velocity;
                    np.onGround = tempP.onGround;
                }

                // Activation EDGES (potion + class/boot/helm skills) are processed HERE,
                // per-input, inside the same drain that walks every unprocessed input — NOT
                // via getLatest() outside the loop. extFlags bits are set for exactly one
                // client tick per press; under jitter ≥2 inputs land between server ticks, so
                // getLatest() (newest only) silently dropped the press whenever a newer input
                // arrived first. Walking every input fixes that, and np.yaw/pitch already hold
                // THIS input's aim (set by updateNetPlayerFromInput above) so skills fire from
                // the pose the client held at press time. push()+cursor guarantee once-only.
                processRemoteActivation(static_cast<u8>(i), in, dt);
                m_lastActivationTick[i] = in.clientTick;
            }
        }

        if (appliedRealInput) {
            m_starvedRepeats[i] = 0;   // link recovered — coasting budget refills
        } else if (!np.isDead) {
            // ---- Input dry-out: keep a starved player moving (last-input coast) -------------
            // Under burst loss (>8 consecutive CL_INPUT packets: WiFi fade, route flap) the
            // buffer runs dry and this player used to STATUE-FREEZE on the host — not even
            // gravity — while their own client kept predicting; the divergence blew past 1 m
            // and hard-snapped them back (16 teleport snaps in a 90 s soak at 45% loss).
            //
            // Coast on the last known input instead, for up to 250 ms — and CLAIM the tick by
            // advancing lastProcessedInputTick. The claim is the load-bearing part, twice over:
            //   * the snapshot's position now stays consistent with its ack tag. The first cut
            //     of this feature moved the player but not the watermark, so every snapshot
            //     reported a coasted position against a pre-coast ack — the client compared it
            //     against the wrong ring entry, "corrected", double-counted the movement, and
            //     the soak got WORSE (237 snaps). Time-tag integrity is not optional.
            //   * a late-delivered real input for a claimed tick is then dropped by the
            //     ordinary monotonic check above — no double integration, no debt bookkeeping.
            //     Only its activation edge still fires (the m_lastActivationTick branch).
            // Edge bits are stripped — a coast must never re-jump or re-fire an activation.
            // Held-key movement is almost always exactly what the lost input contained, so the
            // approximation is usually perfect; a mid-gap direction change surfaces as a small
            // reconcile correction on the client, which the replay path absorbs.
            constexpr u8 STARVE_REPEAT_CAP = 15;   // 250 ms of coasting, then freezing is honest
            const NetInput* last = buf.getLatest();
            if (last && m_starvedRepeats[i] < STARVE_REPEAT_CAP) {
                NetInput synth = *last;
                synth.moveFlags &= static_cast<u8>(~INPUT_JUMP);
                synth.extFlags   = 0;
                PlayerController::updateNetPlayerFromInput(np, synth, dt, /*movementOnly=*/true);
                if (!np.noclip) {
                    // Same integration step as a real input, lag-comp obstacles included.
                    const f32 targetSnapTickF =
                        LagComp::targetTick(m_clientAckedSnap[i], synth.interpDelayMs);
                    CollisionObstacle obs[MAX_ENTITIES];
                    u32 obsCount = 0;
                    buildLagCompPlayerObstacles(targetSnapTickF, obs, obsCount);
                    Player tempP;
                    tempP.position = np.position;
                    tempP.velocity = np.velocity;
                    tempP.onGround = np.onGround;
                    tempP.noclip   = false;
                    Collision::moveAndSlide(tempP, m_level.grid, dt, obs, obsCount);
                    np.position = tempP.position;
                    np.velocity = tempP.velocity;
                    np.onGround = tempP.onGround;
                }
                np.lastProcessedInputTick++;   // claim the approximated tick (see above)
                m_starvedRepeats[i]++;
            }
        }
    }

    // Remote player weapon fire (server-authoritative). FIRE rides the reliable
    // CL_FIRE_WEAPON channel (PendingFire), not an input edge bit, so it's immune to the
    // getLatest() edge-drop and stays in its own per-tick loop. Activation edges moved
    // into the drain loop above.
    for (u32 i = 0; i < MAX_PLAYERS; i++) {
        if (!m_players[i].active) continue;
        if (i < m_splitPlayerCount) continue; // all host-local lanes fire via gameUpdate's local path
        // Don't let a dead remote keep firing: a corpse that died holding FIRE would shoot
        // every tick until it respawns.
        if (!m_players[i].isDead) handleWeaponFireForPlayer(m_players[i], dt);
    }
}

// ---------------------------------------------------------------------------
// processRemoteActivation — apply ONE remote input's activation edges (potion +
// class/boot/helm skills). Called per-input from serverNetPre's drain loop, so
// every press is processed exactly once even under jitter (the old getLatest()
// read saw only the newest buffered input and dropped edges riding an older one —
// the client unreliable-activation bug). Caller guarantees !isDead and that this
// is a remote slot (host activates in gameUpdate). np.yaw/pitch already reflect
// THIS input's aim (updateNetPlayerFromInput ran first), so skills fire from the
// pose the client held at press time. `in.clientTick` drives the same tick-based
// cooldown gate the client predicted with (GameConst::cooldownReady).
// ---------------------------------------------------------------------------
void Engine::processRemoteActivation(u8 slot, const NetInput& in, f32 /*dt*/) {
    const u32 i = slot; // keep the original per-slot indexing identical to the old block

    // Potion (tick-based gate against the remote's clientTick frame).
    if (in.extFlags & INPUT_EX_POTION) {
        // potion cooldown reduction is 10% of itemCdr (matches client). Both sides compute
        // the same cooldownTicks; CL_INVENTORY_SYNC keeps bonusCooldownReduction in step.
        const f32 cdr = m_inventories[i].bonusCooldownReduction * 0.1f;
        const u32 cooldownTicks = static_cast<u32>(GameConst::POTION_COOLDOWN * (1.0f - cdr) * 60.0f + 0.5f);
        const u32 prev = m_players[i].potionLastActivationTick;
        // Same lenient gate the client predicts with — the grace absorbs a few ticks of
        // client-ahead skew so a legit potion press isn't rejected here while the client
        // already healed locally.
        if (GameConst::cooldownReady(in.clientTick, prev, cooldownTicks)) {
            f32 healAmt = m_players[i].maxHealth * GameConst::POTION_HEAL_PCT;
            m_players[i].health += healAmt;
            if (m_players[i].health > m_players[i].maxHealth)
                m_players[i].health = m_players[i].maxHealth;
            m_players[i].potionLastActivationTick = (in.clientTick == 0) ? 1u : in.clientTick;
            // HUD-derived (server tracks for snapshot completeness; the client derives its own).
            m_players[i].potionCooldown = static_cast<f32>(cooldownTicks) * (1.0f / 60.0f);
        }
    }

    // (Pet-consumable use no longer rides the input stream: with one pet per enemy the edge
    // must name WHICH def, so it moved to the reliable CL_USE_PET packet — Engine::onUsePet.)

    // Equipment skills (F = boots, G = helmet)
    if (in.extFlags & INPUT_EX_BOOT_SKILL) {
        const ItemInstance& boots = m_inventories[i].equipped[static_cast<u32>(ItemSlot::BOOTS)];
        if (!isItemEmpty(boots) && boots.rarity == Rarity::LEGENDARY) {
            SkillId bootSkill = m_itemDefs[boots.defId].legendarySkillId;
            if (bootSkill != SkillId::NONE) {
                // R9: use the persistent per-slot SkillState so tryActivate's cooldown
                // gate (cooldownTimer > 0 → reject) actually blocks spam. A throwaway
                // SkillState resets cooldownTimer to 0 every call and silently lets
                // every press through.
                SkillState& ss = m_bootSkillStates[i];
                ss.activeSkill = bootSkill;
                // energy=999 → tryActivate's energy gate is a no-op here; energy is enforced on
                // the firing client (clientNetPre wire-mask strips the bit when unaffordable).
                ss.energy = 999.0f; ss.maxEnergy = 999.0f;
                Vec3 ep = m_players[i].eyePos();
                Vec3 fwd = normalize(Vec3{-sinf(m_players[i].yaw)*cosf(m_players[i].pitch),
                                            sinf(m_players[i].pitch),
                                           -cosf(m_players[i].yaw)*cosf(m_players[i].pitch)});
                // TA-7: set skill-scaling globals from the REMOTE's own data so the
                // guest's skill damage isn't inherited from the host's last cast.
                // Item skills scale by item level (boots) and use base class damage (1.0).
                SkillSystem::setSkillPower(boots.itemLevel > 1
                    ? static_cast<f32>(boots.itemLevel - 1) / 149.0f : 0.0f);
                applySpellScaling(m_inventories[i], 1.0f,
                    Shrine::spellShrinePct(m_players[i].shrineBuff, m_players[i].shrineBuffTimer));
                // Stamp the remote's net slot so per-slot skill state (overcharge buff,
                // meteor/holy kill-heal target) lands on the actual caster, not the host.
                // (H4/H5: overcharge arrays are MAX_PLAYERS-sized; updateMeteors resolves
                //  the heal target against m_players by net slot.)
                SkillSystem::setCastingPlayer(static_cast<u8>(i));
                // TA-3: cast against the GUEST's own view, not the host's m_localPlayer,
                // so position/health-mutating skills (PhaseDash, Blood Nova) hit the guest.
                Player view; buildRemotePlayerView(static_cast<u8>(i), view);
                // R17: tick gate uses the input's clientTick (this remote's frame).
                // Matches what the remote client used locally — no divergence.
                SkillSystem::tryActivate(ss, m_skillDefs, m_skillDefCount,
                                          ep, fwd, m_players[i].yaw,
                                          m_projectiles, m_entities, m_level.grid, view,
                                          in.clientTick,
                                          m_inventories[i].bonusCooldownReduction);
                applyRemotePlayerView(view, static_cast<u8>(i));
            }
        }
    }
    if (in.extFlags & INPUT_EX_HELM_SKILL) {
        const ItemInstance& helm = m_inventories[i].equipped[static_cast<u32>(ItemSlot::HELMET)];
        if (!isItemEmpty(helm) && helm.rarity == Rarity::LEGENDARY) {
            SkillId helmSkill = m_itemDefs[helm.defId].legendarySkillId;
            if (helmSkill != SkillId::NONE) {
                // R9: persistent SkillState so tryActivate's cooldown gate engages.
                SkillState& ss = m_helmetSkillStates[i];
                ss.activeSkill = helmSkill;
                // energy=999 → no-op energy gate; enforced client-side (clientNetPre wire-mask).
                ss.energy = 999.0f; ss.maxEnergy = 999.0f;
                Vec3 ep = m_players[i].eyePos();
                Vec3 fwd = normalize(Vec3{-sinf(m_players[i].yaw)*cosf(m_players[i].pitch),
                                            sinf(m_players[i].pitch),
                                           -cosf(m_players[i].yaw)*cosf(m_players[i].pitch)});
                // TA-7: scale from the REMOTE's own helmet item level (see boot note).
                SkillSystem::setSkillPower(helm.itemLevel > 1
                    ? static_cast<f32>(helm.itemLevel - 1) / 149.0f : 0.0f);
                SkillSystem::setClassDamageMult(1.0f);
                SkillSystem::setCastingPlayer(static_cast<u8>(i)); // caster's net slot (H4/H5)
                // TA-3: cast against the GUEST's own view (see boot-skill note above).
                Player view; buildRemotePlayerView(static_cast<u8>(i), view);
                // R17: same tick-gate as boot — input.clientTick is the remote's frame.
                SkillSystem::tryActivate(ss, m_skillDefs, m_skillDefCount,
                                          ep, fwd, m_players[i].yaw,
                                          m_projectiles, m_entities, m_level.grid, view,
                                          in.clientTick,
                                          m_inventories[i].bonusCooldownReduction);
                applyRemotePlayerView(view, static_cast<u8>(i));
            }
        }
    }

    // Class skill activation (right-click) — use remote player's class.
    if (in.extFlags & INPUT_EX_SKILL) {
        u8 cskSlot = in.skillSlot;
        PlayerClass remoteClass = m_players[i].playerClass;
        if (cskSlot < 4 && static_cast<u32>(remoteClass) < static_cast<u32>(PlayerClass::CLASS_COUNT)) {
            const ClassDef& cls = kClassDefs[static_cast<u32>(remoteClass)];
            // Mirror the host's effectiveFloor gate (engine_update_skills.cpp):
            // difficulty adds +50/floor so remote clients unlock skills at the
            // same depth their own HUD shows, instead of the raw floor.
            u32 effectiveFloor = m_level.currentFloor + m_difficulty * 50;
            if (effectiveFloor >= cls.skillUnlockFloor[cskSlot]) {
                // R9: persistent per-net-slot, per-class-slot state so the cooldown
                // gate in SkillSystem::tryActivate (cooldownTimer > 0 → reject)
                // actually fires. The old throwaway SkillState reset cooldownTimer
                // to 0 every press and let spam through.
                SkillState& tempSS = m_classSkillStatesNet[i][cskSlot];
                tempSS.activeSkill = cls.skills[cskSlot];
                // Energy is NOT enforced here (server has no copy of the remote's pool — energy
                // has no snapshot field). 999 makes tryActivate's energy gate a no-op; the real
                // enforcement is the firing client's wire-mask (clientNetPre), which strips
                // INPUT_EX_SKILL when the local pool can't afford the skill, so an unaffordable
                // press never reaches here. Cooldown IS enforced server-side via the persistent
                // m_classSkillStatesNet tick gate above. (Cheat-resistance for energy would need
                // a server-side per-slot energy sim; deferred — it risks float-regen drift that
                // would wrongly reject legit client-predicted casts.)
                tempSS.energy = 999.0f;
                tempSS.maxEnergy = 999.0f;
                Vec3 eyePos = m_players[i].eyePos();
                Vec3 fwd = normalize(Vec3{-sinf(m_players[i].yaw) * cosf(m_players[i].pitch),
                                            sinf(m_players[i].pitch),
                                           -cosf(m_players[i].yaw) * cosf(m_players[i].pitch)});
                // TA-7: set skill-scaling globals from the REMOTE's own data (mirror the
                // host's class-skill block in engine_update_skills.cpp) so the guest's
                // skill damage uses its own floor/weapon, not the host's last cast.
                SkillSystem::setSkillPower(0.0f); // class skills use base power
                // Class skill damage scales 6% per effective floor (reuse effectiveFloor above).
                applySpellScaling(m_inventories[i], 1.0f + (effectiveFloor - 1) * 0.06f,
                    Shrine::spellShrinePct(m_players[i].shrineBuff, m_players[i].shrineBuffTimer));
                // Weapon damage for Marksman skills that scale off the equipped weapon.
                { const ItemInstance& wpn = m_inventories[i].equipped[static_cast<u32>(ItemSlot::WEAPON)];
                  WeaponDef wd = !isItemEmpty(wpn)
                      ? Inventory::getWeaponFromItem(m_inventories[i], m_itemDefs, wpn)
                      : m_weaponDefs[0];
                  SkillSystem::setWeaponDamage(wd.damage);
                  // Barrage projectile mesh — set on the REMOTE-cast path too (same rule as the
                  // Thunderclap upgrade below: context set only locally leaves guests different).
                  SkillSystem::setWeaponProjectileMesh(
                      (!isItemEmpty(wpn) && m_itemDefs[wpn.defId].weaponSubtype == WeaponSubtype::CROSSBOW)
                          ? m_meshIdBolt : m_meshIdArrow); }
                SkillSystem::setCastingPlayer(static_cast<u8>(i)); // caster's net slot (H4/H5)
                // Thunderclap's floor upgrade — applied here too, not just on the host. Without
                // this a GUEST's Warrior cast the un-upgraded stun forever while the host's scaled,
                // because the upgrade lives on the shared SkillDef and only the local path set it.
                f32 tcOrig = 0.0f;
                SkillDef* tcDef = beginThunderclapUpgrade(cls.skills[cskSlot],
                                                          cls.skillUpgradeFloor[cskSlot],
                                                          effectiveFloor, tcOrig);
                // TA-3: cast against the GUEST's own view (see boot-skill note above)
                // so class dash/blink/Blood Nova mutate the guest, never the host.
                Player view; buildRemotePlayerView(static_cast<u8>(i), view);
                // R17: same tick-gate as boot/helm — input.clientTick is the remote's frame.
                SkillSystem::tryActivate(tempSS, m_skillDefs, m_skillDefCount,
                                          eyePos, fwd, m_players[i].yaw,
                                          m_projectiles, m_entities, m_level.grid, view,
                                          in.clientTick,
                                          m_inventories[i].bonusCooldownReduction);
                endThunderclapUpgrade(tcDef, tcOrig);
                applyRemotePlayerView(view, static_cast<u8>(i));
            }
        }
    }
}

// ---------------------------------------------------------------------------
// R17 — populateSnapshotCooldowns: belt-and-suspenders ship of authoritative
// lastActivationTick values for every slot's class/boot/helm/potion. Pulls
// from Engine's skill-state arrays that Server::buildSnapshotOnly can't see.
// Called from serverNetPost between buildSnapshotOnly and the per-slot send.
// Client adopts MAX(local, snapshot) in Client::reconcile.
// ---------------------------------------------------------------------------
void Engine::populateSnapshotCooldowns() {
    WorldSnapshot* snap = Server::getLastSnapshotMutable();
    if (!snap) return;
    for (u32 pi = 0; pi < snap->playerCount; pi++) {
        SnapPlayer& sp = snap->players[pi];
        const u8 slot = sp.slotIndex;
        // potion's authoritative tick already lives on NetPlayer; buildFromState
        // copied it. Skill ticks live in Engine arrays — fill those here.
        if (slot < m_splitPlayerCount) {
            // Host-local lane — read its per-lane class-skill store directly (the m_classSkillStates
            // alias only mirrors the last-swapped lane, so use the backing array to be lane-correct).
            for (u32 s = 0; s < 4; s++)
                sp.classSkillLastActivationTick[s] = m_classSkillStatesPerPlayer[slot][s].lastActivationTick;
        } else {
            for (u32 s = 0; s < 4; s++)
                sp.classSkillLastActivationTick[s] = m_classSkillStatesNet[slot][s].lastActivationTick;
        }
        sp.bootSkillLastActivationTick   = m_bootSkillStates[slot].lastActivationTick;
        sp.helmetSkillLastActivationTick = m_helmetSkillStates[slot].lastActivationTick;
        // potionLastActivationTick already copied in buildFromState from NetPlayer.
    }
}

// ---------------------------------------------------------------------------
// R17 — adoptSnapshotCooldowns: CLIENT side of the wire cooldown sync. Reads
// the latest snapshot's SnapPlayer for the local slot, and adopts MAX(local,
// snapshot) into m_classSkillStates / m_classSkillStatesPerPlayer (per-player
// backing for swap-victim alias) / m_bootSkillStates / m_helmetSkillStates /
// m_potionLastActivationTick / m_potionLastActivationTicks.
// ---------------------------------------------------------------------------
void Engine::adoptSnapshotCooldowns() {
    if (m_netRole != NetRole::CLIENT) return;
    const WorldSnapshot* snap = Client::getLatestSnapshot();
    if (!snap) return;
    const SnapPlayer* sp = nullptr;
    const u8 mySlot = activeNetSlot();
    for (u32 i = 0; i < snap->playerCount; i++) {
        if (snap->players[i].slotIndex == mySlot) { sp = &snap->players[i]; break; }
    }
    if (!sp) return;
    auto adoptMax = [](u32& dst, u32 src) { if (src > dst) dst = src; };
    // Class skill ticks — both the swap-alias and the per-player backing for
    // the local lane (m_localPlayerIndex). swapInPlayer would otherwise copy
    // the stale backing over the freshly-adopted alias on the next frame.
    for (u32 s = 0; s < 4; s++) {
        adoptMax(m_classSkillStates[s].lastActivationTick,
                 sp->classSkillLastActivationTick[s]);
        adoptMax(m_classSkillStatesPerPlayer[m_localPlayerIndex][s].lastActivationTick,
                 sp->classSkillLastActivationTick[s]);
    }
    // Boot / helm aren't swap-victims (indexed by net slot); single write.
    adoptMax(m_bootSkillStates[m_localPlayerIndex].lastActivationTick,
             sp->bootSkillLastActivationTick);
    adoptMax(m_helmetSkillStates[m_localPlayerIndex].lastActivationTick,
             sp->helmetSkillLastActivationTick);
    // Potion — alias + per-player backing (LOCAL_PLAYER_SWAP_FIELDS via R17 update).
    adoptMax(m_potionLastActivationTick, sp->potionLastActivationTick);
    adoptMax(m_potionLastActivationTicks[m_localPlayerIndex], sp->potionLastActivationTick);

    // Shrine buff — ADOPT, don't predict. The server owns the grant (a guest's E-press goes through
    // CL_PICKUP_ITEM and lands on the authoritative NetPlayer), so this is how the client's local
    // Player learns it has a buff at all. Without it the client would keep predicting BASE speed
    // while the server moved it faster, and the reconcile would drag it forward every tick.
    // Straight assignment, not adoptMax: the server is the sole author, so its value is the truth —
    // including when it expires (a max-merge would make the buff immortal on the client).
    {
        const u8  type = static_cast<u8>((sp->statusFlags >> 5) & 0x07u);   // bits 5-7 since the 4th shrine
        const f32 remaining = static_cast<f32>(sp->shrineTimerQ) * 0.2f;
        // VITALITY's max-HP bump is applied server-side and already rides in SnapPlayer.maxHealth,
        // which the client reconstructs absolute HP from — so the client must NOT re-apply it here
        // or it would double the bonus locally.
        m_localPlayer.shrineBuff      = (remaining > 0.0f) ? type : ShrineBuff::NONE;
        m_localPlayer.shrineBuffValue = Shrine::bonusFor(m_localPlayer.shrineBuff);
        m_localPlayer.shrineBuffTimer = remaining;
    }

    // Static Charge stacks — ADOPT, don't predict (same reasoning as the shrine buff above):
    // they accumulate from server-authoritative damage the client never simulates on itself.
    // chargeTimer here is only the HUD row's visibility driver; the server refreshes the stack
    // count every snapshot, so a constant beats mirroring the real 10s window.
    m_localPlayer.chargeStacks = static_cast<u8>((sp->flags >> 5) & 0x07u);
    m_localPlayer.chargeTimer  = (m_localPlayer.chargeStacks > 0) ? 1.0f : 0.0f;
}

// ---------------------------------------------------------------------------
// Server networking — post-gameplay: status ticks, snapshot broadcast
// ---------------------------------------------------------------------------
void Engine::serverNetPost(f32 dt) {
    // Write the host's combat/status state back into its authoritative NetPlayer. Enemy AI,
    // projectiles, and the bomber blast run in tickSharedSystems (AFTER the per-player loop's
    // swapOut), and damage the PERSISTENT m_localPlayers[host] array — NOT the m_localPlayer
    // swap alias (whose writes were already swapped out before tickSharedSystems). Reading the
    // stale alias here dropped all host damage every frame; read the persistent array instead.
    // Transform (position/velocity) is owned by gameUpdate and already in m_players via
    // syncLocalPlayerToNetPlayer — enemies never move the player, so it's intentionally omitted.
    {
        NetPlayer& host = m_players[m_localPlayerIndex];
        const Player& hp = m_localPlayers[m_localPlayerIndex];
        host.health          = hp.health;
        host.damageFlashTimer = hp.damageFlashTimer;
        host.invulnTimer     = hp.invulnTimer;
        host.lifesaverArmed  = hp.lifesaverArmed;
        host.poisonTimer     = hp.poisonTimer;
        host.poisonDps       = hp.poisonDps;
        host.burnTimer       = hp.burnTimer;
        host.burnDps         = hp.burnDps;
        host.slowTimer       = hp.slowTimer;
        host.freezeTimer     = hp.freezeTimer;
        // Shrine buff: the host grants onto its local Player (it is its own authority), so mirror it
        // into the host's NetPlayer or the host's own snapshot slot would advertise no buff — and
        // VITALITY's raised maxHealth would not reach anyone reading that slot.
        host.shrineBuff      = hp.shrineBuff;
        host.shrineBuffValue = hp.shrineBuffValue;
        host.shrineBuffTimer = hp.shrineBuffTimer;
        host.maxHealth       = hp.maxHealth;
    }

    // Server-side globe auto-pickup for remote players
    for (u32 wi = 0; wi < MAX_WORLD_ITEMS; wi++) {
        WorldItem& item = m_worldItems.items[wi];
        if (!item.active || !isGlobe(item.item)) continue;
        for (u32 pi = 0; pi < MAX_PLAYERS; pi++) {
            if (pi < m_splitPlayerCount) continue; // host-local lanes pick up in gameUpdate
            if (!m_players[pi].active || m_players[pi].isDead) continue;
            Vec3 delta = m_players[pi].position - item.position;
            f32 dist = sqrtf(delta.x * delta.x + delta.z * delta.z);
            if (dist < 3.0f) {
                f32 healAmt = m_players[pi].maxHealth * GameConst::GLOBE_HEAL_PCT;
                m_players[pi].health += healAmt;
                if (m_players[pi].health > m_players[pi].maxHealth)
                    m_players[pi].health = m_players[pi].maxHealth;
                // The ENERGY half of the globe — the host path grants both (engine_update.cpp
                // globe auto-pickup), but this remote path granted HP only, so a guest's globe
                // was worth half a globe. grantEnergy routes it as SV_ENERGY_GAIN to the owning
                // client (the server's m_skillStates[pi].maxEnergy may lag the guest's own
                // descent-scaled max slightly; the client clamps to its real max on receipt).
                grantEnergy(static_cast<u8>(pi),
                            m_skillStates[pi].maxEnergy * GameConst::GLOBE_ENERGY_PCT);
                item.active = false;
                if (m_worldItems.activeCount > 0) m_worldItems.activeCount--;
                break;
            }
        }
    }

    // Server-side source-shard auto-pickup for remote players (mirrors globes above). The host's
    // s_sourceShards is the AUTHORITATIVE key for the Source-portal spawn, so a shard a remote
    // grabs must still register on the host. collectSourceShard sets the bit + whispers (host-side).
    for (u32 wi = 0; wi < MAX_WORLD_ITEMS; wi++) {
        WorldItem& item = m_worldItems.items[wi];
        if (!item.active || !isSourceShard(item.item)) continue;
        for (u32 pi = 0; pi < MAX_PLAYERS; pi++) {
            if (pi < m_splitPlayerCount) continue; // host-local lanes pick up in gameUpdate
            if (!m_players[pi].active || m_players[pi].isDead) continue;
            Vec3 delta = m_players[pi].position - item.position;
            f32 dist = sqrtf(delta.x * delta.x + delta.z * delta.z);
            if (dist < 3.0f) {
                collectSourceShard(item.item);
                item.active = false;
                if (m_worldItems.activeCount > 0) m_worldItems.activeCount--;
                break;
            }
        }
    }

    // Damage flash decay for remote players
    for (u32 i = 0; i < MAX_PLAYERS; i++) {
        if (i < m_splitPlayerCount) continue; // host-local lanes decay in gameUpdate
        if (m_players[i].active && m_players[i].damageFlashTimer > 0.0f)
            m_players[i].damageFlashTimer -= dt;
    }

    // Tick status effects (REMOTES only) + death detection (all). The HOST's status timers and
    // DoT are already ticked by its local tickPlayerStatusEffects pass and copied into its
    // NetPlayer by the writeback at the top of this function — ticking them again here would
    // double the host's poison/burn and halve its debuff/invuln durations. Death detection
    // still runs for every slot (the host's enemy + local-DoT damage is now in np.health).
    for (u32 pi = 0; pi < MAX_PLAYERS; pi++) {
        NetPlayer& np = m_players[pi];
        if (!np.active || np.isDead) continue;

        if (pi >= m_splitPlayerCount) { // host-local lanes tick status in their gameUpdate pass
            if (np.invulnTimer > 0.0f) { np.invulnTimer -= dt; if (np.invulnTimer < 0.0f) np.invulnTimer = 0.0f; }
            // Reset the near-death lifesaver grace once this remote player is healthy again (>85% HP)
            // — mirrors the local-player rule in engine_update.cpp. graceInvuln tags only the
            // lifesaver invuln, so a client's dodge i-frames (also 0.3 s) survive. The cleared
            // invulnTimer rides the next snapshot back to the client. Drop the tag on expiry too.
            if (np.graceInvuln) {
                if (np.invulnTimer <= 0.0f)                 { np.graceInvuln = false; }
                else if (np.health > np.maxHealth * 0.85f)  { np.invulnTimer = 0.0f; np.graceInvuln = false; }
            }
            if (np.slowTimer > 0.0f)   np.slowTimer -= dt;
            if (np.freezeTimer > 0.0f) np.freezeTimer -= dt;
            // R15: potionCooldown drain moved to serverNetPre (alongside the R9 skill
            // cooldown drain) so the input gate at line ~200 sees the post-tick value.

            if (np.invulnTimer <= 0.0f) {
                if (np.poisonTimer > 0.0f) { np.poisonTimer -= dt; np.health -= np.poisonDps * dt; }
                if (np.burnTimer > 0.0f)   { np.burnTimer -= dt;   np.health -= np.burnDps * dt;   }
            } else {
                np.poisonTimer = 0.0f; np.burnTimer = 0.0f;
                np.freezeTimer = 0.0f; np.slowTimer = 0.0f;
            }

            // Passive health regen (HEALTH_REGEN affixes — defensive pack) for REMOTE players,
            // authoritatively. Read from the server's copy of this slot's inventory; additive +
            // clamped to max. Host-local lanes regen in their own tickPlayerStatusEffects pass.
            // Gate on health > 0 so regen NEVER revives a downed player: this pass applies
            // regen BEFORE the np.health<=0 death check below, so without the guard a hit
            // that drove a remote to 0 HP gets nudged back positive by regen and the death
            // never latches — a client with HEALTH_REGEN then "won't die reliably". Mirrors
            // the local path's guard (engine_update_player.cpp: "never revives a corpse").
            f32 regen = Inventory::healthRegenRate(m_inventories[pi]);
            if (regen > 0.0f && np.health > 0.0f && np.health < np.maxHealth) {
                np.health += regen * dt;
                if (np.health > np.maxHealth) np.health = np.maxHealth;
            }
        }

        if (np.health <= 0.0f) {
            np.health = 0.0f;
            np.isDead = true;
            LOG_INFO("Player %u died", pi);
            if (pi < m_splitPlayerCount) {
                m_playerDead[pi] = true; // host-local lane death — flag its lane so the server doesn't freeze
            }
            // Arena PvP: a REMOTE combatant fell — credit + auto-respawn clock. (Host-local
            // lanes route through the gameUpdate death path, which calls arenaHandleDeath
            // itself; this edge only fires for pi >= m_splitPlayerCount because local lanes'
            // HP reaches the NetPlayer already clamped by that path.)
            if (m_level.inArena && pi >= m_splitPlayerCount) {
                arenaHandleDeath(static_cast<u8>(pi), np.lastHitByPlayerSlot);
                np.lastHitByPlayerSlot = 0xFF;
            }
        }
    }

    // Client respawn is handled out-of-band via the reliable CL_RESPAWN packet
    // (Engine::handleRespawnRequest), not through the input buffer — the old
    // INPUT_EX_RESPAWN-via-input approach was silently dropped by the monotonic-tick
    // input ring buffer. The host respawns itself directly in the IN_GAME dead-branch.

    // Per-player equipment passives + armor aura
    for (u32 pi = 0; pi < MAX_PLAYERS; pi++) {
        NetPlayer& np = m_players[pi];
        if (!np.active || np.isDead) continue;

        const ItemInstance& wpnItem = m_inventories[pi].equipped[static_cast<u32>(ItemSlot::WEAPON)];
        np.weaponProc = (!isItemEmpty(wpnItem) && wpnItem.rarity == Rarity::LEGENDARY)
            ? m_itemDefs[wpnItem.defId].legendarySkillId : SkillId::NONE;
        const ItemInstance& armorItem = m_inventories[pi].equipped[static_cast<u32>(ItemSlot::ARMOR)];
        np.armorAura = (!isItemEmpty(armorItem) && armorItem.rarity == Rarity::LEGENDARY)
            ? m_itemDefs[armorItem.defId].legendarySkillId : SkillId::NONE;
        const ItemInstance& ringItem = m_inventories[pi].equipped[static_cast<u32>(ItemSlot::RING)];
        np.ringPassive = (!isItemEmpty(ringItem) && ringItem.rarity == Rarity::LEGENDARY)
            ? m_itemDefs[ringItem.defId].legendarySkillId : SkillId::NONE;
        const ItemInstance& offItem = m_inventories[pi].equipped[static_cast<u32>(ItemSlot::OFFHAND)];
        np.offhandSkill = static_cast<u8>((!isItemEmpty(offItem) && offItem.rarity == Rarity::LEGENDARY)
            ? m_itemDefs[offItem.defId].legendarySkillId : SkillId::NONE);

        np.damageReduction = (np.playerClass == PlayerClass::WARRIOR) ? 0.3f : 0.0f;

        if (np.armorAura != SkillId::NONE) {
            for (u32 a = 0; a < m_entities.activeCount; a++) {
                u32 idx = m_entities.activeList[a];
                Entity& ent = m_entities.entities[idx];
                if (ent.flags & ENT_DEAD) continue;
                if (ent.flags & ENT_FRIENDLY) continue;
                if (ent.enemyType == EnemyType::PROP) continue;
                f32 dist = length(ent.position - np.position);
                switch (np.armorAura) {
                    case SkillId::METEOR_STRIKE: if (dist < 3.0f) { ent.burnTimer = 0.5f; ent.burnDps = 2.0f; ent.burnSrcSlot = np.slotIndex; } break;
                    case SkillId::FROZEN_ORB: if (dist < 4.0f) { ent.freezeTimer = 0.5f; } break;
                    // BLOOD_NOVA is handled below, not here — it retaliates on being STRUCK
                    // rather than as a proximity aura (mirrors tickArmorRingPassives).
                    case SkillId::CHAIN_LIGHTNING: if (dist < 3.0f) { ent.freezeTimer = 0.3f; } break;
                    case SkillId::PHASE_DASH: if (dist < 3.0f) { ent.freezeTimer = 0.4f; } break;
                    default: break;
                }
            }
        }

        // Blood Nova ARMOR aura for a REMOTE (host-local lanes fire theirs in gameUpdate via
        // tickArmorRingPassives — skipping them here avoids a double detonation). This runs in
        // serverNetPost, which is AFTER tickSharedSystems, so np.lastDamageTaken written back from
        // this tick's Player view (writeBackRemoteView) is fresh: the guest erupts on the same tick
        // it was hit. Consume it, then clear — otherwise one hit would re-trigger every tick.
        if (pi >= m_splitPlayerCount) {
            if (np.bloodNovaCooldown > 0.0f) np.bloodNovaCooldown -= dt;
            if (np.armorAura == SkillId::BLOOD_NOVA && np.lastDamageTaken > 0.0f) {
                detonateBloodNova(np.position, np.slotIndex, np.health, np.bloodNovaCooldown);
            }
            // Static Charge / Hemophage for a REMOTE (host lanes run theirs in
            // tickArmorRingPassives — both are damaging/stateful, NOT idempotent like the
            // burn/freeze aura timers above, so running them for host lanes here would double).
            if (np.armorAura == SkillId::STATIC_CHARGE) {
                if (StaticCharge::accumulate(np.chargeStacks, np.chargeTimer,
                                             np.lastDamageTaken > 0.0f, dt))
                    staticDischarge(np.position, np.slotIndex, np.lastDamageAttackerIdx);
            }
            if (np.armorAura == SkillId::HEMOPHAGE)
                hemophageAuraTick(np.position, np.slotIndex, np.hemoTickTimer,
                                  np.health, np.maxHealth, dt);
            np.lastDamageTaken = 0.0f;
        }

        // (M9) Defensive ring passives for REMOTE players. Host-local lanes tick their ring
        // passives in gameUpdate via tickArmorRingPassives — skip them here (all of slots
        // 0..count-1) to avoid double-ticking Second Wind / Divine Judgment / Soul Harvest decay.
        if (pi >= m_splitPlayerCount && np.ringPassive != SkillId::NONE) {
            // Soul Harvest stack window decay (also keeps M8 stacks consistent for remotes).
            if (np.soulHarvestTimer > 0.0f) {
                np.soulHarvestTimer -= dt;
                if (np.soulHarvestTimer <= 0.0f) np.soulHarvestStacks = 0;
            }
            if (np.secondWindCooldown > 0.0f) np.secondWindCooldown -= dt;
            // Second Wind: <20% HP, heal 30% + 2 s invuln, 60 s cooldown.
            if (np.ringPassive == SkillId::SECOND_WIND &&
                np.health > 0.0f &&
                np.health < np.maxHealth * 0.2f &&
                np.secondWindCooldown <= 0.0f) {
                np.health = fminf(np.health + np.maxHealth * 0.3f, np.maxHealth);
                np.invulnTimer = fmaxf(np.invulnTimer, 2.0f);
                np.secondWindCooldown = 60.0f;
                LOG_INFO("Player %u Second Wind triggered (remote)", pi);
            }
            // Divine Judgment: <25% HP, full heal + cleanse + AoE stun, 45 s cooldown (shares slot).
            if (np.ringPassive == SkillId::DIVINE_JUDGMENT &&
                np.health > 0.0f &&
                np.health < np.maxHealth * 0.25f &&
                np.secondWindCooldown <= 0.0f) {
                np.health = np.maxHealth;
                np.slowTimer = np.poisonTimer = np.burnTimer = np.freezeTimer = 0.0f;
                np.poisonDps = np.burnDps = 0.0f;
                np.invulnTimer = fmaxf(np.invulnTimer, 1.5f);
                np.secondWindCooldown = 45.0f;
                // AoE stun nearby enemies (5 m).
                for (u32 a = 0; a < m_entities.activeCount; a++) {
                    Entity& ent = m_entities.entities[m_entities.activeList[a]];
                    if ((ent.flags & ENT_FRIENDLY) || (ent.flags & ENT_DEAD)) continue;
                    Vec3 d = ent.position - np.position;
                    if (d.x*d.x + d.y*d.y + d.z*d.z < 25.0f) ent.stunTimer = 1.5f;
                }
                LOG_INFO("Player %u Divine Judgment triggered (remote)", pi);
            }
        }

        // Wanderer transient timers for REMOTES. Without this, marks/stacks/Shadow Dance
        // credited onto the NetPlayer in handleDeathPreamble accumulate forever (the host's
        // PlayerController tick only touches m_localPlayer). Mirrors the per-frame decay in
        // engine_update_player.cpp:191-211, :508-517 — applied per server tick (60 Hz).
        // Class gate kept loose (decay is safe regardless): a remote that was Wanderer
        // earlier and changed class would still benefit from draining leftover timers.
        if (pi >= m_splitPlayerCount) { // host-local lanes drain these in their gameUpdate pass
            // Mark duration
            if (np.markTimer > 0.0f) { np.markTimer -= dt; if (np.markTimer < 0.0f) np.markTimer = 0.0f; }
            // Per-stack 3 s non-refreshing speed timers — compact down as they expire.
            u8& stacks = np.markSpeedStacks;
            for (u8 i = 0; i < stacks; ) {
                np.markSpeedTimers[i] -= dt;
                if (np.markSpeedTimers[i] <= 0.0f) {
                    for (u8 j = i; j + 1 < stacks; j++) np.markSpeedTimers[j] = np.markSpeedTimers[j + 1];
                    stacks--;
                } else { i++; }
            }
            // Shadow Dance: keep smokeTimer pinned to shadowDanceTimer for the buff's life.
            if (np.shadowDanceTimer > 0.0f) {
                np.shadowDanceTimer -= dt;
                if (np.shadowDanceTimer > 0.0f) {
                    if (np.smokeTimer < np.shadowDanceTimer) np.smokeTimer = np.shadowDanceTimer;
                } else {
                    np.shadowDanceTimer = 0.0f;
                }
            }
            // Overdrive (Mech Overdrive / War Cry speed buff) — remote lanes only, same as the
            // timers above: the host expires its own in engine_update_player.cpp, and ticking
            // both places would burn a guest's buff down at double speed.
            if (np.overdriveTimer > 0.0f) {
                np.overdriveTimer -= dt;
                if (np.overdriveTimer < 0.0f) np.overdriveTimer = 0.0f;
            }
            // Crowd control (PvP), remote lanes only — same split as overdrive: the host expires its
            // own stun/immunity/DR in engine_update_player.cpp, so ticking both would burn them at
            // double speed. stunTimer decays here, the SnapPlayer carries it out, the guest adopts it.
            if (np.stunTimer > 0.0f)     { np.stunTimer -= dt;     if (np.stunTimer < 0.0f) np.stunTimer = 0.0f; }
            if (np.ccImmuneTimer > 0.0f) { np.ccImmuneTimer -= dt; if (np.ccImmuneTimer < 0.0f) np.ccImmuneTimer = 0.0f; }
            CrowdControl::tickStunDr(np.stunDr, dt);
            // Wanderer kit, remote lanes (mirrors tickWandererTimers, which only serves the
            // host's local player). Deflect: absorb accumulated on this remote's view during
            // the AI/projectile pass; when the window runs out, fire the SAME shared burst
            // the local path uses — the guest predicted its own burst locally, this is the
            // authoritative one (real damage, real kill credit).
            if (np.deflectTimer > 0.0f) {
                np.deflectTimer -= dt;
                if (np.deflectTimer <= 0.0f) {
                    np.deflectTimer = 0.0f;
                    // XZ facing from yaw — the burst's nova/targeting is full-circle anyway.
                    Vec3 fwd = {-sinf(np.yaw), 0.0f, -cosf(np.yaw)};
                    fireDeflectBurst(np.position, fwd, np.eyeHeight,
                                     np.deflectAbsorbed, np.deflectHitCount,
                                     static_cast<u8>(pi), /*localVisuals=*/false);
                    if (np.deflectHitCount > 0 && np.deflectAbsorbed > 0.0f)
                        np.deflectSpeedTimer = 3.0f;   // +8% for 3 s, matches the local path
                    np.deflectAbsorbed = 0.0f;
                    np.deflectHitCount = 0;
                }
            }
            if (np.deflectSpeedTimer > 0.0f) {
                np.deflectSpeedTimer -= dt;
                if (np.deflectSpeedTimer < 0.0f) np.deflectSpeedTimer = 0.0f;
            }
            if (np.deathsDanceTimer > 0.0f) {
                np.deathsDanceTimer -= dt;
                if (np.deathsDanceTimer < 0.0f) np.deathsDanceTimer = 0.0f;
            }
            // Adrenaline stack decay — compact-down, mirrors tickWandererTimers exactly.
            {
                u8& astacks = np.counterStacks;
                for (u8 ai = 0; ai < astacks; ) {
                    np.counterTimers[ai] -= dt;
                    if (np.counterTimers[ai] <= 0.0f) {
                        for (u8 aj = ai; aj + 1 < astacks; aj++)
                            np.counterTimers[aj] = np.counterTimers[aj + 1];
                        astacks--;
                    } else { ai++; }
                }
            }
            // Derived gate for updateNetPlayerFromInput's adrenaline speed bonus — player.cpp
            // can't see the floor, so the server refreshes the flag here (mirrors the local
            // per-tick derivation in tickWandererTimers).
            np.adrenalineUpgraded = (np.playerClass == PlayerClass::WANDERER) &&
                                    (m_level.currentFloor >= 30);
            // Shrine buff expiry for REMOTE lanes (this loop already skips the host, which expires
            // its own buff locally in engine_update_player.cpp — ticking it in both places would
            // burn it down at double speed).
            // VITALITY must undo its own max-HP bump, and clamp current HP under the new cap.
            // Remote lanes: same derived max HP as the host's local player (gear health reaches a
            // guest too). The server owns this; the guest adopts it through SnapPlayer.
            Inventory::refreshMaxHealth(np, m_inventories[pi]);

            if (np.shrineBuffTimer > 0.0f) {
                np.shrineBuffTimer -= dt;
                if (np.shrineBuffTimer <= 0.0f) {
                    // Unconditional — see the matching local path in engine_update_player.cpp.
                    if (np.shrineHealthBonus > 0.0f) {
                        np.maxHealth -= np.shrineHealthBonus;
                        if (np.maxHealth < 1.0f) np.maxHealth = 1.0f;
                        if (np.health > np.maxHealth) np.health = np.maxHealth;
                        np.shrineHealthBonus = 0.0f;
                    }
                    np.shrineBuff      = ShrineBuff::NONE;
                    np.shrineBuffValue = 0.0f;
                    np.shrineBuffTimer = 0.0f;
                }
            }
            // Frenzy (glove passive) — mirror of the local decay in tickArmorRingPassives.
            if (np.frenzyTimer > 0.0f) {
                np.frenzyTimer -= dt;
                if (np.frenzyTimer <= 0.0f) np.frenzyStacks = 0;
            }
        }
    }

    // Broadcast snapshot every TICKS_PER_SNAP ticks (now includes world items — N5).
    // Refresh each active player's resolved third-person weapon-mesh ID first — the wire field
    // is a MeshDef index (NOT a weapon-slot index). Without this clients render random props
    // as remote weapons. (See SnapPlayer.weaponMeshId + WeaponState.weaponMeshId in weapon.h.)
    if (m_serverTick % TICKS_PER_SNAP == 0) {
        for (u32 i = 0; i < MAX_PLAYERS; i++) {
            NetPlayer& np = m_players[i];
            if (!np.active) { np.weaponState.weaponMeshId = 0; continue; }
            const ItemInstance& wpn = m_inventories[i].equipped[static_cast<u32>(ItemSlot::WEAPON)];
            np.weaponState.weaponMeshId = (!isItemEmpty(wpn) && wpn.defId < m_itemDefCount)
                                          ? m_itemDefs[wpn.defId].meshId : 0;
        }
        // [AUDIT-P1] Diagnostic: what is the server actually publishing? If ents=0/projs=0 here,
        // the bug is on the host (m_entities/m_projectiles not populated), not in transmit/recv.
        // Throttled to every 5th broadcast (~4 Hz) — enough to confirm steady-state, quiet enough
        // to read. Remove once the no-enemies/no-projectiles symptom is root-caused.
        static u32 s_snapTxLogCounter = 0;
        if ((s_snapTxLogCounter++ % 1800) == 0) {
            u32 activePlayers = 0;
            for (u32 i = 0; i < MAX_PLAYERS; i++) if (m_players[i].active) activePlayers++;
            LOG_INFO("[AUDIT-P1] snap tx tick=%u players=%u ents=%u projs=%u items=%u",
                     m_serverTick, activePlayers,
                     m_entities.activeCount, m_projectiles.activeCount,
                     m_worldItems.activeCount);
        }
        // D7.3 — Build the snapshot once (no broadcast yet) so we can send each remote
        // client the appropriate encoding: delta if they have a confirmed baseline, full
        // if this is their first snapshot or the baseline has been invalidated.
        // Singleplayer (NetRole::NONE) never enters serverNetPost, so this path is
        // server-only. The old Server::sendSnapshot (broadcast) is retained for any
        // caller that needs the backwards-compatible broadcast, but here we drive
        // per-slot sends directly.
        Server::buildSnapshotOnly(m_serverTick, m_players, m_entities, m_projectiles, m_worldItems);

        // R17 — post-build: fill in per-slot cooldown lastActivationTick fields that
        // Server can't see (they live in Engine's m_classSkillStates / *Net /
        // m_bootSkillStates / m_helmetSkillStates). Belt-and-suspenders for the
        // tick gate: client's reconcile takes MAX(local, snapshot) so a benign
        // server-rejected client prediction is never under-gated.
        populateSnapshotCooldowns();

        // Post-build: fill armor tier-mesh ids per player slot. buildFromState has no
        // inventory access, so we patch via getLastSnapshotMutable() (same pattern as
        // populateSnapshotCooldowns). armorMeshId[k] is the tierMeshId of the equipped
        // piece in slot k (0=HELMET, 1=ARMOR/chest, 2=BOOTS, 3=GLOVES); 0 means empty.
        {
            WorldSnapshot* snap = Server::getLastSnapshotMutable();
            if (snap) {
                const ItemSlot kArmorSlots[4] = {
                    ItemSlot::HELMET, ItemSlot::ARMOR, ItemSlot::BOOTS, ItemSlot::GLOVES
                };
                for (u32 pi = 0; pi < snap->playerCount; ++pi) {
                    SnapPlayer& sp = snap->players[pi];
                    const u8 slot = sp.slotIndex;
                    if (slot >= MAX_PLAYERS || !m_players[slot].active) {
                        for (int k = 0; k < 4; ++k) sp.armorMeshId[k] = 0;
                        continue;
                    }
                    const PlayerInventory& inv = m_inventories[slot];
                    for (int k = 0; k < 4; ++k) {
                        const ItemInstance& it = inv.equipped[static_cast<u32>(kArmorSlots[k])];
                        sp.armorMeshId[k] = (!isItemEmpty(it) && it.defId < m_itemDefCount)
                                            ? m_itemDefs[it.defId].tierMeshId : 0;
                    }
                }
            }
        }

        // D7.3 — Per-slot send: full or delta based on baseline tracker state.
        for (u32 slot = 0; slot < MAX_PLAYERS; slot++) {
            if (!m_players[slot].active) continue;
            if (slot == static_cast<u32>(m_localPlayerIndex)) continue; // host has no remote peer

            // Delta against whatever snapshot this client has CONFIRMED decoding (its input
            // acks name it), looked up in the global history ring. A miss — new joiner (ack 0),
            // client stalled past the ring depth (~533 ms), floor transition — falls back to a
            // full snapshot, which re-anchors the ack loop. This is what finally lets deltas
            // ENGAGE: the old exact-match gate (ack == last-sent-tick) was permanently false at
            // any real RTT, so production had only ever sent full snapshots.
            const WorldSnapshot* base = nullptr;
            const u32 ack = m_clientAckedSnap[slot];
            if (ack != 0) {
                for (u32 h = 0; h < m_snapHistoryCount; h++) {
                    if (m_snapHistory[h].serverTick == ack) { base = &m_snapHistory[h]; break; }
                }
            }
            const bool sendFull = (base == nullptr);
            if (sendFull) Server::sendSnapshotFullToSlot(static_cast<u8>(slot));
            else          Server::sendSnapshotDeltaToSlot(static_cast<u8>(slot), *base);
            // Net-metrics: tally full vs delta so the F9 overlay can show the delta/full ratio
            // (a spike in fulls means baseline churn — what we want to see under packet loss).
            Net::noteSnapshotKind(static_cast<u8>(slot), sendFull);
        }

        // D7.3v2 — Push the just-built snapshot into the global baseline history. ONE copy per
        // tick, shared by every client (payloads are recipient-independent) — cheaper than the
        // per-slot copies it replaces.
        {
            const WorldSnapshot* sent = Server::getLastSnapshot();
            if (sent) {
                m_snapHistory[m_snapHistoryHead] = *sent;
                m_snapHistoryHead = (m_snapHistoryHead + 1) % SNAP_HISTORY_DEPTH;
                if (m_snapHistoryCount < SNAP_HISTORY_DEPTH) m_snapHistoryCount++;
            }
        }
        // Phase 3.1 — Capture entity poses at this snapshot tick into the lag-comp
        // history. We push only on snapshot ticks (every TICKS_PER_SNAP server ticks)
        // because that's the cadence the client renders from — every history entry
        // corresponds exactly to a snapshot the client received and could be aiming at.
        pushEntityHistory();
    }

    // Flush coalesced energy grants to remote guests (SV_ENERGY_GAIN): one reliable 4-byte packet
    // per guest per tick carrying the manasteal / mana-on-kill the host computed on its behalf.
    // grantEnergy() applied local lanes directly, so only remote slots accumulate here.
    for (u32 slot = 0; slot < MAX_PLAYERS; slot++) {
        if (m_pendingEnergyGain[slot] <= 0.0f) continue;
        if (!m_players[slot].active) { m_pendingEnergyGain[slot] = 0.0f; continue; }
        u8 buf[sizeof(PacketHeader) + 4];
        PacketHeader* hdr = reinterpret_cast<PacketHeader*>(buf);
        hdr->type  = NetPacketType::SV_ENERGY_GAIN;
        hdr->flags = 0;
        hdr->seq   = 0;
        std::memcpy(buf + sizeof(PacketHeader), &m_pendingEnergyGain[slot], 4);
        Net::sendReliable(static_cast<u8>(slot), buf, sizeof(buf));
        m_pendingEnergyGain[slot] = 0.0f;
    }
}

// ---------------------------------------------------------------------------
// Client networking — pre-gameplay: predict, reconcile
// ---------------------------------------------------------------------------
void Engine::clientNetPre(f32 dt) {
    // D5/M14: sync the fake-latency + fake-loss cvars into the net layer, then flush any outgoing
    // packets (CL_INPUT, CL_FIRE_WEAPON, etc.) whose delivery timestamp has elapsed.
    Net::setFakeLatencyMs(m_netFakeLatencyMs);
    Net::setFakeJitterMs(m_netFakeJitterMs);
    Net::setFakeLossPct(m_netFakeLossPct);
    Net::pumpDelayQueue();

    // Handle server disconnection gracefully (host left or crashed — both surface here as
    // a dropped connection; the host sends no explicit "leaving" packet). Save the client's
    // game first, then return to menu — same as the pause menu's "Save and Quit".
    if (!Net::isConnected()) {
        // Arena: the host leaving (mid-match, or tearing down at match end a beat before our
        // own banner clock) must NOT trigger the save below — the character is unchanged by
        // construction, and saving here would write floor 97 into the header. Clean exit.
        if (m_level.inArena) {
            LOG_INFO("Host left the arena — returning to menu (nothing to save)");
            arenaLeaveToMenu();
            return;
        }
        LOG_WARN("Host left / lost connection — saving and returning to menu");
        // Save BEFORE clearing the CLIENT role. saveCharacter's no-downgrade guard preserves the
        // higher on-disk effective floor regardless of role, so the host's lower floor never
        // overwrites our higher-progress save. Mirrors the pause-menu "Save and Quit" ordering. The
        // slot guard is belt-and-suspenders: an in-game client always has a slot (1-20), but slot 0
        // would clamp to 1 and clobber it. A client is a single local lane (0).
        if (m_activeSaveSlot >= 1) saveCharacter(0, m_activeSaveSlot);
        Net::disconnect();
        m_netRole = NetRole::NONE;
        m_gameState = GameState::MENU;
        AudioSystem::stopMusic();
        Input::setRelativeMouseMode(false);
        return;
    }

    m_serverTick++;
    m_clientTick++; // independent client-local tick; drives NetInput.clientTick (M1)

    // Clock-sync handshake — send 3 CL_TIME_PINGs ~10 ms apart immediately after
    // connection, then stop. Snapshot-driven refinement (ClockSyncOps::onSnapshotReceived,
    // wired in M1.6) takes over from there.
    constexpr u32 HANDSHAKE_PING_COUNT       = 3;
    constexpr f64 HANDSHAKE_PING_SPACING_SEC = 0.010;
    const f64 nowSec = Clock::getElapsedSeconds();
    if (m_pingsSent < HANDSHAKE_PING_COUNT &&
        (m_pingsSent == 0 || nowSec - m_lastPingSentSec >= HANDSHAKE_PING_SPACING_SEC)) {
        const u32 clientTimeMs = static_cast<u32>(nowSec * 1000.0);
        PacketWriter w;
        w.writeU8(static_cast<u8>(NetPacketType::CL_TIME_PING));
        w.writeU8(0);    // flags/padding
        w.writeU16(0);   // seq reserved
        w.writeU32(clientTimeMs);
        Net::sendToServer(w.data, w.cursor, /*reliable=*/false);
        m_lastPingSentSec = nowSec;
        m_pingsSent++;
    }

    // Capture and send input to server. Pass m_localPlayer so captureLocalInput can pack
    // absolute yaw/pitch/position (PROTOCOL_VERSION 2 NetInput). Pass the selected class-
    // skill slot so right-click activates the chosen skill (1-4) rather than always slot 0
    // — captureLocalInput defaults skillSlot to 0, and the server reads input->skillSlot
    // to pick the skill.
    // PER-LANE: capture+send input, reconcile, adopt cooldowns and mirror death for EACH local
    // player (online couch co-op carries two over one connection). swapInPlayer(sp) makes
    // m_localPlayer / activeNetSlot() / the lane-indexed alias arrays resolve to lane `sp`, exactly
    // like the dispatch's update loop. A single client iterates once (lane 0). NB body indent is one
    // level shallow to keep this diff minimal.
    for (u8 sp = 0; sp < m_splitPlayerCount; sp++) {
    swapInPlayer(sp);
    Input::setActivePlayer(sp);

    WeaponState& ws = m_players[activeNetSlot()].weaponState; // this lane's net slot

    // R17 — client-side wire-spam mask. Uses the same tick comparison the server will
    // evaluate against the input's clientTick. Identical formula on both sides → mask
    // matches the server's gate exactly, never strips a press the server would accept
    // (the R15-v2 `> dt` predicted-drain hack and the R16 RTT padding are gone — the
    // integer gate is unambiguous at the boundary). f32 cooldownTimer is HUD only.
    // Use the shared lenient gate (negated) so the mask only strips a press the
    // server would ALSO reject — never one it would accept under the grace window.
    auto onCooldown = [this](const SkillState& ss, f32 cdr) -> bool {
        if (ss.activeSkill == SkillId::NONE || ss.lastActivationTick == 0) return false;
        const SkillDef* def = SkillSystem::findSkillDef(m_skillDefs, m_skillDefCount, ss.activeSkill);
        if (!def) return false;
        const u32 cooldownTicks = SkillSystem::computeCooldownTicks(def->cooldown, cdr);
        return !GameConst::cooldownReady(m_clientTick, ss.lastActivationTick, cooldownTicks);
    };
    const f32 itemCdr = m_inventories[m_localPlayerIndex].bonusCooldownReduction;
    u8 skillClearMask = 0;
    if (onCooldown(m_classSkillStates[m_activeClassSkill],         itemCdr)) skillClearMask |= INPUT_EX_SKILL;
    if (onCooldown(m_bootSkillStates[m_localPlayerIndex],          itemCdr)) skillClearMask |= INPUT_EX_BOOT_SKILL;
    if (onCooldown(m_helmetSkillStates[m_localPlayerIndex],        itemCdr)) skillClearMask |= INPUT_EX_HELM_SKILL;
    // Energy parity: the local predict (tryActivate) refuses an unaffordable press (energy <
    // energyCost), but captureLocalInput sets the skill ext bits on the raw press regardless.
    // Without stripping them here the bit rides to the server, which casts ALL of class/boot/helm
    // with a throwaway energy=999 (processRemoteActivation) and never checks the real pool — so
    // the player "casts with insufficient energy". Class, boot AND helmet skills all draw from
    // the shared pool (handleClassSkillActivation / handleEquipmentSkillActivation), so gate all
    // three the same way. BLOOD_NOVA spends health (its own gate), never energy. Energy is client-
    // authoritative (no snapshot field), so this mask IS the enforcement — same model as the
    // potion/cooldown masks above; client-side also avoids the float-regen drift a server energy
    // gate would suffer (which would wrongly reject legit client-predicted casts).
    {
        const f32 energy = m_skillStates[m_localPlayerIndex].energy;
        auto unaffordable = [this, energy](SkillId active) -> bool {
            if (active == SkillId::NONE || active == SkillId::BLOOD_NOVA) return false;
            const SkillDef* def = SkillSystem::findSkillDef(m_skillDefs, m_skillDefCount, active);
            return def && energy < def->energyCost;
        };
        // Active CLASS skill: resolve from the class def — m_classSkillStates[].activeSkill is
        // only set on cast (can be stale here); boot/helm SkillStates are refreshed every frame.
        const ClassDef& cls = kClassDefs[static_cast<u32>(m_playerClass)];
        if (unaffordable(cls.skills[m_activeClassSkill]))                      skillClearMask |= INPUT_EX_SKILL;
        if (unaffordable(m_bootSkillStates[m_localPlayerIndex].activeSkill))   skillClearMask |= INPUT_EX_BOOT_SKILL;
        if (unaffordable(m_helmetSkillStates[m_localPlayerIndex].activeSkill)) skillClearMask |= INPUT_EX_HELM_SKILL;
    }
    // Send/predict parity while a blocking UI is open (inventory OR pause menu): the local skill AND
    // potion PREDICT paths gate on !gameplayInputFrozen() (engine_update_skills.cpp / the potion heal
    // in gameUpdate), but captureLocalInput sets those ext bits unconditionally — so a press while a
    // menu is open would fire server-side with no local prediction (a desync + an unintended
    // cast/heal). Strip them so send matches predict: you can no longer cast OR drink a potion with
    // the inventory (or pause) open. (Potion drink-while-paused was intentional once; the inventory
    // now navigates with WASD/E/F, so a stray potion press mid-menu is a bug, not a feature.)
    const bool frozen = gameplayInputFrozen();
    if (frozen)
        skillClearMask |= (INPUT_EX_SKILL | INPUT_EX_BOOT_SKILL | INPUT_EX_HELM_SKILL | INPUT_EX_POTION);
    // Potion was previously missing from the wire mask (the asymmetry that let the
    // potion bit ride to the server even while locally on cooldown → phantom heals +
    // forward cooldown bumps). Gate it the same lenient way as skills.
    {
        const f32 potionCdr = itemCdr * 0.1f; // potion gets 10% of item CDR (matches both gates)
        const u32 potionCdTicks = static_cast<u32>(GameConst::POTION_COOLDOWN * (1.0f - potionCdr) * 60.0f + 0.5f);
        if (!GameConst::cooldownReady(m_clientTick, m_potionLastActivationTick, potionCdTicks))
            skillClearMask |= INPUT_EX_POTION;
    }
    // freezeMovement: while frozen the local sim skips PlayerController::update, so zero the
    // wire movement too or the server walks the player from held keys (rubber-band on close).
    Client::captureAndSendInput(m_localPlayer, m_clientTick, ws.currentWeapon,
                                m_activeClassSkill, skillClearMask, /*freezeMovement=*/frozen,
                                /*laneId=*/sp, /*targetSlot=*/activeNetSlot());

    // M10.1: resendPendingFire() removed — CL_FIRE_WEAPON is now reliable.

    // (The prediction-ring push for m_clientTick used to live here. It now lives at the
    // top of clientNetPost — after gameUpdate has advanced m_localPlayer by this tick's
    // input — so the ring entry describes end-of-tick state, matching the server's
    // post-input snapshot at lastProcessedInputTick=T. Pushing pre-update state here
    // caused a constant ~1-tick reconcile mismatch and visible jitter.)

    // Reconcile compares the server's authoritative snapshot against the local-player
    // state we pushed into m_predictionRing on this clientTick. Position is server-
    // authoritative since M2 (posXQ/Y/Z removed from NetInput — see player.cpp:352);
    // the server re-derives position from input moveFlags + yaw via PlayerController
    // ::updateNetPlayerFromInput + Collision::moveAndSlide, integrated per-input in
    // serverNetPre so the server's cadence matches the client's per-tick gameUpdate.
    //
    // We do NOT re-run a prediction step here — m_localPlayer is itself the prediction,
    // updated each gameUpdate exactly as singleplayer would. Render-side smoothing
    // flows through m_renderOffset (RenderOffsetOps::accumulate on divergence, decayed
    // per frame at DECAY_RATE=13.0). The pre-M2 "trust-client position" comment that
    // used to live here described the now-removed posXYZ-in-NetInput model.
    const f32 hpPreAdopt = m_players[activeNetSlot()].health;
    Client::reconcile(m_players[activeNetSlot()], m_localPlayer, activeNetSlot());

    // Hurt feedback for hits the client could NOT predict (melee, champion novas, burning
    // ground, boss AoE): the snapshot is the only place a guest learns about those, and the bare
    // HP adoption in Client::reconcile plays nothing — the guest's HP bar just slid down in
    // silence while the host got sound/shake/rumble/vignette. An adoption that lands below the
    // baseline fires the same damageFlash path a local hit uses (its first tick plays PLAYER_HIT
    // + camera kick + rumble in tickVisualFeedback).
    //
    // The baseline is min(pre-adopt HP, last ADOPTED HP) because each alone has a false-positive:
    //   - pre-adopt alone: the potion heal is locally PREDICTED (engine_update.cpp), so local HP
    //     runs ahead of the server for ~RTT and the pre-heal snapshot reads as a drop — a phantom
    //     hit on every sip;
    //   - last-adopted alone: a PREDICTED incoming projectile (M7/D3.2 already fired feedback and
    //     decremented local HP) fires AGAIN when its authoritative drop arrives in the sequence.
    // min() of the two is immune to both, and still catches a real hit landing during a heal.
    {
        NetPlayer& npSelf   = m_players[activeNetSlot()];
        const f32  adopted  = npSelf.health;
        const f32  baseline = fminf(hpPreAdopt, m_lastAdoptedHp[sp]);
        const f32  drop     = baseline - adopted;
        m_lastAdoptedHp[sp] = adopted;
        // 2% floor: the u8 wire quantization (~0.4% of max) and DoT drip (~dps/60 per snapshot)
        // stay below it, so poison/burn don't strobe the flash — matching the host, where DoT
        // ticks don't re-flash either. No isDead gate: hearing the hit that kills you is correct.
        const f32 thresh = fmaxf(2.0f, npSelf.maxHealth * 0.02f);
        if (drop > thresh) {
            npSelf.damageFlashTimer        = 0.15f;  // np copy survives the lp<->np round-trip syncs
            m_localPlayer.damageFlashTimer = 0.15f;  // alias copy persists via this lane's swapOut
            // Same vignette curve as Combat::applyDamageToPlayer, scaled by the confirmed hit.
            f32 frac = drop / (npSelf.maxHealth > 0.0f ? npSelf.maxHealth : 100.0f);
            f32 v = 0.15f + frac * 0.6f;
            if (v > 0.85f) v = 0.85f;
            m_localPlayer.hurtVignette = fmaxf(m_localPlayer.hurtVignette, v);
        }
    }

    // R17 — adopt server's lastActivationTick values for skills + potion. MAX(local,
    // snapshot) keeps any benign client over-prediction (server rejected non-cooldown
    // gate; local advanced anyway) intact, while catching the rare edge case where
    // server has a newer value than client.
    adoptSnapshotCooldowns();

    // CLIENT death is server-authoritative: reconcile adopted the server's isDead into our net
    // slot; mirror it into the lane-indexed m_playerDead that the dead-branch + HUD overlay read.
    // Auto-clears when the server respawns us — no sticky local flag, no optimistic-revive flicker.
    m_playerDead[m_localPlayerIndex] = m_players[activeNetSlot()].isDead;

    swapOutPlayer(sp);
    } // end per-lane loop
    swapInPlayer(0); // restore lane-0 aliases before the dispatch's own split loop runs
}

// ---------------------------------------------------------------------------
// Client networking — post-gameplay: interpolate remote state
// ---------------------------------------------------------------------------
void Engine::clientNetPost(f32 dt) {
    // gameUpdate already synced m_localPlayer → NetPlayer at its end.

    // Snapshot the local player's POST-update state into the prediction ring, keyed by
    // m_clientTick (the tick we just sent an input for in clientNetPre). The ring entry's
    // position MUST describe the end-of-tick state — that's what the server reports in
    // its snapshot at lastProcessedInputTick=T, because the server runs
    // updateNetPlayerFromInput + moveAndSlide on input T inside serverNetPre. Pushing
    // pre-update state in clientNetPre caused the reconcile to see a constant ~1-tick
    // gap (~10 cm at running speed), triggering the M4 render-offset accumulator every
    // snapshot and producing the "super shaky" jitter the client experienced. The R3
    // per-input integration fix (b524abe) corrected the server's cadence; this push
    // location closes the loop on the client side.
    // Per-lane: push each local player's post-update state into ITS prediction ring (online couch
    // co-op predicts both lanes independently). swapInPlayer(sp) selects the lane.
    for (u8 sp = 0; sp < m_splitPlayerCount; sp++) {
        swapInPlayer(sp);
        PredictedState s;
        s.position    = m_localPlayer.position;
        s.velocity    = m_localPlayer.velocity;
        s.yaw         = m_localPlayer.yaw;
        s.pitch       = m_localPlayer.pitch;
        s.health      = m_localPlayer.health;
        s.invulnTimer = m_localPlayer.invulnTimer;
        s.onGround    = m_localPlayer.onGround;
        const NetInput* latest = Client::getLatestInput(sp);
        if (latest) PredictionRingOps::push(m_predictionRing[sp], m_clientTick, *latest, s);
    }
    swapInPlayer(0);

    // Interpolate remote players, entities, and projectiles from server snapshots. Skip BOTH local
    // lanes' slots (online couch co-op) — they render from their own predicted state, not interp.
    Client::interpolateRemotePlayers(m_clientNetSlot[0],
        (m_splitPlayerCount > 1) ? m_clientNetSlot[1] : static_cast<u8>(0xFF),
        m_renderInterp.playerPositions, m_renderInterp.playerYaws,
        m_renderInterp.playerActive, m_renderInterp.playerHealth, m_renderInterp.playerMaxHealth,
        m_renderInterp.playerAnimFlags, m_renderInterp.playerWeaponMeshId,
        m_renderInterp.playerClass, m_renderInterp.playerArmorMeshId,
        m_renderInterp.playerDodgeFlags, m_renderInterp.playerVelXZ,
        m_renderInterp.playerOnGround);
    Client::interpolateEntities(m_renderInterp.entities, dt);
    // Boss / non-boss halfExtents are now wire-authoritative per SnapEntity (Audit P2 #4).
    // The prior post-pass that looked up BossDef::halfExtents here was made redundant by
    // that wire change; removed to keep the floor lookup out of every snapshot tick.
    Client::interpolateProjectiles(m_renderInterp.projectiles);

    // V2 fire prediction — match-and-KEEP pass (formerly match-and-despawn).
    // Each snapshot projectile with clientTickLow != 0 and ownerSlot == this client's slot is the
    // authoritative copy of a locally-predicted ghost we spawned in handleWeaponFire. The OLD path
    // despawned the ghost and rendered the authoritative — but the authoritative is interpolated to
    // render-time = now - interp_delay, so it lags ~1 m+ BEHIND the smooth client-rate ghost, and the
    // swap looked like the projectile jumping backwards ("the ghost being replaced"). Instead we KEEP
    // the ghost as the canonical render and HIDE the authoritative while they agree (M10/M11 intent:
    // the predicted projectile IS canonical until reconciliation diverges). The ghost and the
    // authoritative fire from the SAME claimed origin/aim (handleWeaponFire sends them; the server
    // fires from those exact values) with identical speed/gravity, so their trajectories are identical
    // — the ghost is always a valid stand-in. We snap to the authoritative only on a pathological
    // divergence (e.g. server origin-clamp) larger than the worst-case interp-lag offset.
    //
    // NOTE: we iterate the pool DIRECTLY rather than the activeList here. Client::interpolate
    // Projectiles populates m_renderInterp.projectiles[poolIndex] and increments activeCount, but does
    // NOT update activeList[]. The renderer gates on .active (engine_render_effects.cpp:77), so
    // toggling .active is sufficient; activeCount stays a loose early-exit hint.
    constexpr f32 GHOST_HANDOFF_BASE_M     = 1.0f; // base divergence tolerance (metres)
    constexpr f32 GHOST_HANDOFF_INTERP_SEC = 0.2f; // × projectile speed = worst-case interp-lag offset (WiFi jitter)
    u8 mySlot = activeNetSlot();
    for (u32 idx = 0; idx < MAX_PROJECTILES; idx++) {
        Projectile& authoritative = m_renderInterp.projectiles.projectiles[idx];
        if (!authoritative.active) continue;
        if (authoritative.predicted) continue;   // defensive: pre-merge there are none; reorder-safe
        if (authoritative.ownerSlot != mySlot) continue;
        // clientTick carried back via wire as low 16 bits; recover here from the projectile's
        // stored clientTick (Client::interpolateProjectiles writes the low bits into it).
        u32 wireLow = authoritative.clientTick & 0xFFFF;
        if (wireLow == 0) continue;
        for (u32 j = 0; j < MAX_PROJECTILES; j++) {
            Projectile& ghost = m_projectiles.projectiles[j];
            if (!ghost.active || !ghost.predicted) continue;
            if ((ghost.clientTick & 0xFFFF) != wireLow) continue;
            // Speed-relative safety valve: keep the ghost unless the authoritative diverged further
            // than one worst-case interp-lag of travel (compare squared — avoid the sqrt).
            const f32 tol = GHOST_HANDOFF_BASE_M + length(ghost.velocity) * GHOST_HANDOFF_INTERP_SEC;
            if (lengthSq(authoritative.position - ghost.position) <= tol * tol) {
                authoritative.active = false; // hide the lagging authoritative; render the smooth ghost
                ghost.confirmed     = true;   // matched at least once
                ghost.predictedLife = 0.0f;   // confirmed this frame — defer the lost/expiry timeout
            } else {
                ghost.active = false;         // genuine correction — show the authoritative instead
            }
            break;
        }
    }

    // V2 fire prediction — merge surviving predicted ghosts into m_renderInterp.projectiles
    // so the existing render path picks them up alongside snapshot projectiles. We allocate
    // free render-pool slots starting from the TOP of the pool (so we don't collide with the
    // snapshot's nearest-first lower-index ordering). Predicted ghosts that have been matched
    // and despawned above won't appear here.
    for (u32 i = 0; i < MAX_PROJECTILES; i++) {
        const Projectile& ghost = m_projectiles.projectiles[i];
        if (!ghost.active || !ghost.predicted) continue;
        // Find a free slot in renderInterp scanning from the top down — snapshot fills from
        // the bottom up (poolIndex-driven), so this keeps the two ranges from colliding.
        for (u32 j = MAX_PROJECTILES; j-- > 0; ) {
            Projectile& dst = m_renderInterp.projectiles.projectiles[j];
            if (dst.active) continue;
            dst = ghost;
            m_renderInterp.projectiles.activeList[m_renderInterp.projectiles.activeCount++] =
                static_cast<u16>(j);
            break;
        }
    }

    // Chakram ricochet "pling" for EVERY OTHER player's chakrams — DETECTED, not replicated.
    //
    // Our own chakram already plings from its predicted ghost (engine_update.cpp), which is the
    // right source for it: the ghost is immediate, whereas anything derived from the snapshot
    // arrives one interp delay late — fine for someone else's chakram across the room, wrong for
    // the weapon in your own hands. But ghosts exist ONLY for our own projectiles, so the host's
    // and other guests' chakrams were silent.
    //
    // The snapshot already carries everything needed to spot a bounce, so this costs no wire
    // change: SnapProjectile has a stable `poolIndex`, the full `projFlags` byte (PROJ_BOUNCE
    // included), and the velocity — and interpolateProjectiles takes velocity STRAIGHT from the
    // newer snapshot rather than lerping it, so a reflection appears as one sharp direction flip
    // rather than a gradual turn. Track each bounce-projectile's unit velocity per render slot and
    // pling whenever it swings hard.
    //
    // Both double-pling hazards are handled by running AFTER the ghost merge above:
    //   • predicted ghosts (merged into this pool) would otherwise re-trigger the sound their own
    //     local sim already played — skipped explicitly via `p.predicted`;
    //   • our own authoritative copy is hidden (`active=false`) by the match-and-keep pass while
    //     its ghost is canonical, so it is skipped for free — and once the ghost is gone it
    //     becomes visible again and transparently takes over as the sound source.
    {
        // Last-seen unit velocity per RENDER slot ({0,0,0} = unseeded) + the owner that seeded it,
        // so a pool slot recycled by a different player's chakram reseeds instead of firing a
        // phantom bounce on the direction discontinuity between two unrelated projectiles.
        static Vec3 s_bounceDir[MAX_PROJECTILES]   = {};
        static u8   s_bounceOwner[MAX_PROJECTILES] = {};
        // cos(~25°). A wall reflection flips the velocity component along the face normal, so a
        // square-on hit reverses hard (dot ≈ -1) and trips this easily. A very shallow graze
        // barely changes direction and is missed — inaudible in practice, and far preferable to
        // a threshold loose enough to fire on ordinary steering.
        constexpr f32 BOUNCE_DOT_THRESH = 0.9f;

        for (u32 i = 0; i < MAX_PROJECTILES; i++) {
            const Projectile& p = m_renderInterp.projectiles.projectiles[i];
            if (!p.active || p.predicted || !(p.projFlags & PROJ_BOUNCE)) {
                s_bounceDir[i] = {0.0f, 0.0f, 0.0f};   // slot free/irrelevant — drop its tracking
                continue;
            }
            const f32 speed = length(p.velocity);
            if (speed < 0.0001f) continue;
            const Vec3 dir  = p.velocity * (1.0f / speed);
            const Vec3 prev = s_bounceDir[i];
            const bool seeded = (prev.x != 0.0f || prev.y != 0.0f || prev.z != 0.0f) &&
                                s_bounceOwner[i] == p.ownerSlot;
            if (seeded && dot(prev, dir) < BOUNCE_DOT_THRESH)
                AudioSystem::playAt(SfxId::RICOCHET, p.position, m_localPlayer.position);
            s_bounceDir[i]   = dir;
            s_bounceOwner[i] = p.ownerSlot;
        }
    }

    // [AUDIT-P1] Diagnostic: what does the RENDER pool actually contain after interp? If
    // [AUDIT-P1] snap rx showed non-zero ents/projs but this shows 0, the rebuild-activeList
    // path (the C1 fix) is broken or m_renderInterp is being clobbered post-interp. Throttled
    // to every 30th frame (~2 Hz at 60 fps) — interp runs once per CLIENT update tick.
    {
        static u32 s_interpLogCounter = 0;
        if ((s_interpLogCounter++ % 1800) == 0) {
            LOG_INFO("[AUDIT-P1] interp render: ents=%u projs=%u",
                     m_renderInterp.entities.activeCount,
                     m_renderInterp.projectiles.activeCount);
        }
    }

    // Mirror server-authoritative world items (loot drops — N5) into the local pool.
    // The renderer and pickup-aim code read m_worldItems directly, so this is all the
    // client needs to see/aim at loot. Runs AFTER gameUpdate so it overrides the local
    // WorldItemSystem::update (lifetime decay is server-driven for clients).
    Client::mirrorWorldItems(m_worldItems, m_itemDefs, m_itemDefCount, dt);

    // M8: Re-apply local pickup predictions — mirrorWorldItems (above) unconditionally
    // re-activates any item still present in the latest snapshot, which would undo the
    // local hide we set in sendPickupRequest this same frame. Walk the pending ring and
    // suppress those items again so they stay hidden until the server snapshot drops them
    // (confirming the pickup) or the prediction expires and mirrorWorldItems wins.
    for (u32 pi = 0; pi < m_pendingPickups.count; pi++) {
        u32 pendingUid = m_pendingPickups.entries[pi].itemUid;
        for (u32 i = 0; i < MAX_WORLD_ITEMS; i++) {
            WorldItem& wi = m_worldItems.items[i];
            if (wi.active && wi.item.uid == pendingUid) {
                wi.active = false;
                break;
            }
        }
    }

    // Bound the ring: drop entries older than ~2 seconds (120 ticks at 60 Hz) to handle
    // reliable-channel loss or server rejects that never explicitly re-showed the item.
    // After expiry, mirrorWorldItems will re-activate the item from the snapshot if the
    // server still has it (no ghost disappearance left behind).
    if (m_clientTick > 120) {
        PendingPickupRingOps::expireOlderThan(m_pendingPickups, m_clientTick - 120);
    }

    // M3.2 — Prediction reconciliation: compare server's authoritative position for the
    // local player at the ACK'd tick against our ring's predicted position. If we diverged
    // by more than 10 cm, log it and snap to the server's value. Full smooth-correction
    // lerp is deferred to M4; for M3 this is an immediate snap with a diagnostic log so
    // we can confirm the pipeline works before adding the visual smoothing layer.
    // Per-lane prediction reconciliation (online couch co-op): each local player compares the
    // server's authoritative position (at its ACK'd tick) against its own ring and snaps + smooths
    // independently. swapInPlayer(sp) selects the lane; swapOutPlayer saves the corrected position.
    for (u8 sp = 0; sp < m_splitPlayerCount; sp++) {
        swapInPlayer(sp);
        const WorldSnapshot* snap = Client::getLatestSnapshot();
        if (snap) {
            u8 mySlot = activeNetSlot();
            u32 ackedTick = snap->lastProcessedInputTick[mySlot];
            // Only reconcile a fresh ACK we haven't already processed this frame.
            if (ackedTick != 0 && ackedTick != m_lastReconciledTick[sp]) {
                const PredictionEntry* e = PredictionRingOps::find(m_predictionRing[sp], ackedTick);
                // players[] is PACKED (players[playerCount++], true slot in .slotIndex), so it must
                // be searched, never indexed by slot. Indexing worked in every 2-player session by
                // pure accident (no slot gaps) and read the WRONG player's position the moment a
                // mid-slot player left a 3-4 player game — every higher slot then "reconciled"
                // against someone else's body.
                const SnapPlayer* spp = Snapshot::findPlayerByPoolIndex(*snap, mySlot);
                if (e && spp && !m_players[mySlot].isDead) {
                    const SnapPlayer& sp_ = *spp;
                    Vec3 serverPos = {
                        Quantize::unpackPos(sp_.posX),
                        Quantize::unpackPos(sp_.posY),
                        Quantize::unpackPos(sp_.posZ)
                    };
                    Vec3 diff   = serverPos - e->state.position;
                    f32  distSq = diff.x * diff.x + diff.y * diff.y + diff.z * diff.z;
                    // Correct anything above quantization noise (u16 position precision is ~2 mm,
                    // so 2 cm is comfortably signal). The 10 cm tier inside only gates TELEMETRY,
                    // keeping the [NET-GRAPH] divergence stats comparable with their M14 baselines.
                    constexpr f32 REPLAY_EPS_SQ = 0.02f * 0.02f;
                    if (distSq > REPLAY_EPS_SQ) {
                        const f32 mag = sqrtf(distSq);
                        if (distSq > 0.01f) {  // > 10 cm — telemetry tier

                        // --- Shaky-client-FOV diagnostic ---------------------------------------
                        // Was this correction fired while the local player was brushing a MOVING
                        // enemy? That's the signature of the client/server obstacle-time mismatch
                        // (client samples enemies at the adaptive interp delay + an interpolated
                        // pose; the server rewinds a FIXED 2 ticks to a discrete pose — see
                        // buildLagCompPlayerObstacles). Scan the interpolated render pool (same
                        // pool the client's local moveAndSlide uses as obstacles) for the nearest
                        // active enemy in the XZ plane; within ~1.5 m (≈2× the ~0.8 m player+enemy
                        // contact distance) counts as "near". Cheap — only runs on a correction.
                        constexpr f32 BRUSH_RANGE_SQ = 1.5f * 1.5f;
                        f32 nearestEnemySq = 1e9f;
                        for (u32 a = 0; a < m_renderInterp.entities.activeCount; a++) {
                            const Entity& e = m_renderInterp.entities.entities[
                                m_renderInterp.entities.activeList[a]];
                            if (e.flags & ENT_DEAD) continue;
                            if (e.flags & ENT_FRIENDLY) continue;
                            if (e.enemyType == EnemyType::PROP) continue;
                            f32 dx = e.position.x - m_localPlayer.position.x;
                            f32 dz = e.position.z - m_localPlayer.position.z;
                            f32 d2 = dx * dx + dz * dz;
                            if (d2 < nearestEnemySq) nearestEnemySq = d2;
                        }
                        const bool nearEnemy = nearestEnemySq <= BRUSH_RANGE_SQ;

                        // Fold into the 1 Hz [NET-GRAPH] window (mean = sum/count, plus max +
                        // near-enemy tally). A high rate with most corrections NEAR an enemy and
                        // a widened interpDelay confirms the obstacle-time-mismatch root cause.
                        m_divergenceSumM += mag;
                        if (mag > m_divergenceMaxM) m_divergenceMaxM = mag;
                        if (nearEnemy) m_divergenceNearEnemyCount++;

                        // Per-correction detail, throttled to ~1-in-12 (~5 Hz on a maximally shaky
                        // floor) so up to 60 corrections/s don't flood the log — the summary above
                        // carries the rate/mean/max, this shows representative individual samples.
                        static u32 s_divLogThrottle = 0;
                        if ((s_divLogThrottle++ % 12) == 0) {
                            if (nearestEnemySq < 1e8f)
                                LOG_INFO("net: prediction divergence (lane %u) tick %u: %.2f m  "
                                         "interpDelay=%.0fms  %snearest-enemy=%.2fm",
                                         sp, ackedTick, mag, Client::getInterpDelaySec() * 1000.0f,
                                         nearEnemy ? "NEAR " : "", sqrtf(nearestEnemySq));
                            else
                                LOG_INFO("net: prediction divergence (lane %u) tick %u: %.2f m  "
                                         "interpDelay=%.0fms  (no enemy nearby)",
                                         sp, ackedTick, mag, Client::getInterpDelaySec() * 1000.0f);
                        }

                        // M14: count every reconcile mismatch for the 1 Hz net-graph log.
                        m_divergenceCount++;

                        // M13: large divergence (>=10 m) triggers a 0.5s screen flash so
                        // the player knows a significant teleport correction happened.
                        if (distSq > 100.0f) {
                            m_localPlayer.screenFlashTimer = 0.5f;
                        }
                        }  // end 10 cm telemetry tier

                        // ---- Rollback-replay reconciliation ------------------------------------
                        //
                        // The old correction snapped the CURRENT position to the server's state at
                        // ackedTick — a position ~RTT old — discarding every input still in flight.
                        // At 100 ms RTT and run speed that rewound the player ~0.5 m on every real
                        // correction. Worse, it wrote only the m_localPlayer alias, and the next
                        // tick's syncNetPlayerToLocalPlayer copied the (uncorrected) NetPlayer
                        // mirror straight back over it — sub-metre corrections were silently ERASED
                        // one tick after being applied. (The >1 m teleport snap in Client::reconcile
                        // writes both mirrors; this path didn't.)
                        //
                        // Now: rewind a scratch NetPlayer to the server's acked state, re-apply
                        // every stored input newer than the ack through the SAME per-input step the
                        // server drain runs (updateNetPlayerFromInput movementOnly + moveAndSlide),
                        // and commit to BOTH mirrors. In-flight inputs survive the correction, so
                        // the render offset only hides the true error, not an artificial rewind.
                        if (distSq > 25.0f) {
                            // Server-driven teleport (respawn, floor door, anti-cheat clamp):
                            // Client::reconcile's >1 m snap already moved both mirrors this tick,
                            // and the ring's history belongs to the pre-teleport world — replaying
                            // it on top of the jump would be nonsense. Teleports are MEANT to cut;
                            // restart prediction history from here.
                            PredictionRingOps::reset(m_predictionRing[sp]);
                        } else {
                            NetPlayer rp = m_players[mySlot];   // carries status/speed for the replay
                            rp.position = serverPos;
                            rp.velocity = { Quantize::unpackVel(sp_.velX),
                                            e->state.velocity.y,   // velY isn't on the wire; predicted value
                                            Quantize::unpackVel(sp_.velZ) };
                            rp.onGround = (sp_.flags & (1 << 1)) != 0;

                            // Same obstacle source as the local move (N4: interp pool on CLIENT).
                            auto* obs = static_cast<CollisionObstacle*>(
                                s_frameAllocator.alloc(MAX_ENTITIES * sizeof(CollisionObstacle)));
                            u32 obsCount = 0;
                            for (u32 a = 0; a < m_renderInterp.entities.activeCount; a++) {
                                const Entity& oe = m_renderInterp.entities.entities[
                                    m_renderInterp.entities.activeList[a]];
                                if (oe.flags & ENT_DEAD) continue;
                                if (oe.flags & ENT_FRIENDLY) continue;
                                if (oe.enemyType == EnemyType::PROP) continue;
                                obs[obsCount++] = {oe.position, oe.halfExtents};
                            }

                            // Replay oldest→newest. Static: 256 × sizeof(NetInput) is too big to
                            // put on the stack every correction; single-threaded engine, safe.
                            static NetInput s_replayInputs[PREDICTION_RING_CAPACITY];
                            const u32 n = PredictionRingOps::collectInputsAfter(
                                m_predictionRing[sp], ackedTick, s_replayInputs, PREDICTION_RING_CAPACITY);
                            constexpr f32 STEP = 1.0f / 60.0f;   // inputs are captured per fixed tick
                            for (u32 k = 0; k < n; k++) {
                                const NetInput& in = s_replayInputs[k];
                                PlayerController::updateNetPlayerFromInput(rp, in, STEP, /*movementOnly=*/true);
                                if (!rp.noclip) {
                                    // Mirror of the server drain's integration step (engine_net.cpp
                                    // serverNetPre): temp Player through the shared moveAndSlide.
                                    Player tp;
                                    tp.position = rp.position;
                                    tp.velocity = rp.velocity;
                                    tp.onGround = rp.onGround;
                                    tp.noclip   = false;
                                    Collision::moveAndSlide(tp, m_level.grid, STEP, obs, obsCount);
                                    rp.position = tp.position;
                                    rp.velocity = tp.velocity;
                                    rp.onGround = tp.onGround;
                                }
                                // Rewrite history so the NEXT ack compares the server against the
                                // corrected prediction — otherwise this one divergence would re-fire
                                // on every ack until the stale entries aged out of the ring.
                                if (PredictionEntry* pe =
                                        PredictionRingOps::findMut(m_predictionRing[sp], in.clientTick)) {
                                    pe->state.position = rp.position;
                                    pe->state.velocity = rp.velocity;
                                    pe->state.onGround = rp.onGround;
                                }
                            }

                            // Commit to BOTH mirrors: the alias write persists via swapOutPlayer;
                            // the NetPlayer write is what survives the next tick's
                            // syncNetPlayerToLocalPlayer (this is the fix for the erased corrections).
                            const Vec3 visibleDelta = m_localPlayer.position - rp.position;
                            m_localPlayer.position = rp.position;
                            m_localPlayer.velocity = rp.velocity;
                            m_localPlayer.onGround = rp.onGround;
                            NetPlayer& live = m_players[mySlot];
                            live.position = rp.position;
                            live.velocity = rp.velocity;
                            live.onGround = rp.onGround;
                            RenderOffsetOps::accumulate(m_renderOffset[sp], visibleDelta);
                        }
                    }
                }
                m_lastReconciledTick[sp] = ackedTick;
            }
        }
        swapOutPlayer(sp);
    }
    swapInPlayer(0); // restore lane-0 aliases
}

// ---------------------------------------------------------------------------
// Server-side CL_TIME_PING handler (M1.4)
// ---------------------------------------------------------------------------
// Static callback — invoked by Net::poll via s_onTimePing when the server receives a
// CL_TIME_PING. Reads the client's stamped time, pairs it with the current serverTick
// and wall-clock time, and ships SV_TIME_PONG back unreliably so the client can
// compute RTT and bootstrap its clock-sync estimate without trusting the server's
// absolute wall time.
void Engine::onTimePing(u8 playerSlot, const u8* data, u32 size) {
    if (!s_engine) return;
    if (size < sizeof(PacketHeader) + 4) {
        LOG_WARN("net: short CL_TIME_PING from slot %u (%u bytes)", playerSlot, size);
        return;
    }

    // Read client-stamped time (skipping the 4-byte header).
    PacketReader r;
    r.data   = data;
    r.size   = size;
    r.cursor = sizeof(PacketHeader);
    const u32 clientTimeMs = r.readU32();

    // Stamp server state at the moment the ping arrived.
    const u32 serverTick   = s_engine->serverTickNow();
    const u32 serverTimeMs = static_cast<u32>(Clock::getElapsedSeconds() * 1000.0);

    // Build SV_TIME_PONG: 4B header + 4B clientTimeMs + 4B serverTick + 4B serverTimeMs.
    PacketWriter w;
    w.writeU8(static_cast<u8>(NetPacketType::SV_TIME_PONG));
    w.writeU8(0);   // flags/padding
    w.writeU16(0);  // seq reserved
    w.writeU32(clientTimeMs); // echoed unchanged so client computes RTT = now - clientTimeMs
    w.writeU32(serverTick);
    w.writeU32(serverTimeMs);
    Net::sendUnreliable(playerSlot, w.data, w.cursor);
}

// ---------------------------------------------------------------------------
// Client-side SV_TIME_PONG handler (M1.5)
// ---------------------------------------------------------------------------
// Static callback — invoked by Net::poll via s_onTimePong when the client receives a
// SV_TIME_PONG. Strips the 4-byte packet header and passes the 12-byte body to
// Client::handleTimePong, which unpacks clientTimeMs / serverTick / serverTimeMs and
// feeds ClockSyncOps::onPongReceived to bootstrap or refine the clock estimate.
// Clock::getElapsedSeconds() is sampled here (decode time) as the pong-arrival wall time
// so the RTT measurement is as accurate as the packet-receive → callback latency allows.
void Engine::onTimePong(const u8* data, u32 size) {
    if (!s_engine) return;
    // Header guard: net.cpp already checks size >= 16 before dispatching, but be safe.
    if (size < sizeof(PacketHeader) + 12) {
        LOG_WARN("net: short SV_TIME_PONG at onTimePong (%u bytes)", size);
        return;
    }
    // Pass the payload (past the 4-byte header) to the decode function.
    const f64 recvNow = Clock::getElapsedSeconds();
    Client::handleTimePong(data + sizeof(PacketHeader), size - sizeof(PacketHeader),
                           s_engine->m_clockSync, recvNow);
}

// M10.2 — Client-side SV_DAMAGE_DONE handler. The server confirmed that a fire from
// this client hit an entity. Ack the matching PendingHitRing entry so the predicted
// hit-marker state is resolved. If the tick/target pair is not found (e.g. the ring
// was already expired by expireOlderThan), the ack is a safe no-op.
void Engine::onDamageDone(u32 clientTick, u16 targetEntityIdx) {
    if (!s_engine) return;
    PendingHitRingOps::ack(s_engine->m_pendingHits, clientTick, targetEntityIdx);
}

// M10.3 — Client-side SV_DAMAGE_TO_ME handler. The server confirmed a projectile hit
// the local player. Ack the matching PendingDamageRing entry so predicted incoming-
// damage visual state (damageFlash / hurtVignette) is resolved correctly.
// The damage value is received but not currently applied locally — HP follows the
// next authoritative snapshot to avoid flicker on mispredicts.
void Engine::onDamageToMe(u32 projectileSrcKey, f32 damage) {
    if (!s_engine) return;
    (void)damage; // damage value received for future use (e.g. precise HP prediction)
    PendingDamageRingOps::ack(s_engine->m_pendingDamage, projectileSrcKey);
}

// D1.1 — Client-side SV_KILL handler. Tallies the guest's own kills; future work can also
// drive a kill-feed HUD, positional audio, or XP UI from this event.
void Engine::onKill(u8 killerSlot, u8 victimType, u16 victimIdx,
                    u8 weaponMeshId, u8 isCrit) {
    (void)weaponMeshId; // reserved for future kill-feed weapon icon
    if (!s_engine) return;
    // Each player tallies their OWN kills — the mirror of the host-side rule in
    // handleFirstKillDrop. The client's ghost sim never runs the authoritative death callback
    // (N5 gates it off before the kill-tracking phase), so this broadcast is the only place a
    // guest learns its kill was confirmed. victimType 0 = entity; player kills (1) don't count
    // as "enemies slain". Looped over the local lanes so client-couch (two lanes, one
    // connection) tallies both when it lands.
    if (victimType == 0) {
        for (u8 lane = 0; lane < s_engine->m_splitPlayerCount; lane++) {
            if (killerSlot == s_engine->m_clientNetSlot[lane]) {
                s_engine->m_transition.floorKillCount++;
                s_engine->m_totalKills[lane]++;   // lifetime "Enemies deleted" (stats sidecar)
                break;
            }
        }
    }
    LOG_INFO("net: kill event — killer=%u victimType=%u victimIdx=%u crit=%u",
             killerSlot, victimType, victimIdx, isCrit);
}

// Grant energy ("mana") to a player slot from a host-authoritative source (projectile manasteal /
// mana-on-kill). Local lanes (SP / host-local) apply immediately; a remote guest's gain is
// coalesced into m_pendingEnergyGain and shipped as one SV_ENERGY_GAIN per guest per tick in
// serverNetPost — the guest can't observe its own projectile hits / kills to predict them.
void Engine::grantEnergy(u8 slot, f32 amount) {
    if (amount <= 0.0f || slot >= MAX_PLAYERS) return;
    bool localLane = (m_netRole != NetRole::CLIENT) && (slot < m_splitPlayerCount);
    if (localLane) {
        SkillState& ss = m_skillStates[slot];
        ss.energy += amount;
        if (ss.energy > ss.maxEnergy) ss.energy = ss.maxEnergy;
    } else if (m_netRole == NetRole::SERVER && m_players[slot].active) {
        m_pendingEnergyGain[slot] += amount; // flushed in serverNetPost
    }
}

// Client-side SV_ENERGY_GAIN handler. Adds server-granted energy (manasteal / mana-on-kill the
// guest couldn't predict) to the local player's pool, clamped to max.
// TODO(client-couch): the payload carries no lane discriminator, so this credits m_localPlayerIndex.
// Correct while a client has one local lane; client-couch (two lanes on one connection) will need a
// u8 target-lane byte in the SV_ENERGY_GAIN payload (+ a matching size-guard bump) to address it.
void Engine::onEnergyGain(f32 amount) {
    if (!s_engine || amount <= 0.0f) return;
    SkillState& ss = s_engine->m_skillStates[s_engine->m_localPlayerIndex];
    ss.energy += amount;
    if (ss.energy > ss.maxEnergy) ss.energy = ss.maxEnergy;
}

// D1.2/D4.2 — Client-side SV_PICKUP_RESULT handler.
//
// On accept: clear the predicted flag on the inventory slot — the item is now confirmed real.
// On reject: call removeFromBackpack to roll back the predicted add. The world item will be
//            restored by the next snapshot's mirrorWorldItems (M8 behavior).
// Always acks the ring entry so expireOlderThan doesn't need to clean it up.
void Engine::onPickupResult(u8 accept, u32 itemUid) {
    if (!s_engine) return;
    s8 slot = PendingPickupRingOps::findSlotByUid(s_engine->m_pendingPickups, itemUid);
    if (accept) {
        // Server confirmed — predicted slot is already in the inventory; nothing to do.
        // Rollback would be the only divergence; since accept matched, leave as-is.
        // First WORLD pickup, guest edition (the SP/host lane unlocks at its local pickup
        // site). slot >= 0 = a real predicted bag add — shrine activations ride the same
        // packet but never enter the pending ring, so they can't count as an "item".
        if (slot >= 0) Steam::unlockAchievement("ACH_FIRST_ITEM");
    } else {
        // Server rejected — roll back the predicted inventory add, in the LANE that predicted
        // it. This handler fires during Net::poll, so m_localPlayerIndex is just whatever lane
        // was swapped in last — on an online-couch client that could be the WRONG player's bag.
        if (slot >= 0) {
            u8 lane = PendingPickupRingOps::findLaneByUid(s_engine->m_pendingPickups, itemUid);
            if (lane >= MAX_LOCAL_PLAYERS) lane = 0;
            Inventory::removeFromBackpack(
                s_engine->m_inventories[lane],
                static_cast<u8>(slot));
        }
        // World item presence is restored by mirrorWorldItems on the next snapshot.
    }
    PendingPickupRingOps::ack(s_engine->m_pendingPickups, itemUid);
    LOG_INFO("net: pickup result — uid=%u accept=%u slot=%d", itemUid, accept, (int)slot);
}

// D1.3 — Client-side SV_LOOT_SPAWN handler. v1: log for diagnostics. The snapshot
// already mirrors the item visually via mirrorWorldItems, so no additional client
// state is required. Future work: pin a loot icon on the minimap immediately.
void Engine::onLootSpawn(u32 uid, f32 posX, f32 posY, f32 posZ, u16 itemDefId) {
    (void)posX; (void)posY; (void)posZ; // future: minimap pin
    LOG_INFO("net: loot spawn — uid=%u defId=%u pos=(%.1f,%.1f,%.1f)",
             uid, itemDefId, posX, posY, posZ);
}

// ---------------------------------------------------------------------------
// Steam matchmaking glue (P2). netHostGame picks the transport at host time;
// beginSteamJoin routes an accepted lobby invite into the join flow.
// ---------------------------------------------------------------------------
bool Engine::netHostGame(u8 localPlayerCount) {
    // Online + Steam available → relay host + a PUBLIC lobby. Public (not friends-only) is required for
    // the in-game public browser (RequestLobbyList only returns k_ELobbyTypePublic lobbies — a
    // friends-only lobby is invite/friends-joinable but invisible to the browser, which is exactly why
    // "Browse Games" returned 0 results). Public is a strict superset: invites, friends-list "Join Game",
    // AND browser discovery all work.
    //
    // PRIVATE lobbies are created as the SAME public Steam lobby TYPE, and hidden by our own
    // `private` metadata flag (the browser query filters private=="0"; a code lookup ignores it).
    // This is deliberate and load-bearing: a k_ELobbyTypeFriendsOnly lobby is not returned by
    // RequestLobbyList AT ALL, so a 4-glyph code — which works by *searching* for the lobby that
    // published it — could never find one. Making the lobby friends-only would silently break the
    // very feature the private option exists to enable. See lobby_code.h for the full trade.
    // Otherwise fall back to the ENet path (Online = UPnP, else LAN-only).
    if (m_menu.hostOnline && Steam::isAvailable()) {
        if (!Net::hostServerSteam(localPlayerCount)) return false;
        Steam::createLobby(/*friendsOnly=*/false, static_cast<int>(MAX_PLAYERS)); // data set in onLobbyCreated
        LOG_INFO("Steam: hosting via relay + %s lobby (%u local)",
                 m_menu.hostPrivate ? "PRIVATE (code/invite only)" : "public", localPlayerCount);
        return true;
    }
    return Net::hostServer(DEFAULT_PORT, m_menu.hostOnline, localPlayerCount);
}

void Engine::beginSteamJoin(u64 hostSteamId) {
    if (m_gameState != GameState::MENU) {   // v1: only join from the menu (finish current game first)
        LOG_WARN("Steam: ignoring lobby join while not at the menu");
        return;
    }
    m_steamJoinHost        = hostSteamId;
    m_netRole              = NetRole::CLIENT;
    m_localPlayerIndex     = 0;
    m_clientLoadedFromSave = false;
    scanSaveSlots();
    // Skip the "Enter Host IP" screen — go straight to New/Continue, then class, then the connect
    // step (which routes through connectToSteamHost because m_steamJoinHost is set).
    m_menu.subState    = 1;
    m_menu.subSelection = 0;
    LOG_INFO("Steam: joining host %llu — pick New/Continue then class", (unsigned long long)hostSteamId);
}

void Engine::steamQuickJoin() {
    if (!Steam::isAvailable()) return;
    m_steamMenuMode = 1;
    char ver[16]; std::snprintf(ver, sizeof(ver), "%u", PROTOCOL_VERSION);
    Steam::requestLobbyList(ver);   // -> onSteamLobbyList
    LOG_INFO("Steam: quickmatch — searching for a game...");
}

void Engine::steamBrowse() {
    if (!Steam::isAvailable()) return;
    m_steamMenuMode = 2;
    m_steamBrowserSel = 0;
    m_steamBrowserSearching = true;   // render "Searching…" until onSteamLobbyList answers
    char ver[16]; std::snprintf(ver, sizeof(ver), "%u", PROTOCOL_VERSION);
    Steam::requestLobbyList(ver);
    LOG_INFO("Steam: browsing public lobbies...");
}

// Join a game by its 4-glyph share code. The code is a LOOKUP KEY, not an encoding of the lobby id
// (4 glyphs = 20 bits can't hold 64), so we ask Steam for the lobby that published this exact code.
// Private lobbies are excluded from the browser but remain findable this way — that's the whole
// point of the feature. `code` must already be normalized (LobbyCode::normalize) so it matches the
// host's published string byte-for-byte.
void Engine::steamJoinByCode(const char* code) {
    if (!Steam::isAvailable() || !code || !code[0]) return;
    m_steamMenuMode = 3;                  // -> onSteamLobbyList's code branch
    m_steamBrowserSearching = true;       // the code screen shows "Searching..." while we wait
    m_menu.codeNotFound = false;
    char ver[16]; std::snprintf(ver, sizeof(ver), "%u", PROTOCOL_VERSION);
    Steam::requestLobbyListByCode(ver, code);
    LOG_INFO("Steam: looking up lobby code %s...", code);
}

void Engine::onSteamLobbyList(int count) {
    if (m_gameState != GameState::MENU) { m_steamMenuMode = 0; return; }
    if (m_steamMenuMode == 3) {           // join-by-code lookup
        m_steamMenuMode = 0;
        m_steamBrowserSearching = false;
        if (count > 0) {
            char nm[64]; int mc = 0, mm = 0;
            u64 id = Steam::lobbyListEntry(0, nm, sizeof(nm), &mc, &mm);   // filter matched exactly one
            if (id) { Steam::joinLobby(id); return; }  // -> onLobbyEntered -> beginSteamJoin
        }
        // No lobby published that code: it's mistyped, or the game ended/filled. Say so on the code
        // screen instead of bouncing the player somewhere unexpected — they'll want to retype it.
        m_menu.codeNotFound = true;
        LOG_INFO("Steam: no game found for that lobby code");
        return;
    }
    if (m_steamMenuMode == 1) {           // quickmatch
        m_steamMenuMode = 0;
        if (count > 0) {
            char nm[64]; int mc = 0, mm = 0;
            u64 id = Steam::lobbyListEntry(0, nm, sizeof(nm), &mc, &mm);  // Steam sorts best-first
            if (id) { Steam::joinLobby(id); return; }   // -> onLobbyEntered -> beginSteamJoin
        }
        // No public games found → route to the host flow (Online preselected) so we become joinable.
        LOG_INFO("Steam: no games found — host one");
        m_netRole = NetRole::SERVER;
        m_localPlayerIndex = 0;
        m_menu.hostOnline  = true;
        scanSaveSlots();
        m_menu.subState     = 10;   // LAN/Online chooser
        m_menu.subSelection = 1;    // Online
    } else if (m_steamMenuMode == 2) {    // browser: the list substate renders Steam::lobbyList*
        m_steamBrowserSel = 0;
        m_steamBrowserSearching = false;  // results are in — the list (or the empty state) can render
        LOG_INFO("Steam: browser found %d public game(s)", count);
    }
}

