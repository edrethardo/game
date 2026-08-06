#pragma once
// zone_route.h — how you get from where you are to what the act wants next, and what may stop you.
//
// WHY THIS EXISTS. Two features need the same answer and would otherwise each guess at it:
//
//   * QUEST GATING. The acts are a walk, and until now every door on that walk was open — you could
//     stroll past TristRAM to the station without touching a quest, which makes the chain set
//     dressing. Diablo 2's Act 1 gates its road (the Cairn Stones are what open the way onward), and
//     that is the beat this restores.
//   * THE OVERWORLD BOT. Autoplay used to END its run on entering a zone, because the brain has no
//     "descend" objective out here. Giving it one means answering "which world am I trying to reach,
//     and which way is the next hop" — which is the same question the gate asks, from the other side.
//
// Both are pure functions of the zone table, the quest table and a per-character quest mask, so they
// live here rather than in the engine: engine-free means unit-tested, and unit-tested is what keeps a
// gate from ever producing an UNFINISHABLE act. That last risk is not hypothetical — a gate that
// depends on a quest sitting behind that same gate locks the run forever, and nothing in play would
// tell you which of the fifteen links was at fault.
//
// THE GATING RULE, in one sentence: leaving a zone by its ONWARD link requires that zone's own quest
// and its interior's quest to be complete. Everything else follows:
//
//   * "Onward" is DERIVED, not authored — the acts run up the floor numbers along the road, so the
//     onward link is the one to a HIGHER floor. A second authored field would be one more thing to
//     keep in sync with the road, and getting it wrong is silent.
//   * Backtracking is NEVER gated. Walking home, or back out of an interior you have not finished,
//     must always work — a player who wanders in under-levelled has to be able to leave.
//   * The portal INTO an interior is gated on the HOST zone's quest, which is exactly D2's Cairn
//     Stones: clear the Field of Unmerged Branches and the way into TristRAM opens. Where the host
//     has no quest (the Den, the Graveyard, Bank) the portal is simply always open, which is also
//     what those beats want — they ARE the optional side areas.

#include "core/types.h"
#include "game/zone_def.h"
#include "game/quest_def.h"

namespace ZoneRoute {

// How to leave the zone you are standing in.
enum struct HopKind : u8 {
    NONE,     // nowhere to go (already there, or the way is gated shut)
    EDGE,     // walk into a border band — `dir` says which
    PORTAL,   // take the POI mouth / an interior's way back out — an INTERACT, not a walk
    COUNT
};

struct Hop {
    HopKind   kind = HopKind::NONE;
    Zone::Dir dir  = Zone::Dir::COUNT;   // EDGE only
    u8        dest = 0;                  // the floor this hop lands on
};

// "This zone is finished with you": it either hosts no quest, or its quest is done.
inline bool zoneSettled(u8 zoneFloor, u64 questMask) {
    if (zoneFloor == Zone::NO_LINK) return true;
    return Quest::forZone(zoneFloor) == nullptr || Quest::isComplete(questMask, zoneFloor);
}

// Is the link from `from` to `to` passable right now?
//
// Only the ONWARD direction is ever gated. "Onward" means toward a higher floor: the road runs up
// the numbers, and an interior always sits above its host, so a portal inward is onward and a portal
// back out is not.
//
// The two onward cases are DIFFERENT, and conflating them is a locked run. An interior's own quest
// happens INSIDE it, so requiring it to enter puts the key behind its own door — TristRAM and the
// Hellgate would both have been permanently shut. So:
//
//   * PORTAL INWARD, to this zone's interior: gated on the HOST's quest only. That is precisely D2's
//     Cairn Stones — clear the Field of Unmerged Branches and the way into TristRAM opens.
//   * ONWARD ROAD, to the next zone along: gated on the host's quest AND its interior's, because the
//     interior is a detour whose whole point is that quest. Without it the side area is skippable and
//     the chain is decoration again.
inline bool linkOpen(u8 from, u8 to, u64 questMask) {
    if (to <= from) return true;              // home, or back out of an interior — never gated
    if (to == Zone::TOWN_FLOOR) return true;  // the town is 98 and is always reachable

    const Zone::ZoneDef* z = Zone::find(from);
    if (!z) return true;

    if (z->poiFloor == to) return zoneSettled(from, questMask);          // the Cairn Stones rule
    return zoneSettled(from, questMask) && zoneSettled(z->poiFloor, questMask);
}

// Every world directly reachable from `floor`, gates ignored. The zone graph is tiny (15 rows, at
// most 6 exits each) so the callers below can afford to walk it rather than precompute anything.
//
// Returns the count written to `outDest`/`outHop`.
inline u8 exitsOf(u8 floor, u8 outDest[6], Hop outHop[6]) {
    const Zone::ZoneDef* z = Zone::find(floor);
    if (!z) return 0;
    u8 n = 0;
    for (u8 d = 0; d < static_cast<u8>(Zone::Dir::COUNT); d++) {
        const u8 nb = z->neighbour[d];
        if (nb == Zone::NO_LINK) continue;
        outHop[n]  = Hop{HopKind::EDGE, static_cast<Zone::Dir>(d), nb};
        outDest[n] = nb;
        n++;
    }
    if (z->poiFloor != Zone::NO_LINK) {
        outHop[n]  = Hop{HopKind::PORTAL, Zone::Dir::COUNT, z->poiFloor};
        outDest[n] = z->poiFloor;
        n++;
    }
    if (z->returnFloor != Zone::NO_LINK) {
        outHop[n]  = Hop{HopKind::PORTAL, Zone::Dir::COUNT, z->returnFloor};
        outDest[n] = z->returnFloor;
        n++;
    }
    return n;
}

// The zone hosting the first quest still outstanding, in table order — i.e. what the act wants next.
// 0 when both acts are done.
//
// Table ORDER is the act's order; that is already load-bearing (the quest mask is indexed by table
// position), so deriving the objective from it costs no new data and cannot disagree with the save.
inline u8 objectiveZone(u64 questMask) {
    for (u32 i = 0; i < Quest::COUNT; i++)
        if ((questMask & (1ull << i)) == 0) return Quest::QUESTS[i].zoneFloor;
    return 0;
}

// One breadth-first step from `from` toward `goal`, honouring the gates.
//
// BFS rather than "walk toward the higher floor" because the road is not a line: the interiors hang
// off it, so getting from inside TristRAM to the Deadlock Woods means going BACKWARD through a portal
// first. A greedy rule picks the wrong door there, and the bot would stand in a dead end.
inline Hop nextHop(u8 from, u8 goal, u64 questMask) {
    if (from == goal || goal == 0) return Hop{};

    // Reverse BFS is the trick that makes this one pass: flood from the GOAL, then the first hop is
    // whichever exit of `from` lands on the smallest distance. Distances are tiny (the graph is 15
    // nodes) so a flat array indexed by floor-52 is the whole data structure.
    constexpr u8 N = Zone::FLOOR_MAX - Zone::FLOOR_MIN + 1;
    u8 dist[N];
    for (u8 i = 0; i < N; i++) dist[i] = 0xFF;

    u8 queue[N]; u8 head = 0, tail = 0;
    dist[goal - Zone::FLOOR_MIN] = 0;
    queue[tail++] = goal;

    while (head < tail) {
        const u8 cur = queue[head++];
        u8 dests[6]; Hop hops[6];
        const u8 n = exitsOf(cur, dests, hops);
        for (u8 i = 0; i < n; i++) {
            const u8 nb = dests[i];
            if (!Zone::isZoneFloor(nb)) continue;                 // the town terminates the search
            // Walking the graph BACKWARD, so the gate to test is the one on nb -> cur, the direction
            // a player would actually travel. Testing cur -> nb would gate the wrong door and route
            // the bot through a link it cannot use.
            if (!linkOpen(nb, cur, questMask)) continue;
            if (dist[nb - Zone::FLOOR_MIN] != 0xFF) continue;
            dist[nb - Zone::FLOOR_MIN] = static_cast<u8>(dist[cur - Zone::FLOOR_MIN] + 1);
            queue[tail++] = nb;
        }
    }

    u8 dests[6]; Hop hops[6];
    const u8 n = exitsOf(from, dests, hops);
    Hop best{}; u8 bestDist = 0xFF;
    for (u8 i = 0; i < n; i++) {
        const u8 nb = dests[i];
        if (!Zone::isZoneFloor(nb)) continue;
        if (!linkOpen(from, nb, questMask)) continue;
        const u8 d = dist[nb - Zone::FLOOR_MIN];
        if (d < bestDist) { bestDist = d; best = hops[i]; }
    }
    return best;
}

// What the bot should DO while standing in `zoneFloor`: finish the local quest, or move on.
enum struct Task : u8 {
    TRAVEL,      // nothing to do here — take the next hop
    CLEAR_ZONE,  // kill everything
    SLAY,        // kill the named boss
    COUNT
};

// A REACH quest needs no task: it completes on arrival, so by the time anything asks, it is done.
inline Task taskFor(u8 zoneFloor, u64 questMask) {
    const Quest::QuestDef* q = Quest::forZone(zoneFloor);
    if (!q || Quest::isComplete(questMask, zoneFloor)) return Task::TRAVEL;
    switch (q->trigger) {
        case Quest::Trigger::CLEAR_ZONE: return Task::CLEAR_ZONE;
        case Quest::Trigger::SLAY:       return Task::SLAY;
        default:                         return Task::TRAVEL;
    }
}

} // namespace ZoneRoute
