#pragma once

// arena.h — pure PvP deathmatch rules for Arena mode (sentinel floor 97).
//
// Pure functions on plain state (the stash.h pattern) so the rules are unit-testable
// without the engine (tests/game/test_arena.cpp). The engine owns WHEN these run —
// server-side death detection ticks recordKill, the respawn timer picks a pad — this
// header owns only the math. Nothing here touches pools, networking, or rendering.

#include "core/types.h"
#include "core/math.h"

namespace Arena {
    // == MAX_PLAYERS. Kept as a local constant so this header stays engine-free; the
    // engine boundary static_assert's the two are equal (engine.h).
    constexpr u32 MAX_COMBATANTS = 4;
    // The map roster. The id travels in the SV_LEVEL_SEED broadcast's seed byte (levelSeed %
    // MAP_COUNT), so adding a map costs no protocol change — both peers carve from the id.
    constexpr u32 MAP_COUNT = 5;
    inline const char* mapName(u32 id) {
        switch (id % MAP_COUNT) {
            case 1:  return "The Crucible";     // molten ring road: causeways + loop over lava
            case 2:  return "The Pit";          // inverted amphitheatre with a rim-deck arcade
            case 3:  return "The Motherboard";  // circuit-board alleys around a defended die
            case 4:  return "The Mainframe";    // WB-303: balcony ring + the mega-item vault
            default: return "Combat Hall";      // the original two-story colosseum
        }
    }
    // First to FIVE since the Quake-mode redesign (WB-296) — matches are meant to be short,
    // loot-escalation rounds, not attrition ladders. Was 10.
    constexpr u32 KILL_TARGET    = 5;
    constexpr f32 RESPAWN_DELAY  = 3.0f;   // seconds dead before the auto-respawn

    struct Score { u16 kills[MAX_COMBATANTS] = {}; };

    // --- THE LOOT ESCALATION (WB-297): the thing players fight OVER -----------------------
    // Every LOOT_INTERVAL seconds the server drops ONE item on one of the map's loot anchors
    // (never the same anchor twice in a row — "immer woanders"), and every wave is stronger
    // than the last. Pure so the curve is pinned by test.
    constexpr f32 LOOT_INTERVAL = 15.0f;
    constexpr u32 LOOT_MAX_ANCHORS = 8;

    // Item level of wave N (0-based): a steep ramp — naked classes fight over the drops, so
    // wave 0 already matters (ilvl 6 beats a starting weapon) and the cap is the game's own
    // ladder end. ~2 minutes to reach the top: matches are first-to-5, not marathons.
    inline u8 lootItemLevel(u32 wave) {
        const u32 lvl = 6 + wave * 6;
        return static_cast<u8>(lvl > 50 ? 50 : lvl);
    }
    // From wave 4 on (60 s in), every second wave is FORCED legendary-or-better: the late
    // drops must be worth sprinting across the map into a fight for.
    inline bool lootWaveForcesLegendary(u32 wave) { return wave >= 4 && (wave % 2) == 0; }
    // --- MONSTER INTERLUDES (WB-298): the second loot source. -----------------------------
    // Every MONSTER_INTERVAL a monster (later: two) crawls out at an anchor and drops
    // equipment on death — a PvE objective both players want, i.e. another fight magnet.
    constexpr f32 MONSTER_INTERVAL = 40.0f;
    inline u32 monsterCountForWave(u32 wave) { return wave >= 6 ? 2u : 1u; }
    // Monsters scale WITH the loot they guard: naked players meet base stats, geared ones
    // meet a beefed copy. Capped so a late monster is a fight, not a raid boss.
    inline f32 monsterHealthMult(u32 wave) {
        const f32 m = 1.0f + 0.25f * static_cast<f32>(wave);
        return m > 4.0f ? 4.0f : m;
    }

    // Anchor pick: any anchor but the previous one. `roll` is the server's random draw —
    // the rule stays pure/testable, the entropy stays at the engine boundary.
    inline u32 nextLootAnchor(u32 roll, s32 last, u32 count) {
        if (count <= 1) return 0;
        u32 pick = roll % count;
        if (static_cast<s32>(pick) == last) pick = (pick + 1) % count;
        return pick;
    }

    // Credit a kill. killerSlot 0xFF (environmental / unknown attacker) or any out-of-range
    // slot records nothing — a death must never invent credit. Returns true and sets
    // winnerOut exactly when this kill reaches KILL_TARGET.
    inline bool recordKill(Score& s, u8 killerSlot, u8& winnerOut) {
        if (killerSlot >= MAX_COMBATANTS) return false;
        s.kills[killerSlot]++;
        if (s.kills[killerSlot] >= KILL_TARGET) { winnerOut = killerSlot; return true; }
        return false;
    }

    // Pick the respawn pad whose NEAREST living hostile is farthest away (max-min), on the
    // XZ plane (pads and players share the floor; Y is noise). Ties keep the lower index and
    // "no hostiles" yields pad 0 — deterministic, so the host's pick needs no wire traffic
    // for an observer to reproduce it.
    inline u32 farthestPad(const Vec3* pads, u32 padCount, const Vec3* hostiles, u32 hostileCount) {
        u32 best = 0;
        f32 bestMin = -1.0f;
        for (u32 p = 0; p < padCount; p++) {
            f32 nearest = 1e30f;
            if (hostileCount == 0) nearest = 0.0f;   // all pads equal -> lowest index wins
            for (u32 h = 0; h < hostileCount; h++) {
                f32 dx = pads[p].x - hostiles[h].x;
                f32 dz = pads[p].z - hostiles[h].z;
                f32 dsq = dx * dx + dz * dz;
                if (dsq < nearest) nearest = dsq;
            }
            if (nearest > bestMin) { bestMin = nearest; best = p; }
        }
        return best;
    }
}
