// zone_def.h — the OVERWORLD zone graph: which outdoor areas exist, how they connect, and what
// fixtures each one guarantees.
//
// A "zone" is an ordinary level that happens to live on a SENTINEL FLOOR (52-96) instead of on the
// numbered 1-50 ladder. That choice is the whole architecture: the floor byte is already the world's
// identity everywhere it matters — `SV_LEVEL_SEED` carries it to clients, the save header stores it,
// and `Engine::onLevelSeed` routes on it — so an overworld of connected areas costs NO protocol change
// and NO new save field. Floors 1-50 are the dungeon, 51 is the cleared marker, 97/98/99 are
// arena/town/Source; 52-96 were simply unused.
//
// Header-only and engine-free so it unit-tests without a GL/engine context, in the style of
// free_play.h and shrine.h. Everything here is data plus pure lookups; the ENTRY behaviour lives in
// engine_world.cpp.
#pragma once

#include "core/types.h"

namespace Zone {

// The sentinel band. Deliberately checked rather than assumed: a zone id that collided with the town
// (98) or an ordinary floor would route a player into the wrong world with no error at all.
inline constexpr u8 FLOOR_MIN = 52;
inline constexpr u8 FLOOR_MAX = 96;

inline bool isZoneFloor(u8 floor) { return floor >= FLOOR_MIN && floor <= FLOOR_MAX; }

// Compass edges. Order is load-bearing: `opposite()` is index^2, and the transition code uses it to
// place a player on the FAR side of the zone they walk into (leave north, arrive at the south edge).
enum struct Dir : u8 { NORTH = 0, EAST = 1, SOUTH = 2, WEST = 3, COUNT = 4 };
inline Dir opposite(Dir d) { return static_cast<Dir>(static_cast<u8>(d) ^ 2u); }

// 0 is "no link" rather than a valid floor, which is why the sentinel band starts at 52 and not 0.
inline constexpr u8 NO_LINK = 0;

// Which generator dresses a zone. Kept as a small enum of INTENTS rather than a raw LayoutStyle so
// zone_def.h stays engine-free (level_gen.h pulls in the grid); engine_zone.cpp maps intent -> style.
//   OPEN_COUNTRY -> WILDERNESS  (Act 1: sky, scattered clumps)
//   CAVE         -> CAVERN      (Act 1 interiors: the Den)
//   TUNNEL       -> GAUNTLET    (Act 2: a serpentine chain of platforms — a tube line IS a gauntlet)
//   STATION      -> HUB         (Act 2: a concourse with passages off it)
enum struct Terrain : u8 { OPEN_COUNTRY, CAVE, TUNNEL, STATION, COUNT };

struct ZoneDef {
    u8          floor;              // this zone's sentinel floor (52-96) — its identity everywhere
    const char* name;               // player-facing, also the waypoint list label
    u8          neighbour[4];       // N/E/S/W -> floor of the adjacent world, or NO_LINK for a wall
    u8          poiFloor;           // an interior reachable from INSIDE this zone (0 = none)
    u8          returnFloor;        // for an interior: where its exit leads back to (0 = none)
    bool        hasWaypoint;        // does this zone carry a waypoint fixture
    bool        peaceful;           // drives EnemyAI::setTownMode — no hostile spawns
    bool        ruins;              // dress as a burnt-out settlement: hut shells, ash, blood-dark ground
    const char* boss;               // a single named enemy spawned at the zone's heart ("" = none)
    Terrain     terrain;            // which generator builds it
    bool        underground;        // keep the ceiling: no sky, no daylight clear colour
    u8          gridSize;           // square grid edge; kept <= 64 (see the minimap/spatial-grid caps)
};

// SLICE 1. Two worlds beyond the town, which is the minimum that proves the architecture: an outdoor
// zone reached by walking, and an interior reached from inside it.
//
// The town (98) is NOT a zone — it is the anchor the graph hangs off. A zone may name it as a
// neighbour (the Blood Buffer's south edge leads home) but it has no ZoneDef of its own; entering it
// goes through enterTown as it always has.
//
// Grid sizes stay at 52 and below on purpose. The minimap's visited/pixel buffers are fixed 64x64
// statics that TRUNCATE SILENTLY past that, the spatial grid that projectile collision uses spans
// only +/-128 m, and the cavern generator bails to BSP above 64x64. None of those announce
// themselves — the world would simply be subtly wrong.
inline constexpr u8 TOWN_FLOOR = 98;   // GameConst::TOWN_SENTINEL_FLOOR, duplicated to stay engine-free

// ACT 1 — "The Blood Buffer to Whitechapel". A Diablo 2 Act 1 homage in this game's own voice: the
// route runs town -> open country -> a ruined settlement -> the gate that should be a monastery and
// is instead a boarded London Underground station, which is where the next arc begins.
//
// The parody register is the one the game already established. It ships The Butcher (D2's own Act 1
// boss) played completely straight, and DiaBRO on floor 40 played completely not — so the names here
// land the joke in the WORD while the place itself is honest dark fantasy. The running gag is that
// the dungeon is software and always has been: you are walking through a program's memory.
//
// The ORIGINAL SPIN: these are not the ruins of a fantasy kingdom, they are the ruins of a BUILD.
// TristRAM is a village that has been rebuilt from backups so many times the villagers no longer
// trust that they are the originals. The Den is a franchise. The moor is a buffer that overflowed.
// That is the throughline — Act 1 of Diablo 2 is a countryside falling to corruption; here it is a
// system falling over, and the monsters are what leaked out.
inline constexpr ZoneDef ZONES[] = {
    // 1. The open country north of town. D2's Blood Moor; here, the memory the corruption spilled
    //    into. First zone, first waypoint, and the Den's mouth sits in it.
    { /*floor*/      52,
      /*name*/       "The Blood Buffer",
      /*neighbour*/  { 54, NO_LINK, TOWN_FLOOR, NO_LINK },   // north -> Cold Storage, south -> town
      /*poiFloor*/   53,
      /*returnFloor*/0,
      /*hasWaypoint*/false,        // D2's Blood Moor has none either — the first walk is the tutorial
      /*peaceful*/   false,
      /*ruins*/      false,
      /*boss*/       "",
      /*terrain*/    Terrain::OPEN_COUNTRY,
      /*underground*/false,
      /*gridSize*/   52 },

    // 2. The starter cave. D2's Den of Evil, rebranded as what a dungeon becomes once it is a going
    //    concern. No waypoint — the Buffer's is a short walk, exactly as in D2.
    { /*floor*/      53,
      /*name*/       "The Den of Evil (Franchise Location #2)",
      /*neighbour*/  { NO_LINK, NO_LINK, NO_LINK, NO_LINK },
      /*poiFloor*/   0,
      /*returnFloor*/52,
      /*hasWaypoint*/false,
      /*peaceful*/   false,
      /*ruins*/      false,
      /*boss*/       "",
      /*terrain*/    Terrain::CAVE,
      /*underground*/true,
      /*gridSize*/   44 },

    // 3. D2's Cold Plains. Cold storage: the place data goes when nobody wants to delete it but
    //    nobody wants to look at it either. Second waypoint, and the burial ground hangs off it.
    { /*floor*/      54,
      /*name*/       "Cold Storage",
      /*neighbour*/  { 56, NO_LINK, 52, NO_LINK },
      /*poiFloor*/   55,
      /*returnFloor*/0,
      /*hasWaypoint*/true,
      /*peaceful*/   false,
      /*ruins*/      false,
      /*boss*/       "",
      /*terrain*/    Terrain::OPEN_COUNTRY,
      /*underground*/false,
      /*gridSize*/   52 },

    // 4. D2's Burial Grounds / Blood Raven. Deprecated, not deleted — and still running.
    { /*floor*/      55,
      /*name*/       "The Deprecated Graveyard",
      /*neighbour*/  { NO_LINK, NO_LINK, NO_LINK, NO_LINK },
      /*poiFloor*/   0,
      /*returnFloor*/54,
      /*hasWaypoint*/false,
      /*peaceful*/   false,
      /*ruins*/      true,          // a graveyard is a ruin too — broken markers, dead ground
      /*boss*/       "The Garbage Collector",   // D2's Blood Raven, and the same joke twice over: it reclaims the dead, and it will not stop
      /*terrain*/    Terrain::OPEN_COUNTRY,
      /*underground*/false,
      /*gridSize*/   44 },

    // 5. D2's Stony Field. A field of standing stones that are, on inspection, monuments to
    //    abandoned features. The waypoint here is the halfway anchor of the act.
    { /*floor*/      56,
      /*name*/       "The Field of Unmerged Branches",
      /*neighbour*/  { 57, NO_LINK, 54, NO_LINK },
      /*poiFloor*/   0,
      /*returnFloor*/0,
      /*hasWaypoint*/true,
      /*peaceful*/   false,
      /*ruins*/      false,
      /*boss*/       "",
      /*terrain*/    Terrain::OPEN_COUNTRY,
      /*underground*/false,
      /*gridSize*/   52 },

    // 6. TristRAM. The set piece, and OVERRUN — as in D2, where the village you come to save is
    //    already lost. This is where ACT 1 PEAKS: its smith is still here, and he is what the
    //    village's endless restoring finally produced. A village restored from backup so many times the villagers stopped being sure
    //    they were the originals; now something else is running in it. It keeps its waypoint (you
    //    will want to leave in a hurry) but it is desolate, hostile ground, not a refuge.
    { /*floor*/      57,
      /*name*/       "TristRAM (pop. 3, mostly)",
      /*neighbour*/  { 58, NO_LINK, 56, NO_LINK },
      /*poiFloor*/   0,
      /*returnFloor*/0,
      /*hasWaypoint*/false,        // D2's Tristram has no waypoint — you arrive by portal and leave fast
      /*peaceful*/   false,         // OVERRUN, like D2's: the village is the ambush, not the rest stop
      /*ruins*/      true,
      /*boss*/       "Griswald, the Unfinished Build",   // ACT 1's FINAL BOSS — see the note above
      /*terrain*/    Terrain::OPEN_COUNTRY,
      /*underground*/false,
      /*gridSize*/   44 },

    // 7. D2's Dark Wood / Black Marsh, compressed into one approach zone. The last stretch of open
    //    country before the gate; no waypoint, so the walk in from TristRAM stays a walk.
    { /*floor*/      58,
      /*name*/       "The Deadlock Woods",
      /*neighbour*/  { 59, NO_LINK, 57, NO_LINK },
      /*poiFloor*/   0,
      /*returnFloor*/0,
      /*hasWaypoint*/true,         // D2's Dark Wood carries one; the long walk back needs it
      /*peaceful*/   false,
      /*ruins*/      false,
      /*boss*/       "",
      /*terrain*/    Terrain::OPEN_COUNTRY,
      /*underground*/false,
      /*gridSize*/   52 },

    // 8. THE GATE. D2 puts the Rogue Monastery here; this puts a boarded Underground station, and
    //    the arc beyond it is the Hellgate London parody. "Terminal" is the joke doing double duty:
    //    a tube terminus and the thing you type into. Its POI is the way DOWN — Act 2 begins on the
    //    escalator, not through the north wall, because you descend into the Underground.
    { /*floor*/      59,
      /*name*/       "Whitechapel Terminal",
      /*neighbour*/  { NO_LINK, NO_LINK, 58, NO_LINK },
      /*poiFloor*/   60,           // down the escalator: ACT 2
      /*returnFloor*/0,
      /*hasWaypoint*/true,
      /*peaceful*/   false,
      /*ruins*/      true,          // the station forecourt is rubble; the tube mouth is the only intact thing
      /*boss*/       "",
      /*terrain*/    Terrain::OPEN_COUNTRY,
      /*underground*/false,
      /*gridSize*/   52 },

    // ================================ ACT 2 — "HELLGATE: LOCALHOST" ================================
    // The Hellgate London parody. London fell; the survivors went underground and live in the
    // stations, and the tunnels between them belong to whatever came through the rift. Everything is
    // roofed — after an entire act of open sky, the ceiling coming down IS the tonal shift.
    //
    // The joke register stays the house one. London's real station and street names are already
    // half-programming puns if you squint (Bank, Threadneedle Street, the Circle Line is a literal
    // loop, a terminus is a terminal), so the parody mostly just has to notice.

    // 9. The descent. A dead escalator shaft and the first stretch of running tunnel.
    { /*floor*/      60,
      /*name*/       "The Northbound Stack",
      /*neighbour*/  { 61, NO_LINK, NO_LINK, NO_LINK },
      /*poiFloor*/   0,
      /*returnFloor*/59,           // back up the escalator into Act 1
      /*hasWaypoint*/false,
      /*peaceful*/   false,
      /*ruins*/      false,
      /*boss*/       "",
      /*terrain*/    Terrain::TUNNEL,
      /*underground*/true,
      /*gridSize*/   48 },

    // 10. The survivor hub — Act 2's town. The last station with the lights still on, and the only
    //     peaceful ground down here. A null terminus: the end of the string, where nothing follows.
    { /*floor*/      61,
      /*name*/       "Null Terminus",
      /*neighbour*/  { 62, NO_LINK, 60, NO_LINK },
      /*poiFloor*/   0,
      /*returnFloor*/0,
      /*hasWaypoint*/true,
      /*peaceful*/   true,         // the one safe platform in the act
      /*ruins*/      false,
      /*boss*/       "",
      /*terrain*/    Terrain::STATION,
      /*underground*/true,
      /*gridSize*/   44 },

    // 11. The Circle Line, which in London genuinely is a loop that never terminates.
    { /*floor*/      62,
      /*name*/       "The Circle Line (Infinite Loop)",
      /*neighbour*/  { 63, NO_LINK, 61, NO_LINK },
      /*poiFloor*/   64,           // a maintenance door into Bank
      /*returnFloor*/0,
      /*hasWaypoint*/true,
      /*peaceful*/   false,
      /*ruins*/      false,
      /*boss*/       "",
      /*terrain*/    Terrain::TUNNEL,
      /*underground*/true,
      /*gridSize*/   52 },

    // 12. Surface, briefly — a street of gutted banking halls under a sky you cannot see for smoke.
    //     Threadneedle Street is a real address in the City and an unimprovable programming pun.
    { /*floor*/      63,
      /*name*/       "Threadneedle Street (Unsafe)",
      /*neighbour*/  { 65, NO_LINK, 62, NO_LINK },
      /*poiFloor*/   0,
      /*returnFloor*/0,
      /*hasWaypoint*/true,
      /*peaceful*/   false,
      /*ruins*/      true,         // the City after it fell
      /*boss*/       "",
      /*terrain*/    Terrain::OPEN_COUNTRY,
      /*underground*/false,        // the one breath of surface in the act
      /*gridSize*/   52 },

    // 13. Bank. Deep-level interchange, structurally a warren, and overdrawn in every sense.
    { /*floor*/      64,
      /*name*/       "Bank Station (Overdrawn)",
      /*neighbour*/  { NO_LINK, NO_LINK, NO_LINK, NO_LINK },
      /*poiFloor*/   0,
      /*returnFloor*/62,
      /*hasWaypoint*/false,
      /*peaceful*/   false,
      /*ruins*/      false,
      /*boss*/       "The Perpetual Commuter",   // has been riding since before the gate opened and has never once reached a destination
      /*terrain*/    Terrain::STATION,
      /*underground*/true,
      /*gridSize*/   44 },

    // 14. Piccadilly Circus: too many lines meeting in too little space, which is what an overflow is.
    { /*floor*/      65,
      /*name*/       "Piccadilly Circus (Buffer Overflow)",
      /*neighbour*/  { 66, NO_LINK, 63, NO_LINK },
      /*poiFloor*/   0,
      /*returnFloor*/0,
      /*hasWaypoint*/true,
      /*peaceful*/   false,
      /*ruins*/      false,
      /*boss*/       "",
      /*terrain*/    Terrain::STATION,
      /*underground*/true,
      /*gridSize*/   52 },

    // 15. THE RIFT. Act 2 ends where the gate itself is — running on this machine, as it turns out.
    //     North stays closed until there is an Act 3, so the act closes cleanly.
    { /*floor*/      66,
      /*name*/       "Hellgate: Localhost",
      /*neighbour*/  { NO_LINK, NO_LINK, 65, NO_LINK },
      /*poiFloor*/   0,
      /*returnFloor*/0,
      /*hasWaypoint*/true,
      /*peaceful*/   false,
      /*ruins*/      true,
      /*boss*/       "Signal Failure",   // ACT 2's FINAL BOSS — the thing holding the gate open, announced as a service disruption
      /*terrain*/    Terrain::STATION,
      /*underground*/true,
      /*gridSize*/   52 },
};

inline constexpr u32 COUNT = sizeof(ZONES) / sizeof(ZONES[0]);

// The zone on `floor`, or nullptr if that floor is not a zone. Callers MUST handle nullptr: a
// corrupt save or a hostile packet can carry any byte, and routing an unknown floor into the zone
// path would build an empty world.
inline const ZoneDef* find(u8 floor) {
    for (u32 i = 0; i < COUNT; i++)
        if (ZONES[i].floor == floor) return &ZONES[i];
    return nullptr;
}

// The zone reached by leaving `from` through `dir`, or NO_LINK. TOWN_FLOOR is a legal answer.
inline u8 neighbourOf(u8 from, Dir dir) {
    const ZoneDef* z = find(from);
    if (!z || dir >= Dir::COUNT) return NO_LINK;
    return z->neighbour[static_cast<u8>(dir)];
}

// Where a player entering `to` from `from` should appear: the edge OPPOSITE the one they walked
// through, so travel reads as continuous. Returns false when the two are not actually linked, which
// is what a caller should treat as "refuse the transition" rather than guessing an edge.
inline bool arrivalEdge(u8 from, u8 to, Dir& outEdge) {
    for (u8 d = 0; d < static_cast<u8>(Dir::COUNT); d++) {
        if (neighbourOf(from, static_cast<Dir>(d)) == to) {
            outEdge = opposite(static_cast<Dir>(d));
            return true;
        }
    }
    return false;
}

} // namespace Zone
