// Engine callback wiring — registers all Combat/SkillSystem/ProjectileSystem/Inventory
// event callbacks that require access to the live engine state via s_engine.
// Called by Engine::init() after initAssets() so mesh IDs and defs are valid.

#define SDL_MAIN_HANDLED
#include <SDL.h>
#include "audio/audio.h"

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
#include "renderer/projectile_renderer.h"
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
#include "game/boss_def.h"
#include "game/boss_ai.h"
#include "game/boss_loader.h"
#include "game/enemy_loader.h"
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
extern Engine* s_engine;
extern FrameAllocator s_frameAllocator;
extern bool s_firstKillDropGiven;

// Spawn the projectile AoE splash VFX at an impact point: a floor-snapped fire-FX ring,
// an explosion particle burst, and a small camera shake. Shared by the host's splash
// callback and the CLIENT's PROJECTILE_SPLASH event handler so both render identically —
// the client can't fire the callback itself (its ProjectileSystem::update is gated off).
void Engine::spawnSplashFX(Vec3 position, f32 radius) {
    // Snap the fire effect to floor level so it doesn't render underground.
    u32 gx, gz;
    Vec3 fxPos = position;
    if (LevelGridSystem::worldToGrid(m_level.grid, position, gx, gz) &&
        !LevelGridSystem::isSolid(m_level.grid, gx, gz)) {
        fxPos.y = LevelGridSystem::getFloorHeight(m_level.grid, gx, gz) + 0.1f;
    }
    for (u32 i = 0; i < Engine::MAX_FIRE_FX; i++) {
        if (!m_fx.fireFX[i].active) {
            m_fx.fireFX[i] = {fxPos, radius, 1.0f, true};
            break;
        }
    }
    // Fiery burst at the impact point.
    ParticleSystem::spawnExplosion(m_particles, position, radius);
    m_camera.shake.trigger(0.06f, 0.3f);
}

void Engine::initCallbacks() {
    // Wire particle pool and screen shake into combat, skill, and projectile systems
    Combat::setFXTargets(&m_particles, &m_camera.shake);
    SkillSystem::setFXTargets(&m_particles, &m_camera.shake);
    extern void ProjectileSystem_setTrailPool(ParticlePool* pool);
    ProjectileSystem_setTrailPool(&m_particles);

    // Apply energy_flat affix bonus to SkillState.maxEnergy on equip/unequip
    Inventory::setStatsChangedCallback([](PlayerInventory& inv) {
        if (!s_engine) return;
        SkillState& ss = s_engine->m_skillStates[s_engine->m_localPlayerIndex];
        f32 baseEnergy = kClassDefs[static_cast<u32>(s_engine->m_playerClass)].baseEnergy;
        f32 oldMax = ss.maxEnergy;
        ss.maxEnergy = baseEnergy + inv.bonusEnergyFlat;
        // Preserve energy percentage so equipping doesn't drain/fill
        if (oldMax > 0.0f) ss.energy = ss.energy * (ss.maxEnergy / oldMax);
        if (ss.energy > ss.maxEnergy) ss.energy = ss.maxEnergy;
    });

    // Thread isCrit and isKill into the damage number renderer.
    // Killing blows reuse the crit visual style (larger, brighter number).
    Combat::setDamageNumberCallback([](Vec3 pos, f32 amount, bool isCrit, bool isKill) {
        if (!s_engine) return;
        s_engine->spawnDamageNumber(pos, amount, /*isHeal*/false, isCrit || isKill);
    });

    // Death callback orchestrator — delegates each phase to a named Engine method.
    // Preamble (squad/speech/passives/bomber) always runs; the two bool helpers
    // mirror the original early-returns: first-kill skips boss+normal+ring, boss
    // skips normal+ring.  Control flow is identical to the original monolithic lambda.
    Combat::setDeathCallback([](EntityPool& pool, u16 entityIndex, Vec3 position) {
        if (!s_engine) return;
        s_engine->handleDeathPreamble(pool, entityIndex, position);
        // Loot is SERVER-AUTHORITATIVE (N5): only the host (SERVER) and offline/split
        // (NONE) roll and spawn drops. A remote CLIENT must NOT roll its own loot — it
        // receives the authoritative world-item list from the server snapshot, which is
        // mirrored into m_worldItems each frame. Clients still run the preamble above
        // (squad/speech/passives are cosmetic-local) but skip every drop phase here.
        if (s_engine->m_netRole == NetRole::CLIENT) return;
        if (s_engine->handleFirstKillDrop(pool, entityIndex, position)) return;
        if (s_engine->handleBossLootDrop(pool, entityIndex, position)) return;
        s_engine->handleNormalLootDrop(pool, entityIndex, position);
        s_engine->handleOnKillRingPassives(pool, entityIndex, position);
    });

    // D1.1 — Kill broadcast: SERVER emits SV_KILL reliably to all connected clients so
    // they can drive kill-feed / sound FX. victimType is always 0 (entity) from this path.
    // Player kills (victimType=1) come from applyDamageToPlayer which currently has no
    // kill hook — that's a follow-up; singleplayer/offline runs skip this (no clients).
    Combat::setOnKill([](u8 killerSlot, u8 victimType, u16 victimIdx,
                         u8 weaponMeshId, u8 isCrit) {
        if (!s_engine) return;
        if (s_engine->m_netRole != NetRole::SERVER) return; // host only; SP/split fires the event locally
        // Build the 6-byte SV_KILL payload after the 4-byte header.
        u8 buf[sizeof(PacketHeader) + 6];
        PacketHeader* hdr = reinterpret_cast<PacketHeader*>(buf);
        hdr->type  = NetPacketType::SV_KILL;
        hdr->flags = 0;
        hdr->seq   = 0;
        u32 off = sizeof(PacketHeader);
        buf[off++] = killerSlot;
        buf[off++] = victimType;
        std::memcpy(buf + off, &victimIdx, 2); off += 2;
        buf[off++] = weaponMeshId;
        buf[off++] = isCrit;
        Net::broadcastReliable(buf, off);
    });

    // Splash effect callback — spawns fire VFX at impact point. Fires only inside
    // ProjectileSystem::update, which is gated off on CLIENT (N4 ghost-sim removal), so the
    // host additionally broadcasts a PROJECTILE_SPLASH event (below) for guests to replay.
    ProjectileSystem::setSplashCallback([](Vec3 position, f32 radius) {
        if (!s_engine) return;
        s_engine->spawnSplashFX(position, radius); // local FX (host + singleplayer)

        // SERVER: replicate to clients — their local sim never fires this callback. Send the
        // RAW (pre-floor-snap) position so the client re-runs the identical snap in
        // spawnSplashFX. Mirrors the DAMAGE_NUMBER broadcast idiom (engine_update.cpp).
        if (s_engine->m_netRole == NetRole::SERVER) {
            u8 buf[sizeof(PacketHeader) + 17]; // hdr(4) + eventType(1) + pos(12) + radius(4)
            PacketHeader* hdr = reinterpret_cast<PacketHeader*>(buf);
            hdr->type  = NetPacketType::SV_EVENT;
            hdr->flags = 0;
            hdr->seq   = 0;
            u32 off = sizeof(PacketHeader);
            buf[off++] = static_cast<u8>(NetEventType::PROJECTILE_SPLASH);
            std::memcpy(buf + off, &position.x, 4); off += 4;
            std::memcpy(buf + off, &position.y, 4); off += 4;
            std::memcpy(buf + off, &position.z, 4); off += 4;
            std::memcpy(buf + off, &radius,     4); off += 4;
            Net::broadcastReliable(buf, off);
        }
    });

    // Projectile hit callback — triggers weapon on-hit procs for projectile weapons
    ProjectileSystem::setHitCallback([](Vec3 position, EntityHandle target) {
        if (!s_engine) return;
        if (s_engine->m_weaponProc == SkillId::NONE) return;

        u32 procRoll = static_cast<u32>(std::rand()) % 100;
        u32 procChance = 20;
        if (s_engine->m_weaponProc == SkillId::VOID_ZONE)       procChance = 5;
        if (s_engine->m_weaponProc == SkillId::FROZEN_ORB)      procChance = 15;
        if (s_engine->m_weaponProc == SkillId::CHAIN_LIGHTNING)  procChance = 25;
        if (s_engine->m_weaponProc == SkillId::METEOR_STRIKE)    procChance = 10;
        if (s_engine->m_weaponProc == SkillId::BLOOD_NOVA)       procChance = 20;
        if (s_engine->m_weaponProc == SkillId::SHADOW_RICOCHET)  procChance = 30;

        LOG_INFO("Projectile hit: weaponProc=%u, roll=%u/%u",
                 static_cast<u32>(s_engine->m_weaponProc), procRoll, procChance);

        if (procRoll >= procChance) return;

        const SkillDef* sd = SkillSystem::findSkillDef(s_engine->m_skillDefs,
                                                         s_engine->m_skillDefCount,
                                                         s_engine->m_weaponProc);
        // No def = this legendary isn't a weapon proc (it's a passive/on-kill skill
        // like Phase Strike, or the Throwaway throw mechanic). Nothing to fire here.
        if (!sd) return;

        LOG_INFO("WEAPON PROC triggered! skill=%u at (%.1f, %.1f, %.1f)",
                 static_cast<u32>(s_engine->m_weaponProc), position.x, position.y, position.z);

        switch (s_engine->m_weaponProc) {
            case SkillId::VOID_ZONE: {
                // AoE void zone — hits all enemies in radius with flat + 60% missing HP
                EntityHandle aoeHits[MAX_ENTITIES];
                f32 aoeDists[MAX_ENTITIES];
                u32 aoeCount = CombatQuery::queryConeSorted(
                    s_engine->m_entities, position, {0,-1,0}, -1.0f, sd->radius,
                    aoeHits, aoeDists, MAX_ENTITIES);
                for (u32 h = 0; h < aoeCount; h++) {
                    Entity* ve = handleGet(s_engine->m_entities, aoeHits[h]);
                    if (!ve || (ve->flags & ENT_DEAD) || (ve->flags & ENT_FRIENDLY)) continue;
                    f32 missingHp = ve->maxHealth - ve->health;
                    f32 voidDmg = sd->damage + missingHp * 0.6f;
                    Combat::applyDamage(s_engine->m_entities, aoeHits[h], voidDmg);
                }
                LOG_INFO("  VOID ZONE AoE: hit %u enemies", aoeCount);
                // Large dark-purple nova + scorch zone for visibility
                for (u32 ni = 0; ni < Engine::MAX_NOVA_FX; ni++) {
                    if (!s_engine->m_fx.novaFX[ni].active) {
                        s_engine->m_fx.novaFX[ni] = {position, 3.0f, 1.2f, true, {0.4f, 0.1f, 0.6f}};
                        break;
                    }
                }
                // Also spawn a dark scorch zone on the ground
                for (u32 si = 0; si < Engine::MAX_SCORCH; si++) {
                    if (!s_engine->m_fx.scorchZones[si].active) {
                        s_engine->m_fx.scorchZones[si] = {position, 3.0f, 1.5f, 0.0f, true};
                        break;
                    }
                }
            } break;
            case SkillId::FROZEN_ORB: {
                Vec3 dir = s_engine->m_localPlayer.forward;
                u16 orbIdx = ProjectileSystem::spawn(s_engine->m_projectiles, position, dir,
                    sd->projectileSpeed, sd->damage, sd->radius, sd->duration, true);
                if (orbIdx != 0xFFFF) s_engine->m_projectiles.projectiles[orbIdx].projFlags = PROJ_ORB;
            } break;
            case SkillId::CHAIN_LIGHTNING: {
                SkillState tempSS;
                tempSS.activeSkill = SkillId::CHAIN_LIGHTNING;
                tempSS.energy = 999.0f; tempSS.maxEnergy = 999.0f;
                // R17: tempSS.lastActivationTick=0 always-pass; currentTick threaded for consistency.
                SkillSystem::tryActivate(tempSS, s_engine->m_skillDefs, s_engine->m_skillDefCount,
                    position, s_engine->m_localPlayer.forward, 0,
                    s_engine->m_projectiles, s_engine->m_entities,
                    s_engine->m_level.grid, s_engine->m_localPlayer,
                    s_engine->currentLocalTick());
            } break;
            case SkillId::BLOOD_NOVA: {
                EntityHandle hits[MAX_ENTITIES];
                f32 dists[MAX_ENTITIES];
                u32 hitCount = CombatQuery::queryConeSorted(
                    s_engine->m_entities, position, {0,0,-1}, -1.0f, sd->radius,
                    hits, dists, MAX_ENTITIES);
                for (u32 h = 0; h < hitCount; h++)
                    Combat::applyDamage(s_engine->m_entities, hits[h], sd->damage * 0.5f);
                for (u32 ni = 0; ni < Engine::MAX_NOVA_FX; ni++) {
                    if (!s_engine->m_fx.novaFX[ni].active) {
                        s_engine->m_fx.novaFX[ni] = {position, sd->radius, 0.6f, true, {1.0f, 0.15f, 0.1f}};
                        break;
                    }
                }
            } break;
            case SkillId::METEOR_STRIKE: {
                extern PendingMeteor s_meteors[MAX_PENDING_METEORS];
                for (u32 mi = 0; mi < MAX_PENDING_METEORS; mi++) {
                    if (!s_meteors[mi].active) {
                        s_meteors[mi] = {position, sd->damage, sd->radius, sd->delay, true};
                        break;
                    }
                }
            } break;
            case SkillId::SHADOW_RICOCHET: {
                // Find 2 nearest enemies (excluding the one we just hit) and fire
                // shadow bolts at them. Each bolt is a normal player projectile so
                // it can re-trigger the proc on hit (10% chance → natural decay).
                EntityHandle nearby[8];
                f32 nearDists[8];
                u32 found = CombatQuery::queryConeSorted(
                    s_engine->m_entities, position, {0,-1,0}, -1.0f, 12.0f,
                    nearby, nearDists, 8);
                u32 spawned = 0;
                for (u32 h = 0; h < found && spawned < 2; h++) {
                    if (nearby[h].index == target.index) continue; // skip primary target
                    Entity* ne = handleGet(s_engine->m_entities, nearby[h]);
                    if (!ne || (ne->flags & ENT_DEAD) || (ne->flags & ENT_FRIENDLY)) continue;
                    Vec3 toEnemy = ne->position - position;
                    f32 dist = length(toEnemy);
                    if (dist < 0.1f) continue;
                    Vec3 dir = toEnemy * (1.0f / dist);
                    // Shadow bolt: 60% of weapon damage, fast, small radius
                    f32 boltDmg = sd->damage * 0.6f;
                    u16 idx = ProjectileSystem::spawn(s_engine->m_projectiles, position, dir,
                        20.0f, boltDmg, 0.1f, 2.0f, true);
                    if (idx != 0xFFFF) {
                        s_engine->m_projectiles.projectiles[idx].projFlags = PROJ_VOID;
                    }
                    spawned++;
                }
                LOG_INFO("  SHADOW RICOCHET: spawned %u bolts", spawned);
            } break;
            default: break;
        }
    });

    // Floating damage numbers for projectile hits
    ProjectileSystem::setDamageNumberCallback([](Vec3 position, f32 damage) {
        if (!s_engine) return;
        s_engine->spawnDamageNumber(position, damage);
    });

    // M10.3 — Enemy projectile hit a player. On SERVER, find which remote slot owns the
    // victim Player view and emit SV_DAMAGE_TO_ME so the client can ack its PendingDamageRing.
    // Key encoding mirrors the client's PendingDamageRingOps::record key:
    //   (ownerSlot << 24) | (clientTick & 0xFFFFFF)
    // Only sent to remote (non-host) clients; the host (slot 0) runs locally.
    ProjectileSystem::setPlayerHitCallback([](u8 ownerSlot, u32 clientTick, f32 damage, Player* victim) {
        if (!s_engine) return;
        if (s_engine->m_netRole != NetRole::SERVER) return;
        // Identify which remote NetPlayer slot the victim view belongs to by matching position.
        // Remote views are ephemeral copies — pointer comparison won't work, so match by
        // position proximity (sub-millimeter threshold; views are freshly built each tick).
        u8 victimSlot = 0xFF;
        for (u32 i = 1; i < MAX_PLAYERS; i++) { // slot 0 = host, not a remote view
            if (!s_engine->m_players[i].active) continue;
            Vec3 d = s_engine->m_players[i].position - victim->position;
            f32 distSq = d.x*d.x + d.y*d.y + d.z*d.z;
            if (distSq < 0.001f) { victimSlot = static_cast<u8>(i); break; }
        }
        if (victimSlot == 0xFF) return; // host or unmatched — no send needed
        u32 key = (static_cast<u32>(ownerSlot) << 24) | (clientTick & 0xFFFFFFu);
        u8 svBuf[sizeof(PacketHeader) + 10]; // header(4) + key(4) + damage(4) + reserved(2)
        PacketHeader* hdr = reinterpret_cast<PacketHeader*>(svBuf);
        hdr->type  = NetPacketType::SV_DAMAGE_TO_ME;
        hdr->flags = 0;
        hdr->seq   = 0;
        u32 off = sizeof(PacketHeader);
        std::memcpy(svBuf + off, &key,    4); off += 4;
        std::memcpy(svBuf + off, &damage, 4); off += 4;
        u16 reserved = 0;
        std::memcpy(svBuf + off, &reserved, 2); off += 2;
        Net::sendReliable(victimSlot, svBuf, off);
    });

    // Perfect block callback — legendary shield stun bash
    Combat::setPerfectBlockCallback([](Player& player) {
        if (!s_engine) return;

        // Check if offhand is a legendary shield
        const ItemInstance& shield = s_engine->m_inventories[s_engine->m_localPlayerIndex].equipped[static_cast<u32>(ItemSlot::OFFHAND)];
        bool hasLegendaryShield = !isItemEmpty(shield) && shield.rarity == Rarity::LEGENDARY;

        if (hasLegendaryShield) {
            for (u32 a = 0; a < s_engine->m_entities.activeCount; a++) {
                u32 idx = s_engine->m_entities.activeList[a];
                Entity& ent = s_engine->m_entities.entities[idx];
                if (ent.flags & ENT_DEAD) continue;
                if (ent.flags & ENT_FRIENDLY) continue;
                if (ent.enemyType == EnemyType::PROP) continue;
                f32 dist = length(ent.position - player.position);
                if (dist < 3.0f) {
                    ent.freezeTimer = 1.0f;
                }
            }
            // Visual nova feedback
            for (u32 ni = 0; ni < MAX_NOVA_FX; ni++) {
                if (!s_engine->m_fx.novaFX[ni].active) {
                    s_engine->m_fx.novaFX[ni] = {player.position, 3.0f, 0.4f, true, Vec3{0.8f, 0.8f, 1.0f}};
                    break;
                }
            }
        }
    });

    // Dodge-through callback: fires an automatic riposte counter-hit when the player
    // dodges through an incoming attack, and grants an adrenaline stack if unlocked.
    Combat::setDodgeThroughCallback([](u16 attackerIdx, Vec3 attackerPos) {
        if (!s_engine) return;
        Player& player = s_engine->m_localPlayer;

        // 1. Instant riposte: 50% weapon damage counter-hit on the attacker.
        // m_weaponDefs[0] is the fallback if no item is equipped (unarmed).
        WeaponDef effectiveWeapon = Inventory::getEffectiveWeapon(
            s_engine->m_inventories[s_engine->m_localPlayerIndex],
            s_engine->m_itemDefs, s_engine->m_weaponDefs[0]);
        f32 riposteDmg = effectiveWeapon.damage * 0.5f;

        if (attackerIdx < MAX_ENTITIES &&
            (s_engine->m_entities.entities[attackerIdx].flags & ENT_ACTIVE)) {
            EntityHandle h;
            h.index      = attackerIdx;
            h.generation = s_engine->m_entities.entities[attackerIdx].generation;
            Combat::applyDamage(s_engine->m_entities, h, riposteDmg, &player.position);
            Combat::spawnDamageNumber(attackerPos, riposteDmg);
        }

        // 2. Adrenaline surge stacks (only if skill 3 is unlocked)
        if (player.adrenalineUnlocked) {
            DodgeState& ds = player.dodgeState;
            if (ds.counterStacks < player.adrenalineMaxStacks) {
                ds.counterTimers[ds.counterStacks] = 4.0f;
                ds.counterStacks++;
            } else {
                // At cap — refresh the oldest (shortest remaining) stack rather than
                // losing one. Scan only the active stacks (cap may be 3, not the array's 5).
                f32 minT  = ds.counterTimers[0];
                u8  minIdx = 0;
                for (u8 i = 1; i < ds.counterStacks; i++) {
                    if (ds.counterTimers[i] < minT) { minT = ds.counterTimers[i]; minIdx = i; }
                }
                ds.counterTimers[minIdx] = 4.0f;
            }
        }

        // 3. Exploit Weakness: dodge-through a marked enemy grants +5% speed stack (3s)
        if (attackerIdx < MAX_ENTITIES) {
            Entity& attacker = s_engine->m_entities.entities[attackerIdx];
            if (attacker.markPreyTimer > 0.0f && player.markSpeedStacks < 20) {
                player.markSpeedTimers[player.markSpeedStacks] = 3.0f;
                player.markSpeedStacks++;
            }
        }

        // 4. Death's Dance AoE slash: if the ultimate is active, radiate a full-circle
        //    slash to all nearby enemies on every dodge-through
        if (player.deathsDanceTimer > 0.0f) {
            WeaponDef ew = Inventory::getEffectiveWeapon(
                s_engine->m_inventories[s_engine->m_localPlayerIndex],
                s_engine->m_itemDefs, s_engine->m_weaponDefs[0]);
            f32 slashDmg = ew.damage;
            Vec3 eyePos  = player.position + Vec3{0, player.eyeHeight, 0};

            EntityHandle hits[MAX_ENTITIES];
            f32 dists[MAX_ENTITIES];
            // cosCone = -1.0f → cos(180°) → query everything in a sphere of radius 3 m
            u32 hitCount = CombatQuery::queryConeSorted(
                s_engine->m_entities, eyePos, player.forward, -1.0f, 3.0f,
                hits, dists, MAX_ENTITIES);
            f32 totalDmg = 0.0f;
            for (u32 k = 0; k < hitCount; k++) {
                Entity* he = handleGet(s_engine->m_entities, hits[k]);
                if (he && !(he->flags & ENT_DEAD)) {
                    // Accumulate actual damage dealt (capped by remaining HP before hit)
                    f32 hpBefore = he->health;
                    Combat::applyDamage(s_engine->m_entities, hits[k], slashDmg, &eyePos);
                    totalDmg += hpBefore - (he->health < 0.0f ? 0.0f : he->health);
                }
            }
            // Death's Dance upgrade: heal 10% of AoE damage dealt (floor >= 40)
            if (s_engine->m_level.currentFloor >= 40 && totalDmg > 0.0f) {
                player.health += totalDmg * 0.1f;
                if (player.health > player.maxHealth) player.health = player.maxHealth;
            }
        }

        // 4. Subtle screen-shake for tactile riposte feedback
        if (s_engine->m_camera.shake.intensity < 0.03f) {
            s_engine->m_camera.shake.trigger(0.03f, 0.2f);
        }
    });

    // Drone/turret spawn callback — skill.cpp delegates spawning here so we
    // have direct access to the mesh registry.  type: 0=combat drone, 1=swarm, 2=turret
    SkillSystem::setDroneSpawnCallback([](Vec3 position, u8 type) {
        if (!s_engine) return;
        EntityPool& pool = s_engine->m_entities;

        f32 floorMult = 1.0f + (s_engine->m_level.currentFloor + s_engine->m_difficulty * 50 - 1) * 0.06f;

        if (type == 0) {
            // Spider drone — melee ground unit. Swarm Overlord spawns many of these.
            EntityHandle h = EntitySystem::spawn(pool, position,
                {0.3f, 0.2f, 0.3f}, false, 30.0f * floorMult, 6.0f * floorMult,
                12.0f, 4.0f, 0.5f, 8.0f);
            Entity* e = handleGet(pool, h);
            if (e) {
                e->ownerLocalPlayer = s_engine->m_localPlayerIndex; // split-screen lane (host-only path)
                e->ownerNetSlot     = SkillSystem::getCastingPlayer(); // N4: remote-cast minions tether to the caster
                e->flags        |= ENT_FRIENDLY;
                e->enemyType     = EnemyType::SPIDER;
                e->meshId        = s_engine->m_meshIdSpider;
                e->materialId    = 49; // prop_iron
                e->npcWeaponType = WeaponType::MELEE;
                e->aiState       = AIState::IDLE;
                e->baseMoveSpeed      = e->moveSpeed;
                e->baseAttackCooldown = e->attackCooldown;
            }
        } else if (type == 1) {
            // Bat drone — flying melee swarm unit. Fast, closes distance to attack.
            EntityHandle h = EntitySystem::spawn(pool, position,
                {0.15f, 0.1f, 0.15f}, true, 20.0f * floorMult, 7.0f * floorMult,
                12.0f, 3.0f, 0.5f, 6.0f);
            Entity* e = handleGet(pool, h);
            if (e) {
                e->ownerLocalPlayer = s_engine->m_localPlayerIndex; // split-screen lane (host-only path)
                e->ownerNetSlot     = SkillSystem::getCastingPlayer(); // N4: remote-cast minions tether to the caster
                e->flags        |= ENT_FRIENDLY | ENT_FLYING;
                e->enemyType     = EnemyType::GENERIC;
                e->meshId        = s_engine->m_meshIdBat;
                e->materialId    = 49; // prop_iron
                e->npcWeaponType = WeaponType::MELEE;
                e->aiState       = AIState::IDLE;
                e->baseMoveSpeed      = e->moveSpeed;
                e->baseAttackCooldown = e->attackCooldown;
            }
        } else if (type == 3) {
            // Swarm Queen — large tanky spider that auto-spawns minis every 2s for 20s
            EntityHandle h = EntitySystem::spawn(pool, position,
                {0.5f, 0.4f, 0.5f}, false, 200.0f * floorMult, 8.0f * floorMult,
                15.0f, 3.0f, 1.0f, 10.0f);
            Entity* e = handleGet(pool, h);
            if (e) {
                e->ownerLocalPlayer = s_engine->m_localPlayerIndex; // split-screen lane (host-only path)
                e->ownerNetSlot     = SkillSystem::getCastingPlayer(); // N4: remote-cast minions tether to the caster
                e->flags        |= ENT_FRIENDLY;
                e->enemyType     = EnemyType::SPIDER;
                e->meshId        = s_engine->m_meshIdSpider;
                e->materialId    = MaterialSystem::getIdByName("gold_trim");
                if (e->materialId == 0) e->materialId = 49; // fallback
                e->npcWeaponType = WeaponType::MELEE;
                e->aiState       = AIState::IDLE;
                e->queenLifeTimer  = 20.0f;
                e->queenSpawnTimer = 2.0f;
                e->baseMoveSpeed      = e->moveSpeed;
                e->baseAttackCooldown = e->attackCooldown;
                // Scale up visually (halfExtents already larger)
            }
        } else if (type == 2) {
            // Mobile turret bot — armored body on tank treads, follows player when idle.
            // HP scales with floor depth so turrets stay relevant in later tiers.
            f32 baseHp = 160.0f;
            f32 floorMult = 1.0f + (s_engine->m_level.currentFloor - 1) * 0.06f;
            EntityHandle h = EntitySystem::spawn(pool, position,
                {0.2f, 0.3f, 0.2f}, false, baseHp * floorMult, 3.0f, 15.0f, 10.0f, 1.5f, 12.0f);
            Entity* e = handleGet(pool, h);
            if (e) {
                e->ownerLocalPlayer = s_engine->m_localPlayerIndex; // split-screen lane (host-only path)
                e->ownerNetSlot     = SkillSystem::getCastingPlayer(); // N4: remote-cast minions tether to the caster
                e->flags        |= ENT_FRIENDLY;
                e->enemyType     = EnemyType::GENERIC;
                e->meshId        = s_engine->findMeshByName("turret");
                e->materialId    = MaterialSystem::getIdByName("turret_skin");
                e->npcWeaponType = WeaponType::PROJECTILE;
                e->npcProjectileSpeed  = 30.0f;
                e->npcProjectileRadius = 0.06f;
                e->aiState       = AIState::IDLE;
                e->baseMoveSpeed      = e->moveSpeed;
                e->baseAttackCooldown = e->attackCooldown;
                // Spark burst on deploy — visual feedback
                ParticleSystem::spawnSparks(s_engine->m_particles, position, {0, 1, 0}, 8);
            }
        }
    });
    // Share the same drone spawn path with EnemyAI for Swarm Queen auto-spawning.
    // The SkillSystem callback is a captureless lambda stored as function pointer —
    // EnemyAI needs the same capability so the queen can spawn mini drones.
    EnemyAI::setDroneSpawnCallback([](Vec3 pos, u8 type) {
        if (!s_engine) return;
        // Re-invoke the skill system's drone spawn (same lambda body as above)
        // by calling the SkillSystem callback which is stored as a static.
        // Simplest: just inline the spawn for type 0 (spider mini drone)
        EntityPool& pool = s_engine->m_entities;
        f32 fm = 1.0f + (s_engine->m_level.currentFloor + s_engine->m_difficulty * 50 - 1) * 0.06f;
        EntityHandle h = EntitySystem::spawn(pool, pos,
            {0.3f, 0.2f, 0.3f}, false, 30.0f * fm, 6.0f * fm,
            12.0f, 4.0f, 0.5f, 8.0f);
        Entity* e = handleGet(pool, h);
        if (e) {
            e->flags        |= ENT_FRIENDLY;
            e->enemyType     = EnemyType::SPIDER;
            e->meshId        = s_engine->m_meshIdSpider;
            e->materialId    = 49;
            e->npcWeaponType = WeaponType::MELEE;
            e->aiState       = AIState::IDLE;
            e->baseMoveSpeed      = e->moveSpeed;
            e->baseAttackCooldown = e->attackCooldown;
        }
    });
}
