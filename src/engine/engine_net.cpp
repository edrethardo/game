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

    // Capture local input and push into server's input buffer
    WeaponState& ws = m_players[m_localPlayerIndex].weaponState;
    NetInput localInput = PlayerController::captureLocalInput(m_serverTick, ws.currentWeapon);
    Server::getInputBuffer(m_localPlayerIndex).push(localInput);

    // Process inputs for remote players only (host movement handled by gameUpdate)
    for (u32 i = 0; i < MAX_PLAYERS; i++) {
        if (i == m_localPlayerIndex) continue; // host moves in gameUpdate
        NetPlayer& np = m_players[i];
        if (!np.active) continue;
        const NetInput* input = Server::getInputBuffer(i).getLatest();
        if (input) {
            PlayerController::updateNetPlayerFromInput(np, *input, dt);
            np.lastProcessedInputTick = input->tick;
            if (input->weaponId < m_weaponDefCount)
                np.weaponState.currentWeapon = input->weaponId;
        }
    }

    // Collision for remote players only (host collision handled by gameUpdate)
    for (u32 i = 0; i < MAX_PLAYERS; i++) {
        if (i == m_localPlayerIndex) continue;
        NetPlayer& np = m_players[i];
        if (!np.active || np.noclip) continue;
        Player tempP;
        tempP.position = np.position;
        tempP.velocity = np.velocity;
        tempP.onGround = np.onGround;
        tempP.noclip = np.noclip;
        // Build obstacle list for net player collision (friendly NPCs non-blocking)
        CollisionObstacle npObs[MAX_ENTITIES];
        u32 npObsCount = 0;
        for (u32 ei = 0; ei < MAX_ENTITIES; ei++) {
            const Entity& e = m_entities.entities[ei];
            if (!(e.flags & ENT_ACTIVE) || (e.flags & ENT_DEAD)) continue;
            if (e.flags & ENT_FRIENDLY) continue;
            if (e.enemyType == EnemyType::PROP) continue;
            npObs[npObsCount++] = {e.position, e.halfExtents};
        }
        Collision::moveAndSlide(tempP, m_level.grid, dt, npObs, npObsCount);
        np.position = tempP.position;
        np.velocity = tempP.velocity;
        np.onGround = tempP.onGround;
    }

    // Remote player weapon fire + extended actions (server-authoritative)
    for (u32 i = 0; i < MAX_PLAYERS; i++) {
        if (!m_players[i].active) continue;
        if (i == m_localPlayerIndex) continue; // host handled by gameUpdate
        handleWeaponFireForPlayer(m_players[i], dt);

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
                        SkillSystem::tryActivate(ss, m_skillDefs, m_skillDefCount,
                                                  ep, fwd, m_players[i].yaw,
                                                  m_projectiles, m_entities, m_level.grid, m_localPlayer);
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
                        SkillSystem::tryActivate(ss, m_skillDefs, m_skillDefCount,
                                                  ep, fwd, m_players[i].yaw,
                                                  m_projectiles, m_entities, m_level.grid, m_localPlayer);
                    }
                }
            }

            // Class skill activation (right-click) — use remote player's class
            if (input->extFlags & INPUT_EX_SKILL) {
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
                        SkillSystem::tryActivate(tempSS, m_skillDefs, m_skillDefCount,
                                                  eyePos, fwd, m_players[i].yaw,
                                                  m_projectiles, m_entities, m_level.grid, m_localPlayer);
                    }
                }
            }
        }
    }

    // Sync NetPlayer → m_localPlayer so gameUpdate sees current server state
    // (gameUpdate's top-of-function sync handles this, but we also need forward vector)
}

// ---------------------------------------------------------------------------
// Server networking — post-gameplay: status ticks, snapshot broadcast
// ---------------------------------------------------------------------------
void Engine::serverNetPost(f32 dt) {
    // gameUpdate already synced m_localPlayer → NetPlayer at its end.
    // Sync EnemyAI damage back (AI ran inside gameUpdate targeting m_localPlayer)
    m_players[m_localPlayerIndex].health = m_localPlayer.health;
    m_players[m_localPlayerIndex].damageFlashTimer = m_localPlayer.damageFlashTimer;

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

    // Tick status effects + death detection for ALL players
    for (u32 pi = 0; pi < MAX_PLAYERS; pi++) {
        NetPlayer& np = m_players[pi];
        if (!np.active || np.isDead) continue;

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

        if (np.health <= 0.0f) {
            np.health = 0.0f;
            np.isDead = true;
            LOG_INFO("Player %u died", pi);
            if (pi == m_localPlayerIndex) {
                m_playerDead[0] = true; // don't freeze the server
            }
        }
    }

    // Respawn handling
    for (u32 pi = 0; pi < MAX_PLAYERS; pi++) {
        NetPlayer& np = m_players[pi];
        if (!np.active || !np.isDead) continue;
        const NetInput* input = Server::getInputBuffer(static_cast<u8>(pi)).getLatest();
        if (input && (input->extFlags & INPUT_EX_RESPAWN)) {
            np.health = np.maxHealth;
            np.position = np.spawnPosition;
            np.velocity = {0, 0, 0};
            np.invulnTimer = 1.5f;
            np.isDead = false;
            LOG_INFO("Player %u respawned", pi);
        }
    }

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
    }

    // Broadcast snapshot every TICKS_PER_SNAP ticks
    if (m_serverTick % TICKS_PER_SNAP == 0) {
        Server::sendSnapshot(m_serverTick, m_players, m_entities, m_projectiles);
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

    // Capture and send input to server
    WeaponState& ws = m_players[m_localPlayerIndex].weaponState;
    Client::captureAndSendInput(m_serverTick, ws.currentWeapon);

    // Apply local prediction (movement + collision)
    const NetInput* input = Client::getLatestInput();
    if (input) {
        NetPlayer& np = m_players[m_localPlayerIndex];
        PlayerController::updateNetPlayerFromInput(np, *input, dt);

        Player tempP;
        tempP.position = np.position;
        tempP.velocity = np.velocity;
        tempP.onGround = np.onGround;
        tempP.noclip = np.noclip;
        // Build obstacle list for client prediction collision (friendly NPCs non-blocking)
        CollisionObstacle clObs[MAX_ENTITIES];
        u32 clObsCount = 0;
        for (u32 ei = 0; ei < MAX_ENTITIES; ei++) {
            const Entity& e = m_entities.entities[ei];
            if (!(e.flags & ENT_ACTIVE) || (e.flags & ENT_DEAD)) continue;
            if (e.flags & ENT_FRIENDLY) continue;
            if (e.enemyType == EnemyType::PROP) continue;
            clObs[clObsCount++] = {e.position, e.halfExtents};
        }
        Collision::moveAndSlide(tempP, m_level.grid, dt, clObs, clObsCount);
        np.position = tempP.position;
        np.velocity = tempP.velocity;
        np.onGround = tempP.onGround;

        Client::storePrediction(*input, np);
    }

    // Reconcile with server
    Client::reconcile(m_players[m_localPlayerIndex], m_level.grid, dt);

    // Sync NetPlayer → m_localPlayer so gameUpdate sees predicted state
    // (gameUpdate's top-of-function sync handles this)
}

// ---------------------------------------------------------------------------
// Client networking — post-gameplay: interpolate remote state
// ---------------------------------------------------------------------------
void Engine::clientNetPost(f32 dt) {
    (void)dt;
    // gameUpdate already synced m_localPlayer → NetPlayer at its end.

    // Interpolate remote players, entities, and projectiles from server snapshots
    Client::interpolateRemotePlayers(m_localPlayerIndex,
        m_renderInterp.playerPositions, m_renderInterp.playerYaws, m_renderInterp.playerPitches,
        m_renderInterp.playerActive, m_renderInterp.playerHealth, m_renderInterp.playerMaxHealth,
        m_renderInterp.playerAnimFlags);
    Client::interpolateEntities(m_renderInterp.entities);
    Client::interpolateProjectiles(m_renderInterp.projectiles);
}

