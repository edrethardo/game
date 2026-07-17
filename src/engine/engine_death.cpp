// engine_death.cpp — Engine death-event handler implementations.
// These are private Engine methods extracted from the Combat death callback in
// engine_init_callbacks.cpp to break up the monolithic lambda.  Each method
// corresponds to one logical phase of the death event and is called in order
// by the lambda; the bool-returning variants signal that they fully handled the
// loot path so the lambda can early-return without running the later phases.
//
// Control-flow contract (mirrors the original lambda exactly):
//   handleDeathPreamble   — always runs (squad, speech, passives, bomber)
//   handleFirstKillDrop   — runs kill tracking then checks first-kill guarantee;
//                           returns true → lambda returns (skip boss/normal/ring)
//   handleBossLootDrop    — returns true → lambda returns (skip normal/ring)
//   handleNormalLootDrop  — void, no early exit
//   handleOnKillRingPassives — void, always last

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
#include "game/champion.h"  // champion death novas (MOLTEN / FROZEN)
#include "game/floor_event.h"  // Goblin:: tunables (death payout)
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
extern u16  s_sourceShards;   // secret superboss key — session-only set of collected shards
extern bool s_engineSlain;    // secret superboss — Engine defeated this session (victory variant)

// D1.3 — Broadcast SV_LOOT_SPAWN to all connected clients when a world item is spawned
// server-side. Called from each WorldItemSystem::spawn site in the loot-drop paths.
// The snapshot already mirrors the item visually; this reliable event lets clients react
// immediately (kill-feed, minimap pins) before the next snapshot window.
// No-op in singleplayer / split-screen (no remote clients to notify).
static void broadcastLootSpawn(const WorldItemPool& pool, u32 uid, Vec3 pos, u16 defId) {
    // Net::getRole() is the public API for role query; avoids touching engine private state.
    if (!s_engine || Net::getRole() != NetRole::SERVER) return;
    // 12-byte payload: u32 uid + u16 posXQ + u16 posYQ + u16 posZQ + u16 itemDefId.
    u8 buf[sizeof(PacketHeader) + 12];
    PacketHeader* hdr = reinterpret_cast<PacketHeader*>(buf);
    hdr->type  = NetPacketType::SV_LOOT_SPAWN;
    hdr->flags = 0;
    hdr->seq   = 0;
    u32 off = sizeof(PacketHeader);
    std::memcpy(buf + off, &uid, 4); off += 4;
    u16 posXQ = Quantize::packPos(pos.x);
    u16 posYQ = Quantize::packPos(pos.y);
    u16 posZQ = Quantize::packPos(pos.z);
    std::memcpy(buf + off, &posXQ, 2); off += 2;
    std::memcpy(buf + off, &posYQ, 2); off += 2;
    std::memcpy(buf + off, &posZQ, 2); off += 2;
    std::memcpy(buf + off, &defId, 2); off += 2;
    Net::broadcastReliable(buf, off);
    (void)pool; // pool param reserved for future per-slot targeting
}

// Runs unconditionally for every death, before any loot logic.
// Handles: squad reassignment, friendly NPC speech, Shadow Dance extension,
// Wanderer mark-prey passives, Mark Prey arrow chain, and Bomber death explosion.
void Engine::handleDeathPreamble(EntityPool& pool, u16 idx, Vec3 pos) {
    // The Dungeon Engine secret superboss is slain → the run's true ending. This handler runs on
    // the authoritative host/SP (enemy deaths aren't simulated on a client). It used to snap the
    // HOST's m_gameState straight to VICTORY — a purely local flip that was never broadcast, so
    // serverUpdate stopped, snapshots stopped, and every co-op client hung on a frozen world.
    // Now the kill spawns the EXIT PORTAL instead (replicated via SV_EVENT::EXIT_PORTAL): play
    // continues — loot the corpse — and whoever steps into the portal starts the shared credits
    // sequence for everyone (beginCreditsSequence broadcast).
    if (pool.entities[idx].isEngine) {
        s_engineSlain = true;    // steers the credits/victory text to the secret ending
        AudioSystem::stopMusic();   // dramatic silence over the corpse; credits restart nothing
        addChatMessage("\?\?\?", "halt. the curse... ends.", Vec3{0.62f, 0.30f, 0.95f});
        // South of the corpse (toward the chamber entrance) so it never overlaps the loot pile.
        spawnExitPortal(pos + Vec3{0.0f, 0.0f, 4.0f});
    }

    // Remove from squad so roles are reassigned before next AI tick
    SquadSystem::onMemberDeath(m_level.squads, idx, pool);

    // Friendly NPC death speech — set before loot drop so it's visible
    if (pool.entities[idx].flags & ENT_FRIENDLY) {
        pool.entities[idx].speechText = "Avenge... me...";
        pool.entities[idx].speechTimer = 4.0f;
    }

    // Resolve who actually got the kill (M8 attribution pattern, applied to the preamble
    // path too). `killerSlot` is stamped on the entity by Combat / Projectile / meteor code.
    //   - host or unattributed → write to m_localPlayer (swap alias)
    //   - remote NetPlayer     → write to m_players[ks] directly
    // The host swap-alias path is preserved when `killer != nullptr`, so split-screen and SP
    // are unchanged. On a CLIENT we still fall through to the host swap-alias path (the
    // entity is server-replicated; the client preamble runs only on host's m_localPlayer
    // which has class+state — matches old behavior).
    const u8 ks = pool.entities[idx].killerSlot;
    Player* killer = &m_localPlayer;
    NetPlayer* killerNp = nullptr;
    PlayerClass killerClass = m_playerClass;
    if (m_netRole == NetRole::SERVER && ks != 0xFF && ks != m_localPlayerIndex &&
        ks < MAX_PLAYERS && m_players[ks].active) {
        killer      = nullptr;
        killerNp    = &m_players[ks];
        killerClass = killerNp->playerClass;
    }

    // Shadow Dance: extend by 0.3s on each kill (route by killer)
    auto getShadowDance = [&]() -> f32 { return killerNp ? killerNp->shadowDanceTimer : killer->shadowDanceTimer; };
    if (!(pool.entities[idx].flags & ENT_FRIENDLY) && getShadowDance() > 0.0f) {
        if (killerNp) {
            killerNp->shadowDanceTimer += 0.3f;
            killerNp->smokeTimer = killerNp->shadowDanceTimer;
        } else {
            killer->shadowDanceTimer += 0.3f;
            killer->smokeTimer = killer->shadowDanceTimer;
        }
    }

    // Wanderer: killing a marked enemy grants speed stack + resets Exploit Weakness cooldown
    if (killerClass == PlayerClass::WANDERER &&
        pool.entities[idx].markPreyTimer > 0.0f) {
        if (killerNp) {
            // +5% speed stack (3s, non-refreshing) — remote slot
            if (killerNp->markSpeedStacks < 20) {
                killerNp->markSpeedTimers[killerNp->markSpeedStacks] = 3.0f;
                killerNp->markSpeedStacks++;
            }
            // Exploit Weakness CD reset for a remote Wanderer: the host doesn't own that
            // client's m_classSkillStates, so ship a targeted SKILL_CD_RESET event and let
            // the client zero the matching slot. Reliable channel — one packet per
            // marked-enemy kill is cheap.
            u8 evBuf[sizeof(PacketHeader) + 1 + 2]; // hdr + eventType + skillId(u16 LE)
            PacketHeader* evHdr = reinterpret_cast<PacketHeader*>(evBuf);
            evHdr->type = NetPacketType::SV_EVENT;
            evHdr->flags = 0;
            evHdr->seq = 0;
            u32 off = sizeof(PacketHeader);
            evBuf[off++] = static_cast<u8>(NetEventType::SKILL_CD_RESET);
            u16 sid = static_cast<u16>(SkillId::EXPLOIT_WEAKNESS);
            std::memcpy(evBuf + off, &sid, 2); off += 2;
            Net::sendReliable(ks, evBuf, off);
        } else {
            Player& wp = *killer;
            if (wp.markSpeedStacks < 20) {
                wp.markSpeedTimers[wp.markSpeedStacks] = 3.0f;
                wp.markSpeedStacks++;
            }
            for (u8 cs = 0; cs < 4; cs++) {
                if (m_classSkillStates[cs].activeSkill == SkillId::EXPLOIT_WEAKNESS) {
                    m_classSkillStates[cs].cooldownTimer = 0.0f;
                    break;
                }
            }
        }
    }

    // Wanderer Exploit Weakness upgrade (floor >= 20): mark spreads to nearest alive enemy on kill
    if (killerClass == PlayerClass::WANDERER &&
        m_level.currentFloor >= 20 &&
        pool.entities[idx].markPreyTimer > 0.0f) {
        constexpr f32 SPREAD_RANGE_SQ = 5.0f * 5.0f;
        for (u32 si = 0; si < pool.activeCount; si++) {
            u32 sIdx = pool.activeList[si];
            if (sIdx == idx) continue;
            Entity& se = pool.entities[sIdx];
            if (!(se.flags & ENT_ACTIVE) || (se.flags & ENT_DEAD)) continue;
            if (se.flags & ENT_FRIENDLY) continue;
            if (se.markPreyTimer > 0.0f) continue; // already marked
            f32 dSq = lengthSq(se.position - pos);
            if (dSq < SPREAD_RANGE_SQ) {
                se.markPreyDmgMult = 1.6f;
                se.markPreyTimer = 5.0f;
                if (killerNp) killerNp->markTimer = 5.0f;
                else          killer->markTimer = 5.0f;
                break; // spread to one nearest
            }
        }
    }

    // Mark Prey chain clear: if marked enemy dies, arrows rain on nearby enemies
    if (pool.entities[idx].markPreyTimer > 0.0f &&
        !(pool.entities[idx].flags & ENT_FRIENDLY)) {
        // Original check was "weapon equipped" via moveSpeed > 0 — both Player and NetPlayer
        // surface moveSpeed identically, so the routed read keeps the same heuristic.
        f32 killerMoveSpeed = killerNp ? killerNp->moveSpeed : killer->moveSpeed;
        f32 chainDmg = killerMoveSpeed > 0.0f
            ? 15.0f * (1.0f + (m_level.currentFloor + m_difficulty * 50 - 1) * 0.06f)
            : 15.0f;
        EntityHandle nearby[MAX_ENTITIES];
        f32 nearDists[MAX_ENTITIES];
        u32 nearCnt = CombatQuery::queryConeSorted(
            pool, pos, {0, -1, 0}, -1.0f, 6.0f, nearby, nearDists, MAX_ENTITIES);
        for (u32 nc = 0; nc < nearCnt; nc++) {
            if (nearby[nc].index == idx) continue;
            Entity* ne = handleGet(pool, nearby[nc]);
            if (!ne || (ne->flags & ENT_FRIENDLY) || (ne->flags & ENT_DEAD)) continue;
            // Spawn arrow impacts as PendingMeteors
            for (u32 arr = 0; arr < 5; arr++) {
                extern PendingMeteor s_meteors[MAX_PENDING_METEORS];
                for (u32 m = 0; m < MAX_PENDING_METEORS; m++) {
                    if (!s_meteors[m].active) {
                        f32 ofs = (std::rand() / static_cast<f32>(RAND_MAX)) * 0.5f;
                        s_meteors[m].position = ne->position + Vec3{ofs - 0.25f, 0, ofs - 0.25f};
                        s_meteors[m].damage   = chainDmg;
                        s_meteors[m].radius   = 0.8f;
                        s_meteors[m].timer    = 0.1f + arr * 0.05f;
                        s_meteors[m].active   = true;
                        s_meteors[m].healsPlayer = false;
                        // Audit P1: recycled PendingMeteor slot retains the previous user's
                        // .caster, which was then stamped via setAttackingPlayer in updateMeteors
                        // and credited the wrong slot for the chain-arrow kills. Stamp the
                        // current killer's net slot (works for host kill and remote-Wanderer
                        // kill identically — 0xFF only when killer is unattributed, but the
                        // outer gate requires markPreyTimer > 0 so an attributed Wanderer kill).
                        s_meteors[m].caster   = ks;
                        s_meteors[m].color    = {0.6f, 0.4f, 0.1f};
                        break;
                    }
                }
            }
        }
    }

    // Bomber death explosion — AoE damage + burn in radius on death
    if ((pool.entities[idx].enemyRole & EnemyRole::BOMBER) &&
        !(pool.entities[idx].flags & ENT_FRIENDLY)) {
        f32 explosionRadius = 3.0f;
        f32 explosionDmg = pool.entities[idx].damage * 2.5f; // suicide hits hard (was 0.8 — and under the old detonation range it whiffed entirely)
        f32 burnDur = pool.entities[idx].onHitDuration;
        f32 burnDps = pool.entities[idx].onHitDps;

        // Players take the blast only where player HP is authoritative — SP/split (NONE) and the
        // host (SERVER). On a CLIENT the server applies it and it arrives via snapshot, so doing it
        // here would double-count. Damage goes to the PERSISTENT m_localPlayers[] array (not the
        // m_localPlayer swap alias, whose writes are discarded after tickSharedSystems) — which is
        // exactly why the blast previously dealt nothing even point-blank.
        if (m_netRole != NetRole::CLIENT) {
            // Local players (SP/split-screen locals; the host is slot 0 with m_splitPlayerCount==1).
            for (u8 p = 0; p < m_splitPlayerCount; p++) {
                if (m_playerDead[p]) continue;
                Player& lp = m_localPlayers[p];
                Vec3 d = lp.position - pos;
                if (sqrtf(d.x * d.x + d.z * d.z) < explosionRadius) {
                    Combat::applyDamageToPlayer(lp, explosionDmg, &pos);
                    if (burnDur > 0.0f) { lp.burnTimer = fmaxf(lp.burnTimer, burnDur); lp.burnDps = burnDps; }
                }
            }
            // Networked co-op: damage remote NetPlayers through a throwaway view so it rides the
            // snapshot (mirrors buildRemotePlayerViews: skip the host slot + inactive/dead remotes).
            if (m_netRole == NetRole::SERVER) {
                for (u8 s = 0; s < MAX_PLAYERS; s++) {
                    if (s == m_localPlayerIndex) continue;
                    const NetPlayer& np = m_players[s];
                    if (!np.active || np.isDead) continue;
                    Vec3 d = np.position - pos;
                    if (sqrtf(d.x * d.x + d.z * d.z) >= explosionRadius) continue;
                    Player view;
                    buildRemotePlayerView(s, view);
                    Combat::applyDamageToPlayer(view, explosionDmg, &pos);
                    if (burnDur > 0.0f) { view.burnTimer = fmaxf(view.burnTimer, burnDur); view.burnDps = burnDps; }
                    applyRemotePlayerView(view, s);
                }
            }
        }
        // Damage friendly NPCs in radius
        for (u32 ni = 0; ni < pool.activeCount; ni++) {
            u32 nIdx = pool.activeList[ni];
            Entity& npc = pool.entities[nIdx];
            if (nIdx == idx) continue;
            if (!(npc.flags & ENT_FRIENDLY)) continue;
            if (npc.flags & ENT_DEAD) continue;
            f32 nDist = length(npc.position - pos);
            if (nDist < explosionRadius) {
                EntityHandle nh = {static_cast<u16>(nIdx), npc.generation};
                Combat::applyDamage(pool, nh, explosionDmg);
            }
        }
        // Visual: spawn explosion particles
        ParticleSystem::spawnExplosion(m_particles, pos, explosionRadius * 0.5f);
    }

    // --- Champion death novas (MOLTEN / FROZEN) ---
    // Killing a champion is not the end of the encounter: Molten leaves a fire blast you must not be
    // standing in, Frozen locks you in place while the rest of the pack closes. Mirrors the BOMBER
    // block above — including the remote-NetPlayer path, without which the blast would exist only on
    // the host's screen and hit nobody else.
    {
        const Entity& champ = pool.entities[idx];
        if ((champ.flags & ENT_CHAMPION) && champ.champAffixes != 0) {
            const bool molten = (champ.champAffixes & ChampAffix::MOLTEN) != 0;
            const bool frozen = (champ.champAffixes & ChampAffix::FROZEN) != 0;

            if (molten) {
                const f32 dmg    = champ.damage * Champion::MOLTEN_NOVA_PCT;
                const f32 radius = Champion::MOLTEN_NOVA_RADIUS;
                for (u8 p = 0; p < m_splitPlayerCount; p++) {
                    if (m_playerDead[p]) continue;
                    Player& lp = m_localPlayers[p];
                    Vec3 d = lp.position - pos;
                    if (sqrtf(d.x * d.x + d.z * d.z) < radius)
                        Combat::applyDamageToPlayer(lp, dmg, &pos);
                }
                if (m_netRole == NetRole::SERVER) {
                    for (u8 s = 0; s < MAX_PLAYERS; s++) {
                        if (s == m_localPlayerIndex) continue;
                        const NetPlayer& np = m_players[s];
                        if (!np.active || np.isDead) continue;
                        Vec3 d = np.position - pos;
                        if (sqrtf(d.x * d.x + d.z * d.z) >= radius) continue;
                        Player view;
                        buildRemotePlayerView(s, view);
                        Combat::applyDamageToPlayer(view, dmg, &pos);
                        applyRemotePlayerView(view, s);
                    }
                }
                // emitNovaFX spawns locally AND broadcasts SV_EVENT/NOVA_FX on the server, so the
                // ring is visible to every guest — not just whoever's machine ran the death.
                emitNovaFX(pos, radius, {1.0f, 0.45f, 0.12f});
                ParticleSystem::spawnExplosion(m_particles, pos, radius * 0.5f);
            }

            if (frozen) {
                // No damage — the threat is the freeze, not the hit.
                const f32 radius = Champion::FROZEN_NOVA_RADIUS;
                for (u8 p = 0; p < m_splitPlayerCount; p++) {
                    if (m_playerDead[p]) continue;
                    Player& lp = m_localPlayers[p];
                    Vec3 d = lp.position - pos;
                    if (sqrtf(d.x * d.x + d.z * d.z) < radius)
                        lp.freezeTimer = fmaxf(lp.freezeTimer, Champion::FROZEN_FREEZE_SEC);
                }
                if (m_netRole == NetRole::SERVER) {
                    for (u8 s = 0; s < MAX_PLAYERS; s++) {
                        if (s == m_localPlayerIndex) continue;
                        NetPlayer& np = m_players[s];
                        if (!np.active || np.isDead) continue;
                        Vec3 d = np.position - pos;
                        if (sqrtf(d.x * d.x + d.z * d.z) >= radius) continue;
                        // freezeTimer lives on NetPlayer and is already replicated in SnapPlayer, so
                        // the guest both sees and feels the freeze.
                        np.freezeTimer = fmaxf(np.freezeTimer, Champion::FROZEN_FREEZE_SEC);
                    }
                }
                emitNovaFX(pos, radius, {0.45f, 0.80f, 1.0f});
            }
        }
    }
}

// Tracks hostile kills, then checks the floor 1-3 first-kill magic-drop guarantee.
// Returns true if the first-kill guarantee fired — caller must return immediately
// to skip boss and normal loot paths (matches original `return; // skip normal drop logic`).
bool Engine::handleFirstKillDrop(EntityPool& pool, u16 idx, Vec3 pos) {
    // Track hostile kills for the floor transition screen. Each player tallies their OWN kills:
    // only deaths credited to a local lane count (killerSlot is stamped by Combat::killEntity;
    // 0xFF = environmental/AI, and a remote guest's slot is not a local lane). The guest tallies
    // its own the same way from the SV_KILL broadcast (Engine::onKill) — its ghost sim never runs
    // this authoritative callback, which is why its counter used to sit at zero forever.
    if (!(pool.entities[idx].flags & ENT_FRIENDLY)) {
        if (pool.entities[idx].killerSlot < m_splitPlayerCount) {
            m_transition.floorKillCount++;
            // Lifetime counter for the killing lane (local lanes are net slots 0..count-1
            // here) — persisted via the stats sidecar, shown on the floor transition.
            m_totalKills[pool.entities[idx].killerSlot]++;
        }
        AudioSystem::playAt(SfxId::ENEMY_DEATH, pos, m_localPlayer.position);
    }

    // Floors 1-3: first hostile kill guarantees a magic (green) quality drop
    if (!s_firstKillDropGiven && m_level.currentFloor <= 3 &&
        !(pool.entities[idx].flags & ENT_FRIENDLY)) {
        s_firstKillDropGiven = true;
        // Loot/item level is u8 (ItemInstance.itemLevel is save-bound) and rollItem
        // wraps it mod 50 anyway, so cap the effective floor at 255 for loot.
        u16 entLvl = pool.entities[idx].level;
        u8 lvl = static_cast<u8>(entLvl > 255 ? 255 : entLvl);
        if (lvl < 1) lvl = 1;
        // Floor 1: force armor drops so tutorial teaches equipping gear.
        // MAGIC rarityFloor replaces the old post-roll force-upgrade: the rarity-window pool
        // pick means the def actually supports the tier (the old force could paint a
        // common-only def blue) and its affixes are rolled at that tier from the start.
        ItemInstance item;
        for (u32 attempt = 0; attempt < 20; attempt++) {
            item = ItemGen::rollItem(lvl, m_itemDefs,
                                     m_itemDefCount,
                                     m_affixDefs,
                                     m_affixDefCount,
                                     Rarity::MAGIC);
            if (isItemEmpty(item)) break;
            if (m_level.currentFloor > 1) break; // no restriction above floor 1
            // On floor 1, only allow shields so the tutorial teaches blocking
            ItemSlot slot = m_itemDefs[item.defId].slot;
            if (slot == ItemSlot::OFFHAND) break;
        }
        if (!isItemEmpty(item)) {
            Vec3 dropPos = pos + Vec3{0, 0.5f, 0};
            WorldItemSystem::spawn(m_worldItems, item,
                                   dropPos, &m_level.grid,
                                   pool.entities[idx].killerSlot); // (L8) reserve to the killer
            // D1.3 — Notify clients of the new loot item immediately (snapshot confirms next window).
            broadcastLootSpawn(m_worldItems, item.uid, dropPos,
                               item.defId < m_itemDefCount ? item.defId : 0xFFFF);
        }
        return true; // skip normal drop logic for this kill
    }

    return false; // no early exit — proceed to boss/normal loot checks
}

// Boss guaranteed loot drop — mini-bosses drop rare+, major bosses drop legendary.
// Returns true if a boss loot drop was handled — caller must return immediately
// to skip normal loot path (matches original `return; // skip normal loot path`).
bool Engine::handleBossLootDrop(EntityPool& pool, u16 idx, Vec3 pos) {
    // Only ROOT bosses drop the guaranteed boss haul (spawnerIdx == 0xFFFF). The Dungeon Engine's
    // recompiled wave-adds are real bosses (bossDefIdx set) but summoned (spawnerIdx == Engine),
    // so they fall through to normal loot — otherwise a single Source fight would dump ~12
    // guaranteed legendaries and saturate the 32-slot world-item pool. The Engine itself is a root
    // boss (spawnerIdx 0xFFFF), so its haul is unaffected.
    if (pool.entities[idx].bossDefIdx != 0xFF &&
        pool.entities[idx].bossDefIdx < m_bossDefs.count &&
        pool.entities[idx].spawnerIdx == 0xFFFF) {
        const BossDef& bd = m_bossDefs.defs[pool.entities[idx].bossDefIdx];
        // Loot level is u8 (save-bound ItemInstance.itemLevel) — cap effective floor at 255.
        u16 bossEntLvl = pool.entities[idx].level;
        u8 bossLvl = static_cast<u8>(bossEntLvl > 255 ? 255 : bossEntLvl);
        if (bossLvl < 1) bossLvl = 1;

        // Guaranteed quality drop. rollItem's rarityFloor raises the rolled tier and the
        // rarity-window pool pick does the rest: a LEGENDARY floor can only ever return one
        // of the named uniques, with affixes rolled at that tier — the old "re-roll 50× until
        // the def supports it, then force-upgrade + re-roll affixes" dance is obsolete.
        Rarity minRarity = static_cast<Rarity>(bd.lootGuarantee);
        ItemInstance bossItem = ItemGen::rollItem(bossLvl, m_itemDefs, m_itemDefCount,
                                                  m_affixDefs, m_affixDefCount, minRarity);
        if (!isItemEmpty(bossItem)) {
            Vec3 bossDropPos = pos + Vec3{0, 0.5f, 0};
            WorldItemSystem::spawn(m_worldItems, bossItem,
                                   bossDropPos, &m_level.grid,
                                   pool.entities[idx].killerSlot); // (L8) reserve to the killer
            // D1.3 — Notify clients of boss loot spawn.
            broadcastLootSpawn(m_worldItems, bossItem.uid, bossDropPos,
                               bossItem.defId < m_itemDefCount ? bossItem.defId : 0xFFFF);
        }

        // Bonus drops for major bosses
        for (u8 bd_i = 0; bd_i < bd.bonusDrops; bd_i++) {
            ItemInstance bonus = ItemGen::rollItem(bossLvl, m_itemDefs,
                                                   m_itemDefCount,
                                                   m_affixDefs,
                                                   m_affixDefCount);
            if (!isItemEmpty(bonus)) {
                Vec3 offset = {(f32)(bd_i) * 0.3f - 0.15f, 0.5f, 0.2f};
                Vec3 bonusDropPos = pos + offset;
                WorldItemSystem::spawn(m_worldItems, bonus,
                                       bonusDropPos, &m_level.grid,
                                       pool.entities[idx].killerSlot); // (L8) reserve to the killer
                // D1.3 — Notify clients of bonus boss loot.
                broadcastLootSpawn(m_worldItems, bonus.uid, bonusDropPos,
                                   bonus.defId < m_itemDefCount ? bonus.defId : 0xFFFF);
            }
        }

        // Always drop a globe from bosses
        ItemInstance globe;
        globe.defId = GLOBE_HEALTH_ID;
        globe.uid   = m_worldItems.nextUid++;
        WorldItemSystem::spawn(m_worldItems, globe,
                               pos + Vec3{0.2f, 0.5f, 0.0f}, &m_level.grid);
        // Globes are auto-pickup and rarely interesting for client-side UIs — skip broadcast.

        // --- Secret superboss key: milestone bosses drop a hidden source shard ---
        // HELL ONLY (m_difficulty 2): the key to the true ending drops exclusively at the top
        // difficulty, so the Engine fight is earned on Hell milestone bosses (floors 5..50 —
        // free-play floors keep working via the raw-floor wrap: Hell 100 → raw 50). Normal and
        // Nightmare milestone bosses drop nothing extra and give no hint; the s_sourceShards
        // set is session-only, so pre-gate saves can't smuggle shards in either.
        // Dead-stripped from the demo (constexpr guard). NEVER for the Engine itself: its
        // effective level recovers to raw floor 50, which would otherwise re-drop a floor-50
        // shard inside The Source. Like globes, the shard is auto-pickup and not broadcast —
        // both host and client pick it from their own world-item view (see updatePlayerPickup).
        if (!GameConst::kDemoBuild && m_difficulty == 2 && !pool.entities[idx].isEngine) {
            u8 rawFloor = static_cast<u8>(((bossEntLvl - 1) % 50) + 1); // 5→5 … 50→50, 55→5 …
            if (rawFloor >= 5 && rawFloor <= 50 && rawFloor % 5 == 0) {
                u8 bit = static_cast<u8>(rawFloor / 5 - 1);             // floor 5→bit0 … 50→bit9
                if (!(s_sourceShards & (1u << bit))) {                  // skip if already held this session
                    ItemInstance shard;
                    shard.defId     = SOURCE_SHARD_ID;
                    shard.itemLevel = rawFloor;                        // carries the lore-whisper index
                    shard.uid       = m_worldItems.nextUid++;
                    // spawnEssential, NOT spawn: the shard is created LAST — after the guaranteed
                    // haul, the bonus drops and the globe — so on a crowded boss floor it is the
                    // first thing a full pool refuses. spawn() returns false there and every caller
                    // ignored it, which means the key simply never existed and the run lost the
                    // superboss without a single line of feedback.
                    if (!WorldItemSystem::spawnEssential(m_worldItems, shard,
                                                         pos + Vec3{-0.2f, 0.5f, 0.0f}, &m_level.grid,
                                                         m_itemDefs, m_itemDefCount))  // defs → eviction spares pet drops
                        LOG_ERROR("Source shard (floor %u) could not be placed — superboss unreachable "
                                  "this run", rawFloor);
                }
            }
        }
        return true; // skip normal loot path
    }

    return false; // not a boss — proceed to normal loot roll
}

// Normal (non-boss, non-first-kill) loot roll + globe drop.
// Drop chance scales with enemy level/floor depth. Floor 1 forces shield drops
// so the tutorial teaches blocking. Void return — no early exit in original.
// Champion pack LEADERS drop a guaranteed item. An elite you must read, kite and out-play has to
// pay, or players learn to walk around them — which would make the whole feature a nuisance tax.
//
// MINIONS are deliberately excluded and fall through to the normal 40% roll. This mirrors the
// decision recorded above for the Engine's wave-adds: a guaranteed drop per pack member would put
// 5 rares on the floor per pack, and with champions now allowed on BOSS floors that lands on top of
// the boss's own guaranteed haul + bonusDrops in a single room. The world-item pool is finite (64),
// and a full pool makes WorldItemSystem::spawn fail SILENTLY — so over-generous guarantees don't
// produce more loot, they produce *vanished* loot.
// The loot goblin's payout: the rest of the sack, but ONLY if you actually caught it. A goblin that
// escapes expires via Entity::lifeTimer in EntitySystem::tickTimers, which frees the slot WITHOUT
// firing this callback at all — so escaping costs you the sack, which is the entire point of the
// chase. Nothing here needs to check "did it escape": by construction, it can't have.
bool Engine::handleGoblinLootDrop(EntityPool& pool, u16 idx, Vec3 pos) {
    const Entity& e = pool.entities[idx];
    if (!(e.flags & ENT_LOOT_GOBLIN)) return false;

    u8 lvl = static_cast<u8>(e.level > 255 ? 255 : e.level);
    if (lvl < 1) lvl = 1;

    u8 dropped = 0;
    for (u8 i = 0; i < Goblin::DEATH_DROPS; i++) {
        // Every death drop is a guaranteed LEGENDARY — with 1200 base HP the kill is a committed
        // DPS race against the escape clock, and the sack is what that race is for. The
        // rarityFloor pick guarantees a real named unique (see handleBossLootDrop).
        ItemInstance item = ItemGen::rollItem(lvl, m_itemDefs, m_itemDefCount,
                                              m_affixDefs, m_affixDefCount, Rarity::LEGENDARY);
        if (isItemEmpty(item)) continue;
        // Fan them out so the pile is readable rather than one item stacked on three others.
        const f32 ang = (6.2831853f * static_cast<f32>(i)) / static_cast<f32>(Goblin::DEATH_DROPS);
        Vec3 dropPos = pos + Vec3{ cosf(ang) * 0.6f, 0.5f, sinf(ang) * 0.6f };
        if (!WorldItemSystem::spawn(m_worldItems, item, dropPos, &m_level.grid, e.killerSlot)) {
            LOG_WARN("LootGoblin: death drop lost — world-item pool full");
            break;
        }
        broadcastLootSpawn(m_worldItems, item.uid, dropPos,
                           item.defId < m_itemDefCount ? item.defId : 0xFFFF);
        dropped++;
    }

    // 1% jackpot: the goblin can also drop a MINI version of itself — the pet-summon consumable
    // (infinite uses; Engine::togglePetCompanion). Its def is unrollable by ItemGen (minLevel 255,
    // dropWeight 0), so this roll is the ONLY source. LEGENDARY rarity is load-bearing, not
    // cosmetic: legendary world items are exempt from the 60 s despawn timer, and a 1-in-100 drop
    // must not rot on the floor while the player is still fighting across the room.
    if ((std::rand() % 100) == 0) {
        for (u32 di = 0; di < m_itemDefCount; di++) {
            // petEnemyIdx 0xFF singles out the goblin's own def — every OTHER petSummon def is a
            // "Mini <Enemy>" bound to an enemies.json entry (those drop from their enemy's
            // 1-in-10000 roll in handleNormalLootDrop, not from the goblin).
            if (!m_itemDefs[di].petSummon || m_itemDefs[di].petEnemyIdx != 0xFF) continue;
            ItemInstance pet;
            pet.defId      = static_cast<u16>(di);
            pet.rarity     = Rarity::LEGENDARY;
            pet.itemLevel  = 1;
            pet.affixCount = 0;
            pet.uid        = m_worldItems.nextUid++;   // direct-construction uid, like globes/shards
            Vec3 dropPos = pos + Vec3{0.0f, 0.7f, 0.0f};   // dead center of the ring of legendaries
            if (WorldItemSystem::spawn(m_worldItems, pet, dropPos, &m_level.grid, e.killerSlot)) {
                broadcastLootSpawn(m_worldItems, pet.uid, dropPos, pet.defId);
                LOG_INFO("LootGoblin: JACKPOT — dropped the Mini Loot Goblin companion");
            } else {
                LOG_WARN("LootGoblin: mini-goblin drop lost — world-item pool full");
            }
            break;
        }
    }

    LOG_INFO("LootGoblin: caught — dropped %u legendary item(s)", dropped);
    return true;   // never also roll the normal 40% table
}

bool Engine::handleChampionLootDrop(EntityPool& pool, u16 idx, Vec3 pos) {
    const Entity& e = pool.entities[idx];
    if (!(e.flags & ENT_CHAMPION) || e.champAffixes == 0) return false;

    // Pool-headroom guard. If the floor is already awash in loot, drop nothing rather than roll an
    // item that spawn() would silently swallow.
    u32 activeItems = 0;
    for (u32 i = 0; i < MAX_WORLD_ITEMS; i++)
        if (m_worldItems.items[i].active) activeItems++;
    if (activeItems + 2 >= MAX_WORLD_ITEMS) {
        LOG_WARN("Champion: world-item pool nearly full (%u/%u) — skipping guaranteed drop",
                 activeItems, MAX_WORLD_ITEMS);
        return false;
    }

    u16 lvl16 = e.level;
    u8  lvl   = static_cast<u8>(lvl16 > 255 ? 255 : lvl16);
    if (lvl < 1) lvl = 1;

    // Rarity floor scales with how nasty the champion was: a 3-affix champion is a real fight.
    u8 affixCount = 0;
    for (u8 b = 0; b < ChampAffix::COUNT; b++)
        if (e.champAffixes & static_cast<u8>(1u << b)) affixCount++;
    const Rarity minRarity = (affixCount >= 3) ? Rarity::LEGENDARY : Rarity::RARE;

    // rarityFloor pick — a LEGENDARY floor yields a real named unique, a RARE floor a proper
    // rare, affixes already rolled at the final tier (see handleBossLootDrop).
    ItemInstance item = ItemGen::rollItem(lvl, m_itemDefs, m_itemDefCount,
                                          m_affixDefs, m_affixDefCount, minRarity);
    if (isItemEmpty(item)) return false;

    Vec3 dropPos = pos + Vec3{0, 0.5f, 0};
    // Check the return: spawn() reports a full pool by returning false, and every OTHER caller in
    // this file ignores it — which is how a *guaranteed* drop can silently not exist.
    if (!WorldItemSystem::spawn(m_worldItems, item, dropPos, &m_level.grid, e.killerSlot)) {
        LOG_WARN("Champion: guaranteed drop LOST — world-item pool full");
        return false;
    }
    broadcastLootSpawn(m_worldItems, item.uid, dropPos,
                       item.defId < m_itemDefCount ? item.defId : 0xFFFF);
    LOG_INFO("Champion: guaranteed %s drop (mask 0x%02X, %u affixes)",
             minRarity == Rarity::LEGENDARY ? "LEGENDARY" : "RARE", e.champAffixes, affixCount);
    return true;
}

// Open a real chest: retire the sentinel and put its rolled loot where it stood. Lives here
// with the other loot-drop paths (it IS one — broadcastLootSpawn above is file-static). The
// roll happens at open time on the authoritative sim: a chest carries only a loot LEVEL
// (itemLevel), never an item, so nothing exists to desync, duplicate, or persist. The loot
// spawns as an ordinary free-for-all world item (rollItem assigns its uid) and reaches guests
// through the usual snapshot mirror + SV_LOOT_SPAWN event, exactly like a monster drop.
// SP/host tap calls this directly; a guest's CL_PICKUP_ITEM lands in handlePickupRequest's
// chest branch.
void Engine::openChest(u32 worldIdx) {
    if (worldIdx >= MAX_WORLD_ITEMS) return;
    WorldItem& wi = m_worldItems.items[worldIdx];
    if (!wi.active || !isChest(wi.item)) return;

    u8 lootLevel = wi.item.itemLevel;
    if (lootLevel < 1) lootLevel = 1;
    const Vec3 lootPos = wi.position + Vec3{0, 0.2f, 0};
    wi.active = false;
    if (m_worldItems.activeCount > 0) m_worldItems.activeCount--;

    const ItemInstance loot = ItemGen::rollItem(lootLevel, m_itemDefs, m_itemDefCount,
                                                m_affixDefs, m_affixDefCount);
    if (isItemEmpty(loot)) return;   // failed roll = the chest was empty; nothing to place
    if (WorldItemSystem::spawn(m_worldItems, loot, lootPos, &m_level.grid)) {
        broadcastLootSpawn(m_worldItems, loot.uid, lootPos,
                           loot.defId < m_itemDefCount ? loot.defId : 0xFFFF);
    }
}

void Engine::handleNormalLootDrop(EntityPool& pool, u16 idx, Vec3 pos) {
    // Hostile enemies only drop loot; chance scales with floor depth.
    // Loot level is u8 (save-bound ItemInstance.itemLevel) — cap effective floor at 255.
    u16 entLvl = pool.entities[idx].level;
    u8 enemyLevel = static_cast<u8>(entLvl > 255 ? 255 : entLvl);
    f32 dropChance = GameConst::LOOT_DROP_CHANCE + enemyLevel * 0.01f;
    if (dropChance > 0.70f) dropChance = 0.70f;
    if (!(pool.entities[idx].flags & ENT_FRIENDLY) &&
        (std::rand() % 100) < static_cast<int>(dropChance * 100.0f)) {
        if (enemyLevel < 1) enemyLevel = 1;
        ItemInstance item;
        // Floor 1: force shield drops so the tutorial teaches blocking
        for (u32 attempt = 0; attempt < 20; attempt++) {
            item = ItemGen::rollItem(enemyLevel, m_itemDefs,
                                     m_itemDefCount,
                                     m_affixDefs,
                                     m_affixDefCount);
            if (isItemEmpty(item)) break;
            if (m_level.currentFloor > 1) break;
            ItemSlot slot = m_itemDefs[item.defId].slot;
            if (slot == ItemSlot::OFFHAND) break;
        }
        if (!isItemEmpty(item)) {
            // (L8) Reserve the drop to whoever landed the kill for a few seconds (their kills,
            // their loot), then it falls back to free-for-all. killerSlot is 0xFF for
            // environmental/AoE kills, which spawn() treats as free-for-all. Globes stay
            // free-for-all auto-pickup below.
            u8 killer = pool.entities[idx].killerSlot;
            Vec3 normalDropPos = pos + Vec3{0, 0.5f, 0};
            WorldItemSystem::spawn(m_worldItems, item,
                                   normalDropPos, &m_level.grid, killer);
            // D1.3 — Notify clients of normal loot spawn so UI can react immediately.
            broadcastLootSpawn(m_worldItems, item.uid, normalDropPos,
                               item.defId < m_itemDefCount ? item.defId : 0xFFFF);
        }

        // Chance to drop a globe (restores both HP and energy on pickup)
        if ((std::rand() % 100) < static_cast<int>(GameConst::GLOBE_DROP_CHANCE * 100.0f)) {
            ItemInstance globe;
            globe.defId = GLOBE_HEALTH_ID; // single globe type
            globe.uid   = m_worldItems.nextUid++;
            WorldItemSystem::spawn(m_worldItems, globe,
                                   pos + Vec3{0.2f, 0.5f, 0.0f});
        }
    }

    // 1-in-10000 companion jackpot: every normal enemy can drop the "Mini <itself>" pet
    // consumable (COMMON — the drop rate is the prestige, not the border color). Independent
    // of the regular loot roll above, and OUTSIDE it: a jackpot must never be eaten by the
    // ordinary drop gate. The def is unrollable by ItemGen (minLevel 255, dropWeight 0), so
    // this roll is its only source; COMMON would normally rot on the 60 s trash timer, but
    // WorldItemSystem::update exempts petSummon defs — and the tri-beam beacon
    // (renderWorldItems) marks it from across the room.
    const Entity& deadEnt = pool.entities[idx];
    if (!(deadEnt.flags & ENT_FRIENDLY) && deadEnt.enemyDefIdx < MAX_ENEMY_DEFS &&
        (std::rand() % 10000) == 0) {
        const u16 petDef = m_petItemForEnemy[deadEnt.enemyDefIdx];
        if (petDef != 0xFFFF) {
            ItemInstance petItem;
            petItem.defId      = petDef;
            petItem.rarity     = Rarity::COMMON;
            petItem.itemLevel  = 1;
            petItem.affixCount = 0;
            petItem.uid        = m_worldItems.nextUid++;   // direct-construction uid (globes/shards pattern)
            Vec3 petPos = pos + Vec3{-0.2f, 0.7f, 0.2f};
            if (WorldItemSystem::spawn(m_worldItems, petItem, petPos, &m_level.grid,
                                       deadEnt.killerSlot)) {
                broadcastLootSpawn(m_worldItems, petItem.uid, petPos, petItem.defId);
                LOG_INFO("Pet: JACKPOT — '%s' dropped (enemy def %u)",
                         m_itemDefs[petDef].name, deadEnt.enemyDefIdx);
            } else {
                LOG_WARN("Pet: jackpot drop lost — world-item pool full");
            }
        }
    }
}

// Ring on-kill passives — Soul Harvest, Phase Strike, Void Kill.
// Only fires for hostile kills and when a ring passive is active.
// Void return — no early exit in original.
void Engine::handleOnKillRingPassives(EntityPool& pool, u16 idx, Vec3 pos) {
    if (pool.entities[idx].flags & ENT_FRIENDLY) return;

    // (M8) Credit the actual killer, not always the host. killerSlot is stamped on the entity
    // by Combat (direct/projectile/skill paths) and by updateMeteors (caster). 0xFF = no
    // specific player (e.g. world hazard) — skip ring credit entirely.
    const u8 killer = pool.entities[idx].killerSlot;
    if (killer == 0xFF) return;
    const bool isHost   = (killer == m_localPlayerIndex);
    const bool isRemote = (!isHost && killer < MAX_PLAYERS && m_players[killer].active);
    if (!isHost && !isRemote) return;

    // Effective ringPassive for the credited player — host uses the swap alias (so the
    // existing HUD/sync paths see the stack changes), remote uses the NetPlayer's mirror.
    const SkillId effRing = isHost ? m_ringPassive : m_players[killer].ringPassive;
    if (effRing == SkillId::NONE) return;

    {
        // Soul Harvest: +5% speed, +3% damage per stack for 10s (max 5)
        if (effRing == SkillId::SOUL_HARVEST) {
            if (isHost) {
                Player& p = m_localPlayer;
                if (p.soulHarvestStacks < 5) p.soulHarvestStacks++;
                p.soulHarvestTimer = 5.0f;
            } else {
                NetPlayer& np = m_players[killer];
                if (np.soulHarvestStacks < 5) np.soulHarvestStacks++;
                np.soulHarvestTimer = 5.0f;
            }
            // Speed bonus applied via moveSpeed multiplier
        }
        // Phase Strike (Shadow Ring): 20% on kill, drop smoke bomb — 0.5s stealth + aggro reset
        if (effRing == SkillId::PHASE_STRIKE && (std::rand() % 100) < 20) {
            if (isHost) m_localPlayer.smokeTimer = 0.5f;
            else        m_players[killer].smokeTimer = 0.5f;
            // Reset aggro on all enemies within 6m
            for (u32 si = 0; si < MAX_ENTITIES; si++) {
                Entity& se = pool.entities[si];
                if (!(se.flags & ENT_ACTIVE) || (se.flags & ENT_DEAD)) continue;
                if (se.flags & ENT_FRIENDLY) continue;
                Vec3 diff = se.position - pos;
                if (diff.x * diff.x + diff.z * diff.z < 6.0f * 6.0f) {
                    se.aiState = AIState::IDLE;
                    se.velocity = {0, 0, 0};
                }
            }
            // Dark grey smoke nova at kill position
            for (u32 ni = 0; ni < Engine::MAX_NOVA_FX; ni++) {
                if (!m_fx.novaFX[ni].active) {
                    m_fx.novaFX[ni] = {pos, 4.0f, 1.2f, true, {0.3f, 0.3f, 0.35f}};
                    break;
                }
            }
        }
        // Void Kill: 15% chance to spawn void zone on corpse
        if (effRing == SkillId::VOID_KILL && (std::rand() % 100) < 15) {
            // AoE void damage to nearby enemies
            EntityHandle aoeHits[MAX_ENTITIES];
            f32 aoeDists[MAX_ENTITIES];
            u32 aoeCount = CombatQuery::queryConeSorted(
                pool, pos, {0,-1,0}, -1.0f, 3.0f,
                aoeHits, aoeDists, MAX_ENTITIES);
            for (u32 h = 0; h < aoeCount; h++) {
                Entity* ve = handleGet(pool, aoeHits[h]);
                if (!ve || (ve->flags & ENT_DEAD) || (ve->flags & ENT_FRIENDLY)) continue;
                f32 missingHp = ve->maxHealth - ve->health;
                Combat::applyDamage(pool, aoeHits[h], 10.0f + missingHp * 0.6f);
            }
            // Dark purple nova visual
            for (u32 ni = 0; ni < Engine::MAX_NOVA_FX; ni++) {
                if (!m_fx.novaFX[ni].active) {
                    m_fx.novaFX[ni] = {pos, 3.0f, 1.0f, true, {0.4f, 0.1f, 0.6f}};
                    break;
                }
            }
        }
    }
}
