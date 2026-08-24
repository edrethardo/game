// engine_autoplay_arena.cpp — the ARENA bot: autoplay plays PvP deathmatch.
//
// Exists so arena MAPS can be EVALUATED without four humans (Aaron: "Bau bots für Arena
// Multiplayer um sie evaluieren zu können"): a soak launches a host and N bot clients, every
// lane fights through the exact human input path, and the [ARENA-BOT]/[ARENA] telemetry turns
// a match into a dataset (time-to-first-blood, kill cadence, vertical occupancy).
//
// The design mirrors autoplayTownStep/zoneAutoplayStep: a DRIVER-side branch that owns the one
// thing the pure brain cannot express — here, that the hostiles are PLAYERS, not entities.
// buildBotView still supplies everything else (weapon reach, skills, HP, block/projectile
// scans); this file only REPLACES the target list with the live NetPlayer slots and re-enables
// the brain (buildBotView deliberately reports onNormalFloor=false in the arena, which would
// idle the bot — the pre-bot behaviour).
//
// What deliberately does NOT change: firing rides the ordinary held GameActions, so a bot's
// hits flow through the same Combat::pvp* validation as a human's (host-authoritative, client
// inputs on the wire) — the bot cannot cheat, which is the whole point of evaluating with it.

#include "engine/engine.h"
#include "platform/input.h"    // Input::clearBotHeld — release synthetic holds on a dead lane
#include "game/autoplay_brain.h"
#include "game/autoplay_combat.h"
#include "game/autoplay_nav.h"    // Autoplay::stepAllowed — the loot-run heading rides the same veto as travel
#include "world/raycast.h"
#include "core/log.h"
#include <cmath>

void Engine::autoplayArenaStep(f32 dt, bool uiOpen) {
    // Dead lane: the server's 3 s auto-respawn brings us back without any input — just release
    // everything so a held Fire can't leak into the first alive frame.
    if (m_localPlayer.health <= 0.0f) { Input::clearBotHeld(); return; }

    Autoplay::BotView v = buildBotView();
    // The brain idles off "normal" floors, and the arena is deliberately one of them for the
    // PvE bot. Re-enable it here: the arena IS this bot's floor. floorDoorActive stays false,
    // so DESCEND never arms; the flow field (seeded at the arena centre by buildArenaLevel)
    // gives TRAVEL a rally point whenever no target has line of sight.
    v.onNormalFloor = true;

    // --- The target list: every OTHER live combatant ---------------------------------------
    // NetPlayer positions are FEET (the seat/spawn anchor), so aim at chest height. LOS is the
    // same world-only slab-aware DDA the PvE scan uses — a body must never make geometry
    // disappear, and the arena's platform slabs must block sightlines or balcony players would
    // be "visible" through the floor.
    static Autoplay::BotTarget s_tgts[MAX_PLAYERS];
    const u8  self = activeNetSlot();
    const Vec3 eye = m_localPlayer.position + Vec3{0.0f, m_localPlayer.eyeHeight, 0.0f};
    u32 n = 0;
    // One combatant -> one BotTarget, whatever array it lives in. slotId keys the identity
    // and the respawn-grace lookup; nearest-first insertion because the brain and target
    // stickiness assume the list is sorted by distance.
    auto addCombatant = [&](u8 slotId, const Vec3& feetPos, const Vec3& vel, f32 hp) {
        if (hp <= 0.0f) return;                             // dead: waiting out the respawn timer
        Autoplay::BotTarget t;
        t.id   = 0x50560000u | (slotId + 1u);               // 'PV'<<16 | slot+1 — stable identity
        t.pos  = feetPos + Vec3{0.0f, 0.9f, 0.0f};          // NetPlayer positions are FEET: aim chest
        t.vel  = vel;
        t.hp   = hp;
        // A freshly respawned player is briefly invulnerable (arenaRespawnSlot's grace) — mark
        // it so pickTarget swings to someone shootable instead of feeding the grace window.
        t.invulnerable = (slotId < MAX_PLAYERS && m_arenaRespawn[slotId] > 0.0f);
        // Every combatant can hurt us from range, and none telegraphs a swing the block tap
        // could read (attackTimer stays at its huge default) — the projectile-ETA block scan in
        // buildBotView still times real incoming shots.
        t.isRanged    = true;
        t.attackRange = 24.0f;
        const Vec3 to = t.pos - eye;
        t.dist  = length(to);
        t.feetY = feetPos.y;
        if (t.dist > 1e-3f) {
            const RayHit hit = Raycast::cast(m_level.grid, eye, to * (1.0f / t.dist), t.dist);
            t.hasLOS = !hit.hit;
        } else {
            t.hasLOS = true;
        }
        u32 slot = n++;
        while (slot > 0 && s_tgts[slot - 1].dist > t.dist) {
            s_tgts[slot] = s_tgts[slot - 1];
            slot--;
        }
        s_tgts[slot] = t;
    };
    // Networked combatants. THREE sources, one per net role, because each role has a different
    // authority on where the other players are:
    //  - SERVER: m_players[] is the live simulation.
    //  - CLIENT: m_players[] is NEVER activated client-side (onPlayerJoin is a server affair) —
    //    the first MP soak had every client bot blind (tgts=0), all four parked on the crown,
    //    and the only HP loss in six minutes was fall damage. The client's authority on remote
    //    players is the snapshot INTERPOLATION mirror (m_renderInterp), the same source the
    //    renderer and HUD trust.
    //  - NONE (local versus): the fighters live in m_localPlayers[] (loop below).
    if (m_netRole == NetRole::CLIENT) {
        for (u8 s = 0; s < MAX_PLAYERS; s++) {
            if (s == self || !m_renderInterp.playerActive[s]) continue;
            addCombatant(s, m_renderInterp.playerPositions[s],
                         m_renderInterp.playerVelXZ[s], m_renderInterp.playerHealth[s]);
        }
    } else {
        for (u8 s = 0; s < MAX_PLAYERS; s++) {
            if (s == self || !m_players[s].active) continue;
            addCombatant(s, m_players[s].position, m_players[s].velocity, m_players[s].health);
        }
    }
    // Local-versus lanes. NetRole NONE only: on a host-couch the sibling lane is ALSO an
    // active m_players slot and would be counted twice.
    if (m_netRole == NetRole::NONE) {
        for (u8 lane = 0; lane < m_splitPlayerCount && lane < MAX_LOCAL_PLAYERS; lane++) {
            if (lane == m_localPlayerIndex) continue;
            const Player& q = m_localPlayers[lane];
            addCombatant(lane, q.position, q.velocity, q.health);
        }
    }
    v.targets     = s_tgts;
    v.targetCount = n;

    // --- THE LOOT RUN (WB-299): the mode's economy needs runners. -------------------------
    // The nearest live drop is always scanned (telemetry needs it every tick — the first
    // version only looked when nothing was visible, which read as "clients never see loot"
    // while they were actually plinking at 22 m the whole match). The run itself fires in
    // two shapes:
    //  - NO line of sight to anyone: the drop replaces the map centre as the travel goal.
    //  - VISIBLE enemy but the drop is much closer (under 12 m and well inside the enemy
    //    distance): keep shooting, but WALK to the loot — the FEET are overridden after
    //    decide() (the boss-movement-fill pattern), because FIGHT's kite/strafe movement
    //    ignores flowDir entirely. Without this, two ranged bots plink forever and the
    //    wave economy never touches the match.
    f32  lootD = -1.0f;
    Vec3 lootPos{};
    {
        f32 bestD2 = 1e30f;
        for (u32 i = 0; i < MAX_WORLD_ITEMS; i++) {
            const WorldItem& wi = m_worldItems.items[i];
            if (!wi.active) continue;
            const f32 d2 = lengthSq(wi.position - m_localPlayer.position);
            if (d2 < bestD2) { bestD2 = d2; lootPos = wi.position; }
        }
        if (bestD2 < 1e29f) lootD = std::sqrt(bestD2);
    }
    bool anyLOS = false;
    f32  nearestVis = 1e30f;
    for (u32 i = 0; i < n; i++)
        if (s_tgts[i].hasLOS) { anyLOS = true; if (s_tgts[i].dist < nearestVis) nearestVis = s_tgts[i].dist; }
    // The vetoed heading toward the drop (lava maps: a straight line can cross a trace).
    Vec3 lootDir{0, 0, 0};
    if (lootD > 1.0f) {
        Vec3 dir = lootPos - m_localPlayer.position;
        dir.y = 0.0f;
        if (lengthSq(dir) > 1e-4f) {
            dir = normalize(dir);
            const f32 feetY = m_localPlayer.position.y;
            if (!Autoplay::stepAllowed(m_level.grid, m_localPlayer.position, feetY, dir, false)) {
                const f32 c = 0.70710678f;
                const Vec3 l{dir.x * c - dir.z * c, 0.0f, dir.x * c + dir.z * c};
                const Vec3 r{dir.x * c + dir.z * c, 0.0f, -dir.x * c + dir.z * c};
                if      (Autoplay::stepAllowed(m_level.grid, m_localPlayer.position, feetY, l, false)) dir = l;
                else if (Autoplay::stepAllowed(m_level.grid, m_localPlayer.position, feetY, r, false)) dir = r;
                else dir = {0, 0, 0};
            }
            lootDir = dir;
        }
    }
    if (!anyLOS && lengthSq(lootDir) > 1e-4f) {
        v.flowDir = lootDir; v.flowValid = true; v.atExit = false;   // travel goal = the drop
    }

    Autoplay::BotIntent in = Autoplay::decide(v);

    // Feet-to-loot override DURING a fight: shooting continues (aim/fire untouched), but the
    // walk goes to the nearby drop. Same shape as the boss-floor movement fill — FIGHT's own
    // movement never reads flowDir, so a flowDir override alone cannot move a fighting bot.
    if (anyLOS && lengthSq(lootDir) > 1e-4f && lootD > 1.0f &&
        lootD < 12.0f && lootD < nearestVis * 0.75f) {
        // Decompose the world heading into the player's forward/right basis (player.cpp's
        // own convention: forward = {-sin(yaw), 0, -cos(yaw)}).
        const f32 sy = std::sin(m_localPlayer.yaw), cy = std::cos(m_localPlayer.yaw);
        const f32 fwd   = lootDir.x * -sy + lootDir.z * -cy;
        const f32 right = lootDir.x * -cy + lootDir.z *  sy;
        in.moveFwd = in.moveBack = in.moveLeft = in.moveRight = false;
        if (fwd   >  0.38f) in.moveFwd   = true;
        if (fwd   < -0.38f) in.moveBack  = true;
        if (right >  0.38f) in.moveRight = true;
        if (right < -0.38f) in.moveLeft  = true;
    }

    // 1 Hz telemetry, per lane — the soak's dataset. y carries the VERTICAL story (pit vs
    // balcony occupancy), tgtDist the engagement range; kills come from the shared scoreboard.
    ap().arenaTelemTimer += dt;
    if (ap().arenaTelemTimer >= 1.0f) {
        ap().arenaTelemTimer = 0.0f;
        LOG_INFO("[ARENA-BOT] slot=%u pos=(%.1f,%.1f,%.1f) hp=%.0f tgts=%u nearest=%.1f fire=%d kills=%u loot=%.1f",
                 self, m_localPlayer.position.x, m_localPlayer.position.y,
                 m_localPlayer.position.z, m_localPlayer.health, n,
                 (n > 0) ? s_tgts[0].dist : -1.0f,
                 in.fire ? 1 : 0, m_arenaScore.kills[self], lootD);
    }

    applyBotIntent(in, uiOpen, dt, v.weaponIsMelee);
}
