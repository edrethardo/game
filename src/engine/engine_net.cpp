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
    // D5: sync the fake-latency cvar into the net layer, then flush any delayed
    // packets whose delivery timestamp has elapsed. Done first so a queued snapshot
    // from the prior frame arrives before we build + broadcast the new one.
    Net::setFakeLatencyMs(m_netFakeLatencyMs);
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
    // Build the obstacle list ONCE for this server tick. m_entities is stable for the
    // duration of serverNetPre — entity spawns/deaths run later in gameUpdate. Live
    // enemies block; friendly NPCs + props don't (mirrors the host's gameUpdate pass).
    CollisionObstacle obs[MAX_ENTITIES];
    u32 obsCount = 0;
    for (u32 ei = 0; ei < MAX_ENTITIES; ei++) {
        const Entity& e = m_entities.entities[ei];
        if (!(e.flags & ENT_ACTIVE) || (e.flags & ENT_DEAD)) continue;
        if (e.flags & ENT_FRIENDLY) continue;
        if (e.enemyType == EnemyType::PROP) continue;
        obs[obsCount++] = {e.position, e.halfExtents};
    }

    for (u32 i = 0; i < MAX_PLAYERS; i++) {
        if (i == m_localPlayerIndex) continue; // host moves in gameUpdate
        NetPlayer& np = m_players[i];
        if (!np.active) continue;
        InputRingBuffer& buf = Server::getInputBuffer(i);
        for (u32 k = 0; k < buf.count; k++) {
            // oldest→newest walk: head points one past the newest write; count entries trail back from there
            u32 idx = (buf.head + INPUT_BUFFER_SIZE - buf.count + k) % INPUT_BUFFER_SIZE;
            const NetInput& in = buf.inputs[idx];
            if (in.clientTick <= np.lastProcessedInputTick) continue; // already applied last tick
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
                u32 fullAck = (m_serverTick & ~0xFFFFu) | lowAck;
                if (fullAck > m_serverTick) fullAck -= 0x10000; // client ack is from prior window
                m_clientAckedSnap[i] = fullAck;
            }
            if (in.weaponId < m_weaponDefCount)
                np.weaponState.currentWeapon = in.weaponId;
            if (!np.isDead) {
                // SERVER-AUTHORITATIVE POSITION (M2+): server runs PlayerController on the
                // remote slot — updateNetPlayerFromInput computes yaw/pitch from the absolute
                // quantized values in the input, then drives applyMovement to produce the new
                // np.velocity. Position integration happens immediately below, per input.
                PlayerController::updateNetPlayerFromInput(np, in, dt);

                // Integrate np.position by THIS input's velocity for one client-tick (dt).
                // Per-input cadence matches the client's per-tick gameUpdate, so when N
                // inputs arrive batched in one server tick (jitter / packet bursts) the
                // server advances N ticks of motion instead of losing N-1 of them. The
                // previous "one moveAndSlide after the drain loop" pattern under-integrated
                // under any jitter — every dropped tick of motion produced a ~6-12 cm
                // divergence on reconcile, visible as small render-offset jitter on the
                // client. applyMovement writes velocity only; this is where position moves.
                if (!np.noclip) {
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
            }
        }
    }

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
        // D7.3 — Build the snapshot once (no broadcast yet) so we can send each remote
        // client the appropriate encoding: delta if they have a confirmed baseline, full
        // if this is their first snapshot or the baseline has been invalidated.
        // Singleplayer (NetRole::NONE) never enters serverNetPost, so this path is
        // server-only. The old Server::sendSnapshot (broadcast) is retained for any
        // caller that needs the backwards-compatible broadcast, but here we drive
        // per-slot sends directly.
        Server::buildSnapshotOnly(m_serverTick, m_players, m_entities, m_projectiles, m_worldItems);

        // D7.3 — Per-slot send: full or delta based on baseline tracker state.
        for (u32 slot = 0; slot < MAX_PLAYERS; slot++) {
            if (!m_players[slot].active) continue;
            if (slot == static_cast<u32>(m_localPlayerIndex)) continue; // host has no remote peer

            bool sendFull = BaselineTrackerOps::shouldSendFullSnapshot(
                                m_baselines[slot], m_clientAckedSnap[slot]);

            if (sendFull) {
                // No confirmed baseline: send a full snapshot so the client can anchor.
                Server::sendSnapshotFullToSlot(static_cast<u8>(slot));
            } else {
                // Client ACKed our stored baseline tick — send delta against that snapshot.
                Server::sendSnapshotDeltaToSlot(static_cast<u8>(slot), m_baselineSnap[slot]);
            }

            // Advance the baseline after sending so the next tick has a reference point.
            BaselineTrackerOps::store(m_baselines[slot], m_serverTick);
        }

        // D7.3 — Update m_baselineSnap for every active remote slot to the snapshot just
        // built. This is the snapshot the server will delta against on the NEXT tick if
        // the client confirms receipt by echoing this tick's serverTick in its next input.
        {
            const WorldSnapshot* sent = Server::getLastSnapshot();
            if (sent) {
                for (u32 slot = 0; slot < MAX_PLAYERS; slot++) {
                    if (!m_players[slot].active) continue;
                    if (slot == static_cast<u32>(m_localPlayerIndex)) continue;
                    m_baselineSnap[slot] = *sent;
                }
            }
        }
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
    // D5: sync the fake-latency cvar into the net layer, then flush any outgoing
    // packets (CL_INPUT, CL_FIRE_WEAPON, etc.) whose delivery timestamp has elapsed.
    Net::setFakeLatencyMs(m_netFakeLatencyMs);
    Net::pumpDelayQueue();

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
    WeaponState& ws = m_players[activeNetSlot()].weaponState; // local player's net slot
    Client::captureAndSendInput(m_localPlayer, m_clientTick, ws.currentWeapon, m_activeClassSkill);

    // M10.1: resendPendingFire() removed — CL_FIRE_WEAPON is now reliable.

    // M3.2 — Snapshot the local player's state into the prediction ring keyed by
    // m_clientTick. On snapshot arrival (clientNetPost) we compare the server's
    // authoritative position at the ACK'd tick against the matching ring entry.
    {
        PredictedState s;
        s.position    = m_localPlayer.position;
        s.velocity    = m_localPlayer.velocity;
        s.yaw         = m_localPlayer.yaw;
        s.pitch       = m_localPlayer.pitch;
        s.health      = m_localPlayer.health;
        s.invulnTimer = m_localPlayer.invulnTimer;
        s.onGround    = m_localPlayer.onGround;
        const NetInput* latest = Client::getLatestInput();
        if (latest) PredictionRingOps::push(m_predictionRing, m_clientTick, *latest, s);
    }

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
    {
        const WorldSnapshot* snap = Client::getLatestSnapshot();
        if (snap) {
            u8 mySlot = activeNetSlot();
            u32 ackedTick = snap->lastProcessedInputTick[mySlot];
            // Only reconcile a fresh ACK we haven't already processed this frame.
            if (ackedTick != 0 && ackedTick != m_lastReconciledTick) {
                const PredictionEntry* e = PredictionRingOps::find(m_predictionRing, ackedTick);
                if (e) {
                    // Unpack the server's quantized position for our slot.
                    // SnapPlayer stores posX/posY/posZ as u16 packed via Quantize::packPos;
                    // unpack each component individually (no unpackPosVec3 helper exists).
                    const SnapPlayer& sp = snap->players[mySlot];
                    Vec3 serverPos = {
                        Quantize::unpackPos(sp.posX),
                        Quantize::unpackPos(sp.posY),
                        Quantize::unpackPos(sp.posZ)
                    };
                    Vec3 diff   = serverPos - e->state.position;
                    f32  distSq = diff.x * diff.x + diff.y * diff.y + diff.z * diff.z;
                    if (distSq > 0.01f) {  // > 10 cm (0.1 m) squared = 0.01 m²
                        LOG_INFO("net: prediction divergence at tick %u: %.2f m",
                                 ackedTick, sqrtf(distSq));

                        // M14: count every reconcile mismatch for the 1 Hz net-graph log.
                        m_divergenceCount++;

                        // M13: large divergence (>=10 m) triggers a 0.5s screen flash so
                        // the player knows a significant teleport correction happened.
                        if (distSq > 100.0f) {
                            m_localPlayer.screenFlashTimer = 0.5f;
                        }

                        // M4 smooth correction: accumulate the visible delta so the camera
                        // doesn't teleport. The sim position snaps immediately (server-
                        // authoritative for replay correctness); m_renderOffset decays each
                        // frame so the rendered eye position slides toward the corrected sim
                        // position over ~150 ms rather than popping.
                        Vec3 visibleDelta = m_localPlayer.position - serverPos;
                        RenderOffsetOps::accumulate(m_renderOffset, visibleDelta);
                        m_localPlayer.position = serverPos;
                    }
                }
                m_lastReconciledTick = ackedTick;
            }
        }
    }
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

// D1.1 — Client-side SV_KILL handler. v1: log the kill for diagnostics; future work
// can drive a kill-feed HUD, positional audio, or XP UI from this event.
void Engine::onKill(u8 killerSlot, u8 victimType, u16 victimIdx,
                    u8 weaponMeshId, u8 isCrit) {
    (void)weaponMeshId; // reserved for future kill-feed weapon icon
    LOG_INFO("net: kill event — killer=%u victimType=%u victimIdx=%u crit=%u",
             killerSlot, victimType, victimIdx, isCrit);
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
        (void)slot;
    } else {
        // Server rejected — roll back the predicted inventory add.
        if (slot >= 0) {
            Inventory::removeFromBackpack(
                s_engine->m_inventories[s_engine->m_localPlayerIndex],
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

