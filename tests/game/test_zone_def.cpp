// test_zone_def.cpp — the overworld zone graph (game/zone_def.h).
//
// These are structural pins, not behaviour tests: the zone table is DATA, and every failure mode it
// has is a data mistake that produces a silently wrong world rather than a crash — a zone id that
// collides with the town, a one-way link that strands a player, a grid size past the minimap's fixed
// 64x64 buffers. Each of those looks completely fine in review.
#include "doctest/doctest.h"
#include "game/zone_def.h"
#include "game/quest_def.h"
#include <cstring>

using namespace Zone;

TEST_CASE("zone floors sit in the free sentinel band") {
    // 1-50 dungeon, 51 cleared marker, 97/98/99 arena/town/Source. 52-96 is what is left.
    CHECK(FLOOR_MIN == 52);
    CHECK(FLOOR_MAX == 96);
    CHECK_FALSE(isZoneFloor(50));
    CHECK_FALSE(isZoneFloor(51));   // the cleared marker — must never route into a zone
    CHECK(isZoneFloor(52));
    CHECK(isZoneFloor(96));
    CHECK_FALSE(isZoneFloor(97));   // arena
    CHECK_FALSE(isZoneFloor(98));   // town
    CHECK_FALSE(isZoneFloor(99));   // Source
    CHECK_FALSE(isZoneFloor(0));

    for (u32 i = 0; i < COUNT; i++)
        CHECK_MESSAGE(isZoneFloor(ZONES[i].floor), "zone out of band: ", ZONES[i].name);
}

TEST_CASE("zone ids are unique and every zone is named") {
    for (u32 i = 0; i < COUNT; i++) {
        REQUIRE(ZONES[i].name != nullptr);
        CHECK(std::strlen(ZONES[i].name) > 0);
        for (u32 j = i + 1; j < COUNT; j++)
            CHECK_MESSAGE(ZONES[i].floor != ZONES[j].floor,
                          "duplicate zone floor ", (u32)ZONES[i].floor);
    }
}

TEST_CASE("every link points somewhere real") {
    for (u32 i = 0; i < COUNT; i++) {
        const ZoneDef& z = ZONES[i];
        for (u8 d = 0; d < static_cast<u8>(Dir::COUNT); d++) {
            const u8 n = z.neighbour[d];
            if (n == NO_LINK) continue;
            // The town is a legal destination without being a zone; anything else must be a zone.
            CHECK_MESSAGE((n == TOWN_FLOOR || find(n) != nullptr),
                          "zone '", z.name, "' links to unknown floor ", (u32)n);
        }
        if (z.poiFloor != NO_LINK)
            CHECK_MESSAGE(find(z.poiFloor) != nullptr, "zone '", z.name, "' has an unknown POI");
        if (z.returnFloor != NO_LINK)
            CHECK_MESSAGE((z.returnFloor == TOWN_FLOOR || find(z.returnFloor) != nullptr),
                          "interior '", z.name, "' returns to an unknown floor");
    }
}

// A one-way edge is the bug that strands a player: they walk north, and the zone they arrive in has
// no south link back. Nothing crashes; they simply cannot return the way they came.
TEST_CASE("zone-to-zone links are reciprocal") {
    for (u32 i = 0; i < COUNT; i++) {
        const ZoneDef& z = ZONES[i];
        for (u8 d = 0; d < static_cast<u8>(Dir::COUNT); d++) {
            const u8 n = z.neighbour[d];
            if (n == NO_LINK || n == TOWN_FLOOR) continue;   // the town's side is enterTown, not a table row
            const ZoneDef* other = find(n);
            REQUIRE(other != nullptr);
            const u8 back = other->neighbour[static_cast<u8>(opposite(static_cast<Dir>(d)))];
            CHECK_MESSAGE(back == z.floor,
                          "one-way link: '", z.name, "' -> '", other->name, "' has no way back");
        }
    }
}

// An interior's return must match the zone that hosts its entrance, or its exit drops the player
// into a world they never came from.
TEST_CASE("POI entrances and returns agree") {
    for (u32 i = 0; i < COUNT; i++) {
        const ZoneDef& z = ZONES[i];
        if (z.poiFloor == NO_LINK) continue;
        const ZoneDef* poi = find(z.poiFloor);
        REQUIRE(poi != nullptr);
        CHECK_MESSAGE(poi->returnFloor == z.floor,
                      "'", poi->name, "' does not return to the zone holding its entrance");
    }
}

TEST_CASE("opposite() pairs the compass") {
    CHECK(opposite(Dir::NORTH) == Dir::SOUTH);
    CHECK(opposite(Dir::SOUTH) == Dir::NORTH);
    CHECK(opposite(Dir::EAST)  == Dir::WEST);
    CHECK(opposite(Dir::WEST)  == Dir::EAST);
    for (u8 d = 0; d < static_cast<u8>(Dir::COUNT); d++)
        CHECK(opposite(opposite(static_cast<Dir>(d))) == static_cast<Dir>(d));
}

TEST_CASE("arrivalEdge puts you on the far side, and refuses unlinked pairs") {
    // The Blood Buffer's south edge leads to town, so walking south should arrive at the NORTH edge.
    Dir edge;
    REQUIRE(arrivalEdge(52, TOWN_FLOOR, edge));
    CHECK(edge == Dir::NORTH);

    // An unlinked pair must be refused rather than guess an edge — the caller treats false as
    // "refuse the transition", which is the only safe answer for a corrupt or hostile floor byte.
    CHECK_FALSE(arrivalEdge(52, 96, edge));
    CHECK_FALSE(arrivalEdge(200, TOWN_FLOOR, edge));
}

TEST_CASE("find() rejects non-zone floors") {
    CHECK(find(52) != nullptr);
    CHECK(find(TOWN_FLOOR) == nullptr);   // the town is an anchor, not a zone
    CHECK(find(1) == nullptr);
    CHECK(find(0) == nullptr);
    CHECK(find(255) == nullptr);
}

// The caps that bind an outdoor world do not announce themselves: the minimap's visited/pixel arrays
// are fixed 64x64 statics that truncate silently, and the spatial grid projectile collision uses
// spans only +/-128 m. A zone authored past those looks fine and plays wrong.
TEST_CASE("zone grids stay inside the engine's fixed-buffer limits") {
    for (u32 i = 0; i < COUNT; i++) {
        CHECK_MESSAGE(ZONES[i].gridSize <= 64,
                      "'", ZONES[i].name, "' exceeds the 64x64 minimap buffers");
        CHECK_MESSAGE(ZONES[i].gridSize >= 24, "'", ZONES[i].name, "' is implausibly small");
    }
}

// Act 1 must be WALKABLE end to end from the town, or a zone exists that no player can reach. The
// table is data, so this is the only thing standing between a typo'd link and an orphaned area.
TEST_CASE("Act 1 is reachable from town, and ends at the Terminal") {
    // Walk the graph from the town's first zone and collect everything reachable by edges + POIs.
    bool seen[COUNT] = {};
    u8   queue[COUNT + 1];
    u32  head = 0, tail = 0;
    queue[tail++] = 52;                       // the zone the town's north gate opens onto
    while (head < tail) {
        const u8 cur = queue[head++];
        const ZoneDef* z = find(cur);
        REQUIRE(z != nullptr);
        for (u32 i = 0; i < COUNT; i++) if (ZONES[i].floor == cur) seen[i] = true;
        const auto push = [&](u8 f) {
            if (f == NO_LINK || f == TOWN_FLOOR || !find(f)) return;
            for (u32 i = 0; i < COUNT; i++)
                if (ZONES[i].floor == f) { if (seen[i]) return; break; }
            for (u32 q = 0; q < tail; q++) if (queue[q] == f) return;
            queue[tail++] = f;
        };
        for (u8 d = 0; d < static_cast<u8>(Dir::COUNT); d++) push(z->neighbour[d]);
        push(z->poiFloor);
    }
    for (u32 i = 0; i < COUNT; i++)
        CHECK_MESSAGE(seen[i], "unreachable zone: '", ZONES[i].name, "'");

    // The act terminates at the Tube station: its north link is deliberately NO_LINK until the
    // London arc exists, so Act 1 closes rather than opening onto an empty world.
    const ZoneDef* terminal = find(59);
    REQUIRE(terminal != nullptr);
    CHECK(terminal->neighbour[(u8)Dir::NORTH] == NO_LINK);
    CHECK(terminal->neighbour[(u8)Dir::SOUTH] == 58);
}

// Waypoints are the act's fast-travel spine. Too few and the walk back is punishing; every zone and
// they stop meaning anything. D2's Act 1 gives you roughly one every other area.
TEST_CASE("Act 1 waypoint spacing is sane") {
    u32 wp = 0, outdoor = 0;
    for (u32 i = 0; i < COUNT; i++) {
        if (ZONES[i].hasWaypoint) wp++;
        if (ZONES[i].returnFloor == NO_LINK) outdoor++;   // not an interior
    }
    CHECK(wp >= 3);
    CHECK(wp <= outdoor);            // an interior must never carry one
    for (u32 i = 0; i < COUNT; i++)
        if (ZONES[i].returnFloor != NO_LINK)
            CHECK_MESSAGE(!ZONES[i].hasWaypoint, "interior '", ZONES[i].name, "' has a waypoint");
    // The mask is a u64 indexed by table position — the table cannot outgrow it.
    CHECK(COUNT <= 64);
}

TEST_CASE("slice 1 ships the Blood Buffer and the Den, and the Den is an interior") {
    const ZoneDef* buffer = find(52);
    const ZoneDef* den    = find(53);
    REQUIRE(buffer != nullptr);
    REQUIRE(den != nullptr);
    // The Blood Buffer deliberately has NO waypoint, matching D2's Blood Moor: the first walk out of
    // town is the tutorial, and handing you fast travel before you have walked anywhere undercuts it.
    CHECK_FALSE(buffer->hasWaypoint);
    CHECK(buffer->neighbour[(u8)Dir::SOUTH] == TOWN_FLOOR);
    CHECK(buffer->poiFloor == den->floor);
    CHECK(den->returnFloor == buffer->floor);
    for (u8 d = 0; d < (u8)Dir::COUNT; d++)
        CHECK(den->neighbour[d] == NO_LINK);            // an interior has no open edges
}

// --- Act 1 quest chain (game/quest_def.h) --------------------------------------------------------
// The quest table is data hanging off the zone table, so the failure modes are the same class: a
// quest pointing at a zone that does not exist, a SLAY objective naming an enemy that was renamed,
// or two quests fighting over one zone. None of those crash; they just never complete.
TEST_CASE("every quest hangs off a real zone, one per zone") {
    for (u32 i = 0; i < Quest::COUNT; i++) {
        const Quest::QuestDef& q = Quest::QUESTS[i];
        CHECK_MESSAGE(find(q.zoneFloor) != nullptr,
                      "quest '", q.name, "' names a floor that is not a zone");
        REQUIRE(q.name != nullptr);
        REQUIRE(q.blurb != nullptr);
        CHECK(std::strlen(q.name) > 0);
        CHECK(std::strlen(q.blurb) > 0);
        for (u32 j = i + 1; j < Quest::COUNT; j++)
            CHECK_MESSAGE(Quest::QUESTS[j].zoneFloor != q.zoneFloor,
                          "two quests share zone ", (u32)q.zoneFloor);
        // An objective whose target the trigger USES must name something, or it can never
        // complete — the silent failure this pins. SLAY names an enemy, ACTIVATE a fixture tag;
        // every other trigger must leave it empty rather than carry a stale string.
        REQUIRE(q.narration != nullptr);
        CHECK(std::strlen(q.narration) > 0);
        for (u32 o = 0; o < q.objectiveCount; o++) {
            const Quest::ObjectiveDef& od = q.objectives[o];
            // Non-null BEFORE strlen, and it is the real lint: an objectiveCount larger than the
            // number of objectives actually authored leaves value-initialised slots whose strings
            // are null, which is exactly the authoring slip that would read as a missing journal
            // row rather than as an error.
            REQUIRE(od.text != nullptr);
            REQUIRE(od.target != nullptr);
            REQUIRE(od.required >= 1);
            CAPTURE(od.text);
            if (od.trigger == Quest::Trigger::SLAY || od.trigger == Quest::Trigger::ACTIVATE)
                CHECK_MESSAGE(std::strlen(od.target) > 0, "quest '", q.name, "' has an untargeted objective");
            else
                CHECK(std::strlen(od.target) == 0);
        }
    }
    CHECK(Quest::COUNT <= 64);   // the completion mask is a u64
}

TEST_CASE("quest completion bits round-trip and gate the act") {
    u64 mask = 0;
    CHECK_FALSE(Quest::actComplete(mask, 1));
    CHECK_FALSE(Quest::actComplete(mask, 2));
    for (u32 i = 0; i < Quest::COUNT; i++) {
        const u8 f = Quest::QUESTS[i].zoneFloor;
        CHECK_FALSE(Quest::isComplete(mask, f));
        mask |= (1ull << Quest::indexForZone(f));
        CHECK(Quest::isComplete(mask, f));
    }
    CHECK(Quest::actComplete(mask, 1));
    CHECK(Quest::actComplete(mask, 2));
    // A zone with no quest is never "complete" — it must not read as a satisfied objective.
    CHECK_FALSE(Quest::isComplete(mask, 52));
    CHECK(Quest::indexForZone(52) == 0xFF);
}

// Act 1 must END on a quest, or finishing the walk has no payoff. The act's CLIMAX is the
// TristRAM boss and its EPILOGUE is the tube mouth — two different beats, so both are pinned:
// a climax that is merely a REACH is an anticlimax, and a doorway that carries no quest at all
// leaves the walk to the Underground unmotivated.
TEST_CASE("the act ends on a boss fight, then a doorway") {
    const Quest::QuestDef* climax = Quest::forZone(57);   // TristRAM
    REQUIRE(climax != nullptr);
    REQUIRE(Quest::deedObjective(*climax) != nullptr);
    CHECK(Quest::deedObjective(*climax)->trigger == Quest::Trigger::SLAY);

    const Quest::QuestDef* last = Quest::forZone(59);     // Whitechapel Terminal
    REQUIRE(last != nullptr);
    REQUIRE(Quest::deedObjective(*last) != nullptr);
    CHECK(Quest::deedObjective(*last)->trigger == Quest::Trigger::REACH);
}

// A SLAY quest names its victim as a STRING and the zone that hosts the fight names its boss as
// another STRING; nothing in the type system makes the two agree. Rename the enemy in enemies.json
// and update only one of them and the quest becomes uncompletable — the zone spawns a boss whose
// death never satisfies the objective, which reads in-game as "I killed it and nothing happened".
// So every zone boss must be the target of its zone's quest, and vice versa.
TEST_CASE("a zone boss and its SLAY quest name the same enemy") {
    for (u32 i = 0; i < Zone::COUNT; i++) {
        const Zone::ZoneDef& z = Zone::ZONES[i];
        const Quest::QuestDef* q = Quest::forZone(z.floor);
        const bool hasBoss = z.boss && z.boss[0];
        const Quest::ObjectiveDef* deed = q ? Quest::deedObjective(*q) : nullptr;
        if (hasBoss) {
            REQUIRE(q != nullptr);
            REQUIRE(deed != nullptr);
            CHECK(deed->trigger == Quest::Trigger::SLAY);
            CHECK(std::strcmp(deed->target, z.boss) == 0);
        }
        // The converse: a SLAY quest whose target is a zone BOSS must be hosted by the zone that
        // spawns it. (A SLAY may also target a champion the content spawner seeds, which is why
        // this only fires when the zone declares a boss of its own.)
        if (deed && deed->trigger == Quest::Trigger::SLAY && hasBoss)
            CHECK(std::strcmp(deed->target, z.boss) == 0);
    }
}

// Every link in the graph, walked both ways ------------------------------------------------------
//
// The spot-check above covers one pair. This walks the WHOLE table, because the overworld's bugs
// have all been in the arrival path — the ping-pong (arrival landing on the trigger threshold) and
// the out-of-bounds respawn (arrival landing past the cleared ground) were both "you got to the
// next zone, but where you were put down was wrong". Data-level cover for that is cheap; the live
// walk-test is not.
TEST_CASE("every edge link resolves to the opposite side, from both ends") {
    using namespace Zone;
    u32 checked = 0;
    for (u32 i = 0; i < COUNT; i++) {
        const ZoneDef& z = ZONES[i];
        for (u8 d = 0; d < static_cast<u8>(Dir::COUNT); d++) {
            const u8 dest = z.neighbour[d];
            if (dest == NO_LINK || dest == TOWN_FLOOR) continue;
            CAPTURE(z.name); CAPTURE(dest);

            // Walking out of `z` through edge d must put us on the OPPOSITE edge of `dest`.
            Dir arrive = Dir::COUNT;
            REQUIRE(arrivalEdge(z.floor, dest, arrive));
            CHECK(arrive == opposite(static_cast<Dir>(d)));

            // …and the neighbour must name us back through that same edge, or one direction of the
            // border is a one-way door.
            const ZoneDef* back = find(dest);
            REQUIRE(back != nullptr);
            CHECK(back->neighbour[static_cast<u8>(arrive)] == z.floor);
            checked++;
        }
    }
    CHECK(checked >= 12);   // the act chains; a table that stopped linking would pass vacuously
}

// A POI gate and its return gate must name each other. A one-way den is a room you cannot leave.
TEST_CASE("every POI gate has a matching return gate") {
    using namespace Zone;
    u32 pairs = 0;
    for (u32 i = 0; i < COUNT; i++) {
        const ZoneDef& z = ZONES[i];
        if (z.poiFloor == NO_LINK) continue;
        CAPTURE(z.name); CAPTURE(z.poiFloor);
        const ZoneDef* inner = find(z.poiFloor);
        REQUIRE(inner != nullptr);
        CHECK(inner->returnFloor == z.floor);
        pairs++;
    }
    CHECK(pairs >= 3);      // the two Act 1 dens and Bank Station
}

// ---------------------------------------------------------------------------------------------
// RESPAWN CLEARANCE — "respawning teleports me from zone to zone".
//
// spawnPosition is both where a zone entry PUTS you and where a death RETURNS you, and those are
// different questions. Arriving wants continuity of travel, so an edge crossing lands you in the
// gate you walked through: EDGE_ARRIVE_INSET (5 m) from a border whose transition band starts at
// 2.5 m. Fine to walk out of, and a terrible place to be dropped by a death — you come back in the
// doorway, take two steps the wrong way under whatever killed you, cross back, and land at the
// neighbour's gate 5 m from the same seam, which bounces you again.
//
// A death now returns you to the zone's own INTERIOR arrival point. These pin the geometry that
// makes that safe, because the margin is what the bug was about — not the mechanism.
// ---------------------------------------------------------------------------------------------

TEST_CASE("an arriving player lands clear of the band that would send them back") {
    // The ping-pong bug in its original form: these two were equal, so arrival satisfied the
    // trigger on its first tick and the world rebuilt 52 times a second.
    CHECK(Zone::EDGE_ARRIVE_INSET > Zone::EDGE_TRIGGER_BAND);
}

TEST_CASE("every zone's RESPAWN anchor clears the transition band by a wide margin") {
    // Wide, not merely positive. A respawn is involuntary and usually happens with enemies on top
    // of you, so "a couple of steps from a world change" is exactly what must not be true. The gate
    // arrival's own margin is EDGE_ARRIVE_INSET - EDGE_TRIGGER_BAND = 2.5 m, which is what this
    // deliberately beats.
    constexpr f32 MIN_MARGIN = 5.0f;
    // NOTE: this used to also assert that the GATE arrival's own margin fell short of MIN_MARGIN,
    // as a way of showing the respawn point beat it. That stopped being true when EDGE_ARRIVE_INSET
    // was widened x2 -> x4 (combat drift was bouncing bots back through the gate they arrived by),
    // and an assertion that only holds because a neighbouring constant is small is a trap. What
    // still matters — and is what this pins — is that a respawn is not in a doorway at all.

    for (u32 i = 0; i < Zone::COUNT; i++) {
        const Zone::ZoneDef& z = Zone::ZONES[i];
        CAPTURE(z.name);
        CHECK(z.gridSize >= Zone::MIN_GRID_SIZE);            // the clearance is derived from this
        CHECK(Zone::respawnBandClearance(z.gridSize) >= MIN_MARGIN);
    }
}

TEST_CASE("the respawn anchor stays inside the world it belongs to") {
    // The anchor is offset SOUTH of centre; on a small enough grid that walks off the map, which
    // would put a death outside the world — the failure this whole pass exists to remove.
    for (u32 i = 0; i < Zone::COUNT; i++) {
        const Zone::ZoneDef& z = Zone::ZONES[i];
        CAPTURE(z.name);
        const f32 anchorZ = static_cast<f32>(z.gridSize) * 0.5f
                          + Zone::RETURN_GATE_OFFSET + Zone::ARRIVAL_BACKOFF;
        CHECK(anchorZ > 1.0f);                                   // not in the border ring
        CHECK(anchorZ < static_cast<f32>(z.gridSize) - 1.0f);
    }
}

// Every interior's doorway is modelled as what it actually is, and reads the same from BOTH sides.
//
// The symmetry is the load-bearing half. An entrance is one object seen from two worlds — the Den's
// mouth out in the Blood Buffer and the way back out seen from inside the Den are the same hole in
// the same rock — and keying on the destination alone is what once dressed the Den's exit as an
// orange standing stone floating in a cave. Anything that picks a MESH or a MAP GLYPH goes through
// entranceFor, so this pins both consumers at once.
TEST_CASE("zone entrances are modelled per interior and symmetric") {
    struct Pair { u8 outside, interior; Zone::Entrance kind; const char* what; };
    const Pair PAIRS[] = {
        { 52, 53, Zone::Entrance::CAVE,     "the Den of Evil is a hole in a rock" },
        { 56, 57, Zone::Entrance::STONES,   "TristRAM is reached through the Cairn Stones" },
        { 65, 66, Zone::Entrance::HELLGATE, "Hellgate: Localhost is a forced rift" },
        { 54, 55, Zone::Entrance::GRAVE,    "the Deprecated Graveyard is behind a cemetery gate" },
        { 59, 60, Zone::Entrance::TUBE,     "Act 2 is down an Underground stair" },
        { 62, 64, Zone::Entrance::DOOR,     "Bank is through a staff maintenance door" },
    };
    for (const Pair& p : PAIRS) {
        CAPTURE(p.what);
        CHECK(Zone::entranceFor(p.outside, p.interior) == p.kind);   // walking in
        CHECK(Zone::entranceFor(p.interior, p.outside) == p.kind);   // and back out
    }

    // The old narrow predicate still answers for the cave and ONLY the cave — the stone circle and
    // the rift must not inherit the cave mouth's mesh just because they are also interiors.
    CHECK(Zone::isCaveBoundary(52, 53));
    CHECK(Zone::isCaveBoundary(53, 52));
    CHECK_FALSE(Zone::isCaveBoundary(56, 57));
    CHECK_FALSE(Zone::isCaveBoundary(65, 66));

    // A road-to-road border is not an entrance at all: it is a walk, and dressing it as a doorway
    // would put a monument in the middle of an open field.
    CHECK(Zone::entranceFor(52, 54) == Zone::Entrance::STONE);
}

// EVERY interior has a modelled entrance. An interior that forgets to author one still WORKS — it
// falls back to the plain standing stone — which is exactly why this needs a test rather than a
// review: the failure is a silent downgrade to the generic marker, in a corner of a zone nobody
// walks to twice.
TEST_CASE("every overworld interior authors a modelled entrance") {
    u32 interiors = 0;
    for (const Zone::ZoneDef& z : Zone::ZONES) {
        if (z.returnFloor == 0) continue;          // a road zone, not an interior
        interiors++;
        CAPTURE(z.name);
        CHECK(z.entrance != Zone::Entrance::STONE);
        CHECK(z.entrance < Zone::Entrance::COUNT);
    }
    CHECK(interiors == 6);   // Den, Graveyard, TristRAM, Act 2, Bank, the Hellgate
}
