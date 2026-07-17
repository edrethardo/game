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
    constexpr u32 KILL_TARGET    = 10;     // first to 10 wins (v1 fixed; lobby config parked)
    constexpr f32 RESPAWN_DELAY  = 3.0f;   // seconds dead before the auto-respawn

    struct Score { u16 kills[MAX_COMBATANTS] = {}; };

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
