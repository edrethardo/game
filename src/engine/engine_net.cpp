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
    m_serverTick++;

    // Capture local input and push into server's input buffer. Pass m_localPlayer
    // so captureLocalInput can pack absolute yaw/pitch/position (PROTOCOL_VERSION 2
    // wire layout); the host's own input still rides the same NetInput shape so the
    // server's snapshot lastInputTick is uniform across all slots.
    WeaponState& ws = m_players[m_localPlayerIndex].weaponState;
    NetInput localInput = PlayerController::captureLocalInput(m_localPlayer, m_serverTick, ws.currentWeapon);
    Server::getInputBuffer(m_localPlayerIndex).push(localInput);
    // Track the host's own ack too. The host moves in gameUpdate (not the remote loop
    // below), so without this its lastInputTick stays 0 forever on the wire — harmless
    // today (clients only reconcile their own slot) but a trap for any future feature
    // that reads slot 0's ack (spectate/lag-comp/debug).
    m_players[m_localPlayerIndex].lastProcessedInputTick = localInput.tick;

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
    for (u32 i = 0; i < MAX_PLAYERS; i++) {
        if (i == m_localPlayerIndex) continue; // host moves in gameUpdate
        NetPlayer& np = m_players[i];
        if (!np.active) continue;
        InputRingBuffer& buf = Server::getInputBuffer(i);
        for (u32 k = 0; k < buf.count; k++) {
            // oldest→newest walk: head points one past the newest write; count entries trail back from there
            u32 idx = (buf.head + INPUT_BUFFER_SIZE - buf.count + k) % INPUT_BUFFER_SIZE;
            const NetInput& in = buf.inputs[idx];
            if (in.tick <= np.lastProcessedInputTick) continue; // already applied last tick
            // Always advance the ack and mirror the weapon, even while dead, so the
            // client's reconcile keeps a fresh ack across the death window. But DON'T
            // drive movement/aim from input while dead — a corpse that died holding a
            // move key or aiming somewhere would keep updating each tick.
            np.lastProcessedInputTick = in.tick;
            if (in.weaponId < m_weaponDefCount)
                np.weaponState.currentWeapon = in.weaponId;
            if (!np.isDead) {
                // CLIENT-AUTHORITATIVE POSITION: snap NetPlayer.position to the absolute
                // position the client reported, with a max-delta sanity clamp to keep
                // flagrant teleport-cheats from working. This eliminates the divergence
                // between client-side prediction-collision (against interp entities) and
                // server-side moveAndSlide (against live entities) that produced the MED
                // reconcile snaps (~0.2-1 m every few seconds). Combat / HP / loot stay
                // server-authoritative; the only thing we trust the client for is its own
                // position. Co-op trade: a cheating client could phase through enemies on
                // their own screen but can't fake damage or steal kills.
                Vec3 reported{Quantize::unpackPos(in.posXQ),
                              Quantize::unpackPos(in.posYQ),
                              Quantize::unpackPos(in.posZQ)};
                // Cap per-tick movement at 4× the player's nominal speed × dt — a real
                // sprint+jump+dodge is well under this; a teleport hack is way over.
                // Reject by holding the prior position so the cheating client snaps in
                // place visibly to others (server is still the source of truth on the wire).
                Vec3 delta = reported - np.position;
                f32 maxStep = np.moveSpeed * 4.0f * dt + 0.5f; // +0.5 m slack for spawns / floor transitions
                if (lengthSq(delta) > maxStep * maxStep) {
                    // Out-of-bounds move: keep np.position unchanged this tick.
                    // (No log spam: a single warn-once would be fine to add if cheating
                    //  becomes a real concern; for now silent reject is sufficient.)
                } else {
                    np.position = reported;
                }
                // Yaw/pitch + movement state (velocity, onGround) come from this call —
                // updateNetPlayerFromInput now SETS np.yaw/pitch absolutely from the
                // input (no longer applies a mouse delta), and runs applyMovement with
                // zero look-delta to compute velocity / WASD-derived motion / jump.
                // applyMovement also writes np.position, so we set position again AFTER
                // it to keep client-authoritative.
                Vec3 preMovePos = np.position;
                PlayerController::updateNetPlayerFromInput(np, in, dt);
                // applyMovement integrated np.position by velocity*dt against the grid.
                // Discard that — the client's reported position is the truth — but keep
                // the velocity, onGround, yaw, pitch, etc. it computed.
                np.position = preMovePos;
            }
        }
    }

    // (Server-side remote moveAndSlide block intentionally removed: position is now
    // client-authoritative, set above from in.posXQ/Y/Z. Server doesn't need to slide
    // remote players against the grid because the client already did that during its
    // own prediction. Removes the ghost-vs-live entity collision divergence that
    // was producing reconcile MED snaps.)

    // Remote player weapon fire + extended actions (server-authoritative)
    for (u32 i = 0; i < MAX_PLAYERS; i++) {
        if (!m_players[i].active) continue;
        if (i == m_localPlayerIndex) continue; // host handled by gameUpdate
        // Don't let a dead remote keep firing: getLatest() repeats the last input on
        // packet loss/idle, so a corpse that died holding FIRE would shoot every tick
        // until it respawns. Mirrors the !isDead gate on the potion/skill actions below.
        if (!m_players[i].isDead) handleWeaponFireForPlayer(m_players[i], dt);

        const NetInput* input = Server::getInputBuffer(static_cast<u8>(i)).getLatest();
        if (input) {
            // Potion (with per-player cooldown)
            if ((input->extFlags & INPUT_EX_POTION) &&
                m_players[i].potionCooldown <= 0.0f && !m_players[i].isDead) {
                f32 healAmt = m_players[i].maxHealth * GameConst::POTION_HEAL_PCT;
                m_players[i].health += healAmt;
                if (m_players[i].health > m_players[i].maxHealth)
                    m_players[i].health = m_players[i].maxHealth;
                m_players[i].potionCooldown = GameConst::POTION_COOLDOWN;
            }

            // Equipment skills (F = boots, G = helmet)
            if ((input->extFlags & INPUT_EX_BOOT_SKILL) && !m_players[i].isDead) {
                const ItemInstance& boots = m_inventories[i].equipped[static_cast<u32>(ItemSlot::BOOTS)];
                if (!isItemEmpty(boots) && boots.rarity == Rarity::LEGENDARY) {
                    SkillId bootSkill = m_itemDefs[boots.defId].legendarySkillId;
                    if (bootSkill != SkillId::NONE) {
                        SkillState ss; ss.activeSkill = bootSkill; ss.energy = 999.0f; ss.maxEnergy = 999.0f;
                        Vec3 ep = m_players[i].eyePos();
                        Vec3 fwd = normalize(Vec3{-sinf(m_players[i].yaw)*cosf(m_players[i].pitch),
                                                    sinf(m_players[i].pitch),
                                                   -cosf(m_players[i].yaw)*cosf(m_players[i].pitch)});
                        // TA-7: set skill-scaling globals from the REMOTE's own data so the
                        // guest's skill damage isn't inherited from the host's last cast.
                        // Item skills scale by item level (boots) and use base class damage (1.0).
                        SkillSystem::setSkillPower(boots.itemLevel > 1
                            ? static_cast<f32>(boots.itemLevel - 1) / 149.0f : 0.0f);
                        SkillSystem::setClassDamageMult(1.0f);
                        // Stamp the remote's net slot so per-slot skill state (overcharge buff,
                        // meteor/holy kill-heal target) lands on the actual caster, not the host.
                        // (H4/H5: overcharge arrays are MAX_PLAYERS-sized; updateMeteors resolves
                        //  the heal target against m_players by net slot.)
                        SkillSystem::setCastingPlayer(static_cast<u8>(i));
                        // TA-3: cast against the GUEST's own view, not the host's m_localPlayer,
                        // so position/health-mutating skills (PhaseDash, Blood Nova) hit the guest.
                        Player view; buildRemotePlayerView(static_cast<u8>(i), view);
                        SkillSystem::tryActivate(ss, m_skillDefs, m_skillDefCount,
                                                  ep, fwd, m_players[i].yaw,
                                                  m_projectiles, m_entities, m_level.grid, view);
                        applyRemotePlayerView(view, static_cast<u8>(i));
                    }
                }
            }
            if ((input->extFlags & INPUT_EX_HELM_SKILL) && !m_players[i].isDead) {
                const ItemInstance& helm = m_inventories[i].equipped[static_cast<u32>(ItemSlot::HELMET)];
                if (!isItemEmpty(helm) && helm.rarity == Rarity::LEGENDARY) {
                    SkillId helmSkill = m_itemDefs[helm.defId].legendarySkillId;
                    if (helmSkill != SkillId::NONE) {
                        SkillState ss; ss.activeSkill = helmSkill; ss.energy = 999.0f; ss.maxEnergy = 999.0f;
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
                        SkillSystem::tryActivate(ss, m_skillDefs, m_skillDefCount,
                                                  ep, fwd, m_players[i].yaw,
                                                  m_projectiles, m_entities, m_level.grid, view);
                        applyRemotePlayerView(view, static_cast<u8>(i));
                    }
                }
            }

            // Class skill activation (right-click) — use remote player's class.
            // Gate on !isDead like potion/boot/helm above; a latched INPUT_EX_SKILL on a
            // dead remote would otherwise keep casting (teleporting/healing the corpse).
            if ((input->extFlags & INPUT_EX_SKILL) && !m_players[i].isDead) {
                u8 slot = input->skillSlot;
                PlayerClass remoteClass = m_players[i].playerClass;
                if (slot < 4 && static_cast<u32>(remoteClass) < static_cast<u32>(PlayerClass::CLASS_COUNT)) {
                    const ClassDef& cls = kClassDefs[static_cast<u32>(remoteClass)];
                    // Mirror the host's effectiveFloor gate (engine_update_skills.cpp):
                    // difficulty adds +50/floor so remote clients unlock skills at the
                    // same depth their own HUD shows, instead of the raw floor.
                    u32 effectiveFloor = m_level.currentFloor + m_difficulty * 50;
                    if (effectiveFloor >= cls.skillUnlockFloor[slot]) {
                        SkillState tempSS;
                        tempSS.activeSkill = cls.skills[slot];
                        tempSS.cooldownTimer = 0.0f;
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
                        SkillSystem::setClassDamageMult(1.0f + (effectiveFloor - 1) * 0.06f);
                        // Weapon damage for Marksman skills that scale off the equipped weapon.
                        { const ItemInstance& wpn = m_inventories[i].equipped[static_cast<u32>(ItemSlot::WEAPON)];
                          WeaponDef wd = !isItemEmpty(wpn)
                              ? Inventory::getWeaponFromItem(m_inventories[i], m_itemDefs, wpn)
                              : m_weaponDefs[0];
                          SkillSystem::setWeaponDamage(wd.damage); }
                        SkillSystem::setCastingPlayer(static_cast<u8>(i)); // caster's net slot (H4/H5)
                        // TA-3: cast against the GUEST's own view (see boot-skill note above)
                        // so class dash/blink/Blood Nova mutate the guest, never the host.
                        Player view; buildRemotePlayerView(static_cast<u8>(i), view);
                        SkillSystem::tryActivate(tempSS, m_skillDefs, m_skillDefCount,
                                                  eyePos, fwd, m_players[i].yaw,
                                                  m_projectiles, m_entities, m_level.grid, view);
                        applyRemotePlayerView(view, static_cast<u8>(i));
                    }
                }
            }
        }
    }

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
    }

    // Server-side globe auto-pickup for remote players
    for (u32 wi = 0; wi < MAX_WORLD_ITEMS; wi++) {
        WorldItem& item = m_worldItems.items[wi];
        if (!item.active || !isGlobe(item.item)) continue;
        for (u32 pi = 0; pi < MAX_PLAYERS; pi++) {
            if (pi == m_localPlayerIndex) continue; // host pickup handled in gameUpdate
            if (!m_players[pi].active || m_players[pi].isDead) continue;
            Vec3 delta = m_players[pi].position - item.position;
            f32 dist = sqrtf(delta.x * delta.x + delta.z * delta.z);
            if (dist < 3.0f) {
                f32 healAmt = m_players[pi].maxHealth * GameConst::GLOBE_HEAL_PCT;
                m_players[pi].health += healAmt;
                if (m_players[pi].health > m_players[pi].maxHealth)
                    m_players[pi].health = m_players[pi].maxHealth;
                item.active = false;
                if (m_worldItems.activeCount > 0) m_worldItems.activeCount--;
                break;
            }
        }
    }

    // Damage flash decay for remote players
    for (u32 i = 0; i < MAX_PLAYERS; i++) {
        if (i == m_localPlayerIndex) continue; // host handled in gameUpdate
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

        if (pi != m_localPlayerIndex) {
            if (np.invulnTimer > 0.0f) { np.invulnTimer -= dt; if (np.invulnTimer < 0.0f) np.invulnTimer = 0.0f; }
            if (np.slowTimer > 0.0f)   np.slowTimer -= dt;
            if (np.freezeTimer > 0.0f) np.freezeTimer -= dt;
            if (np.potionCooldown > 0.0f) np.potionCooldown -= dt;

            if (np.invulnTimer <= 0.0f) {
                if (np.poisonTimer > 0.0f) { np.poisonTimer -= dt; np.health -= np.poisonDps * dt; }
                if (np.burnTimer > 0.0f)   { np.burnTimer -= dt;   np.health -= np.burnDps * dt;   }
            } else {
                np.poisonTimer = 0.0f; np.burnTimer = 0.0f;
                np.freezeTimer = 0.0f; np.slowTimer = 0.0f;
            }
        }

        if (np.health <= 0.0f) {
            np.health = 0.0f;
            np.isDead = true;
            LOG_INFO("Player %u died", pi);
            if (pi == m_localPlayerIndex) {
                m_playerDead[0] = true; // don't freeze the server
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
                    case SkillId::METEOR_STRIKE: if (dist < 3.0f) { ent.burnTimer = 0.5f; ent.burnDps = 2.0f; } break;
                    case SkillId::FROZEN_ORB: if (dist < 4.0f) { ent.freezeTimer = 0.5f; } break;
                    case SkillId::BLOOD_NOVA: if (dist < 3.0f) { ent.poisonTimer = 0.5f; ent.poisonDps = 1.0f; } break;
                    case SkillId::CHAIN_LIGHTNING: if (dist < 3.0f) { ent.freezeTimer = 0.3f; } break;
                    case SkillId::PHASE_DASH: if (dist < 3.0f) { ent.freezeTimer = 0.4f; } break;
                    default: break;
                }
            }
        }

        // (M9) Defensive ring passives for REMOTE players. The host's ring passives are ticked
        // in gameUpdate via tickArmorRingPassives — we skip slot 0 here to avoid double-ticking
        // Second Wind / Divine Judgment / Soul Harvest decay on the host.
        if (pi != m_localPlayerIndex && np.ringPassive != SkillId::NONE) {
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
        if (pi != m_localPlayerIndex) {
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
        if ((s_snapTxLogCounter++ % 5) == 0) {
            u32 activePlayers = 0;
            for (u32 i = 0; i < MAX_PLAYERS; i++) if (m_players[i].active) activePlayers++;
            LOG_INFO("[AUDIT-P1] snap tx tick=%u players=%u ents=%u projs=%u items=%u",
                     m_serverTick, activePlayers,
                     m_entities.activeCount, m_projectiles.activeCount,
                     m_worldItems.activeCount);
        }
        Server::sendSnapshot(m_serverTick, m_players, m_entities, m_projectiles, m_worldItems);
        // Phase 3.1 — Capture entity poses at this snapshot tick into the lag-comp
        // history. We push only on snapshot ticks (every TICKS_PER_SNAP server ticks)
        // because that's the cadence the client renders from — every history entry
        // corresponds exactly to a snapshot the client received and could be aiming at.
        pushEntityHistory();
    }
}

// ---------------------------------------------------------------------------
// Client networking — pre-gameplay: predict, reconcile
// ---------------------------------------------------------------------------
void Engine::clientNetPre(f32 dt) {
    // Handle server disconnection gracefully
    if (!Net::isConnected()) {
        LOG_WARN("Lost connection to server");
        Net::disconnect();
        m_netRole = NetRole::NONE;
        m_gameState = GameState::MENU;
        Input::setRelativeMouseMode(false);
        return;
    }

    m_serverTick++;

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
    WeaponState& ws = m_players[activeNetSlot()].weaponState; // local player's net slot
    Client::captureAndSendInput(m_localPlayer, m_serverTick, ws.currentWeapon, m_activeClassSkill);

    // Phase 1.1 — Retransmit the most recent CL_FIRE_WEAPON for FIRE_TX_REPEATS ticks.
    // The original send was unreliable, so a lost UDP fragment doesn't get retransmitted
    // by ENet on its own; we instead push the same bytes a few more times in quick
    // succession and let the server's per-clientTick dedup ring squash the duplicates.
    resendPendingFire();

    // No client-side prediction step anymore — under the trust-client position model,
    // m_localPlayer is the authoritative source for the local camera/transform (updated
    // in gameUpdate via PlayerController::update). The server adopts the absolute yaw/
    // pitch/position the client packed in NetInput and mirrors them back via snapshot.
    // Running a second prediction here on NetPlayer would produce a post-movement
    // position that diverges from the pre-movement position the server adopts (every
    // frame), and the snap-back via syncNetPlayerToLocalPlayer would feed double-applied
    // mouse delta into the camera — caused the "can't aim" + "shaking" pair after the
    // absolute-aim change. Reconcile is now HP/status-only (+ a loose-threshold safety
    // snap for legitimate teleports).
    Client::reconcile(m_players[activeNetSlot()], m_localPlayer);

    // CLIENT death is server-authoritative: reconcile adopted the server's isDead into our net
    // slot; mirror it into the lane-indexed m_playerDead that the dead-branch + HUD overlay read
    // (lane is 0 on a client). Auto-clears when the server respawns us — no sticky local flag,
    // no optimistic-revive flicker.
    m_playerDead[m_localPlayerIndex] = m_players[activeNetSlot()].isDead;
}

// ---------------------------------------------------------------------------
// Client networking — post-gameplay: interpolate remote state
// ---------------------------------------------------------------------------
void Engine::clientNetPost(f32 dt) {
    // gameUpdate already synced m_localPlayer → NetPlayer at its end.

    // Interpolate remote players, entities, and projectiles from server snapshots
    Client::interpolateRemotePlayers(activeNetSlot(),
        m_renderInterp.playerPositions, m_renderInterp.playerYaws,
        m_renderInterp.playerActive, m_renderInterp.playerHealth, m_renderInterp.playerMaxHealth,
        m_renderInterp.playerAnimFlags, m_renderInterp.playerWeaponMeshId,
        m_renderInterp.playerClass);
    Client::interpolateEntities(m_renderInterp.entities, dt);
    // Boss / non-boss halfExtents are now wire-authoritative per SnapEntity (Audit P2 #4).
    // The prior post-pass that looked up BossDef::halfExtents here was made redundant by
    // that wire change; removed to keep the floor lookup out of every snapshot tick.
    Client::interpolateProjectiles(m_renderInterp.projectiles);

    // V2 fire prediction — match-and-despawn pass.
    // Each snapshot projectile carrying clientTickLow != 0 and ownerSlot == this client's
    // slot corresponds to a locally-predicted ghost we spawned in handleWeaponFire. Find
    // the matching ghost by clientTick low 16 bits and deactivate it — the authoritative
    // snapshot projectile (now in m_renderInterp.projectiles) takes over rendering.
    //
    // NOTE: we iterate the pool DIRECTLY rather than the activeList here. Client::interpolate
    // Projectiles populates m_renderInterp.projectiles[poolIndex] and increments activeCount,
    // but does NOT update activeList[] — so activeList is stale from previous frames' merge
    // appends (predicted-ghost slot indices that are now inactive after the per-frame clear).
    // The renderer iterates the pool the same way (engine_render_effects.cpp:75), using
    // activeCount only as an early-exit hint. Match-despawn follows that canonical pattern.
    u8 mySlot = activeNetSlot();
    for (u32 idx = 0; idx < MAX_PROJECTILES; idx++) {
        const Projectile& authoritative = m_renderInterp.projectiles.projectiles[idx];
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
            ghost.active = false;   // authoritative has taken over rendering — despawn ghost
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

    // [AUDIT-P1] Diagnostic: what does the RENDER pool actually contain after interp? If
    // [AUDIT-P1] snap rx showed non-zero ents/projs but this shows 0, the rebuild-activeList
    // path (the C1 fix) is broken or m_renderInterp is being clobbered post-interp. Throttled
    // to every 30th frame (~2 Hz at 60 fps) — interp runs once per CLIENT update tick.
    {
        static u32 s_interpLogCounter = 0;
        if ((s_interpLogCounter++ % 30) == 0) {
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

