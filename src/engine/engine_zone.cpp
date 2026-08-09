// engine_zone.cpp — the OVERWORLD zones: build, entry, and the edge/POI transitions between them.
//
// A zone is an ordinary level on a SENTINEL FLOOR (52-96, see game/zone_def.h). That is the whole
// architecture, and it is what makes an open world affordable here: the floor byte is already the
// world's identity on the wire (`SV_LEVEL_SEED`) and in the save header, so a network of connected
// areas costs no protocol change, no save-format change, and no geometry traffic — a client rebuilds
// a zone from the same six bytes (floor, difficulty, seed) it already uses for a dungeon floor.
//
// The terrain is SEEDED (LayoutStyle::WILDERNESS) but the LANDMARKS are not: edge gates, the waypoint
// and any POI entrance are stamped at deterministic anchors AFTER the carve. That split is
// deliberate — pure procedural placement is how a landmark ends up walled in (VERTICAL_HALL and
// FOUR_STORY both shipped that bug), and hand-authoring every zone is how you get no variety and a
// bespoke function per area.

#include "engine/engine.h"
#include "game/game_constants.h"
#include "game/zone_def.h"
#include "game/zone_route.h"
#include "game/free_play.h"   // overworldUnlocked — the Inferno gate on the town's north road
#include "world/level_gen.h"
#include "world/level_mesh.h"
#include "renderer/material.h"
#include "renderer/minimap.h"
#include "audio/audio.h"
#include "platform/input.h"
#include "core/log.h"
#include <cstring>   // strcmp — the named zone boss is resolved against the enemy def table

namespace {

// How wide the walkable opening in a border wall is, and how deep into the zone the trigger band
// reaches. The gate is generous (5 cells) because an edge you have to hunt for reads as a wall.
// The geometry constants themselves live in game/zone_def.h — see the block there for why the
// RELATIONSHIPS between them (arrival clears the band; a respawn clears it by a lot more) are the
// load-bearing part and are pinned by test.
using Zone::RETURN_GATE_OFFSET;
using Zone::EDGE_TRIGGER_BAND;
using Zone::EDGE_ARRIVE_INSET;
using Zone::ARRIVAL_BACKOFF;
constexpr u32 GATE_HALF = 2;          // gate spans centre-2 .. centre+2
// Where a zone's fixtures sit, in cells, as a fraction of the grid. Deterministic so a player who
// learns a zone keeps their bearings, and so co-op peers agree without any traffic.
constexpr f32 WAYPOINT_FRAC_X = 0.5f, WAYPOINT_FRAC_Z = 0.62f;
constexpr f32 POI_FRAC_X      = 0.28f, POI_FRAC_Z      = 0.30f;

// A local integer LCG for the ruins dressing. LevelGen's GenRNG lives in its .cpp, and the dressing
// must stay INTEGER-ONLY and libm-free for the same reason the carve does: host and client dress the
// zone independently from the shared seed, and a cosf() here would desync co-op across platforms.
struct DressRNG {
    u32 state;
    u32 next() { state = state * 1664525u + 1013904223u; return state; }
    u32 range(u32 lo, u32 hi) { return (hi <= lo) ? lo : lo + (next() >> 8) % (hi - lo); }
};

} // namespace

// Clear a walkable pad around (cx,cz) so a stamped landmark can never be buried by the terrain that
// was generated before it. Mirrors the clearPad idea the stacked floors use for their endpoints.
void Engine::zoneClearPad(u32 cx, u32 cz, u32 radius) {
    const u32 W = m_level.grid.width, D = m_level.grid.depth;
    for (s32 dz = -static_cast<s32>(radius); dz <= static_cast<s32>(radius); dz++) {
        for (s32 dx = -static_cast<s32>(radius); dx <= static_cast<s32>(radius); dx++) {
            const s32 x = static_cast<s32>(cx) + dx, z = static_cast<s32>(cz) + dz;
            if (x < 1 || z < 1 || x >= static_cast<s32>(W) - 1 || z >= static_cast<s32>(D) - 1) continue;
            GridCell& c = LevelGridSystem::getCell(m_level.grid, static_cast<u32>(x), static_cast<u32>(z));
            c.flags         = CELL_FLOOR;    // no CELL_CEILING — still outdoors
            c.floorHeight   = 0;
            c.ceilingHeight = 12;            // 3 m: the height of adjacent solid walls
        }
    }
}

// Cut the walkable opening in one border wall. Called only for edges the zone graph actually links,
// so an unlinked side stays solid and the world still "ends politely".
void Engine::zoneOpenGate(Zone::Dir dir) {
    const u32 W = m_level.grid.width, D = m_level.grid.depth;
    const u32 cx = W / 2, cz = D / 2;
    for (s32 o = -static_cast<s32>(GATE_HALF); o <= static_cast<s32>(GATE_HALF); o++) {
        u32 gx = 0, gz = 0;
        switch (dir) {
            case Zone::Dir::NORTH: gx = static_cast<u32>(static_cast<s32>(cx) + o); gz = 0;     break;
            case Zone::Dir::SOUTH: gx = static_cast<u32>(static_cast<s32>(cx) + o); gz = D - 1; break;
            case Zone::Dir::WEST:  gx = 0;     gz = static_cast<u32>(static_cast<s32>(cz) + o); break;
            case Zone::Dir::EAST:  gx = W - 1; gz = static_cast<u32>(static_cast<s32>(cz) + o); break;
            default: continue;
        }
        if (!LevelGridSystem::isInBounds(m_level.grid, gx, gz)) continue;
        GridCell& c = LevelGridSystem::getCell(m_level.grid, gx, gz);
        c.flags         = CELL_FLOOR;
        c.floorHeight   = 0;
        c.ceilingHeight = 12;
    }
    // ...and clear a CORRIDOR from the opening inward, all the way past where an arriving player is
    // put down. Clearing a single pad two cells in was not enough: the arrival point sits
    // EDGE_ARRIVE_INSET metres inside (5 m — deliberately clear of the re-trigger band), and beyond
    // the pad the generator is free to put a rock clump or a tunnel wall there. Measured: TristRAM's
    // north gate and the Circle Line's west gate both landed the player INSIDE solid geometry, and
    // being spawned in rock shoves the body out through the border — "I got respawned out of bounds".
    //
    // The depth is derived from EDGE_ARRIVE_INSET (+1 cell of margin) rather than typed, so moving
    // the arrival can never again outrun the ground that was cleared for it.
    const u32 reach = static_cast<u32>(EDGE_ARRIVE_INSET) + 1u;
    for (u32 step = 1; step <= reach; step++) {
        switch (dir) {
            case Zone::Dir::NORTH: zoneClearPad(cx, step, GATE_HALF);             break;
            case Zone::Dir::SOUTH: zoneClearPad(cx, D - 1 - step, GATE_HALF);     break;
            case Zone::Dir::WEST:  zoneClearPad(step, cz, GATE_HALF);             break;
            case Zone::Dir::EAST:  zoneClearPad(W - 1 - step, cz, GATE_HALF);     break;
            default: break;
        }
    }
}

// World position of a zone's gate mouth, one step INSIDE the wall — where a player arriving from the
// neighbouring zone is placed.

// Snap a fixture anchor onto a ROOM the generator actually carved.
//
// WHY. The waypoint and POI mouths used to be stamped at fixed FRACTIONS of the grid, with a pad
// cleared around them. That is safe on WILDERNESS, whose interior starts open — but zones now choose
// their terrain, and a GAUNTLET tunnel or a HUB concourse is mostly solid. Clearing a 5x5 pad in the
// middle of solid rock does not make a landmark reachable; it makes an isolated POCKET with the
// landmark inside it. The Bank Station portal generated exactly like that: the act soak found a
// wanderer eight metres from it for twenty-one minutes with NO flow direction at all and a wall
// ahead — the quest, and therefore the rest of Act 2, was unreachable. A human would have been just
// as stuck.
//
// Room centres are the fix because every layout style guarantees them open and connected — it is the
// contract every other placement consumer (enemies, chests, bosses, lights) already relies on. The
// room is picked by SEED so host and client agree without a byte on the wire, and biased toward the
// requested fraction so a zone's landmarks still sit roughly where they were authored to.
u32 Engine::zoneAnchorRoom(const DungeonResult& gen, u32 seed, f32 fracX, f32 fracZ) const {
    if (gen.roomCount == 0) return 0;
    const f32 want = fracX * static_cast<f32>(m_level.grid.width);
    const f32 wantZ = fracZ * static_cast<f32>(m_level.grid.depth);
    u32 best = 0; f32 bestD2 = 1e18f;
    for (u32 i = 0; i < gen.roomCount; i++) {
        const f32 cx = static_cast<f32>(gen.rooms[i].x) + static_cast<f32>(gen.rooms[i].w) * 0.5f;
        const f32 cz = static_cast<f32>(gen.rooms[i].z) + static_cast<f32>(gen.rooms[i].d) * 0.5f;
        const f32 dx = cx - want, dz = cz - wantZ;
        const f32 d2 = dx * dx + dz * dz;
        if (d2 < bestD2) { bestD2 = d2; best = i; }
    }
    (void)seed;   // deterministic by geometry alone; no roll needed, so no seed to disagree about
    return best;
}

// The world position of a room's centre.
Vec3 Engine::zoneRoomCentre(const DungeonResult& gen, u32 idx) const {
    if (gen.roomCount == 0) return { static_cast<f32>(m_level.grid.width) * 0.5f, 0.0f,
                                     static_cast<f32>(m_level.grid.depth) * 0.5f };
    const DungeonRoom& r = gen.rooms[idx < gen.roomCount ? idx : 0];
    return { static_cast<f32>(r.x) + static_cast<f32>(r.w) * 0.5f, 0.0f,
             static_cast<f32>(r.z) + static_cast<f32>(r.d) * 0.5f };
}

Vec3 Engine::zoneGatePos(Zone::Dir dir) const {
    const f32 W = static_cast<f32>(m_level.grid.width), D = static_cast<f32>(m_level.grid.depth);
    const f32 cx = W * 0.5f, cz = D * 0.5f;
    // INSIDE the trigger band, not on its edge. This used to be a literal 2.5 — exactly
    // EDGE_TRIGGER_BAND — so an arriving player satisfied `p.z <= EDGE_TRIGGER_BAND` on the very
    // first tick and was sent straight back out through the gate they had just come through.
    // Derived from the band so the two can never drift apart again.
    const f32 in = EDGE_ARRIVE_INSET;
    switch (dir) {
        case Zone::Dir::NORTH: return { cx,      0.0f, in };
        case Zone::Dir::SOUTH: return { cx,      0.0f, D - in };
        case Zone::Dir::WEST:  return { in,      0.0f, cz };
        case Zone::Dir::EAST:  return { W - in,  0.0f, cz };
        default:               return { cx,      0.0f, cz };
    }
}

// Build a zone's level: seeded terrain, then the deterministic landmarks stamped on top.
// Returns the zone centre.
Vec3 Engine::buildZoneLevel(const Zone::ZoneDef& def) {
    const u32 size = def.gridSize;
    LevelGridSystem::init(m_level.grid, size, size, 1.0f);

    // Terrain is chosen per zone (ZoneDef::terrain), not inferred: Act 1 is open country and caves,
    // Act 2 is tube tunnels and station concourses, and a tube line genuinely IS a serpentine chain
    // of platforms — which is what GAUNTLET already builds. Reusing the shipped generators rather
    // than writing an "underground" one keeps every layout invariant they are already tested for.
    // The seed is derived exactly as a dungeon floor's, so a client rebuilds byte-identical terrain.
    const u32 zoneSeed = m_level.levelSeed + static_cast<u32>(def.floor) * 7919u
                       + static_cast<u32>(m_difficulty) * 104729u;
    LevelGen::LayoutStyle style = LevelGen::LayoutStyle::WILDERNESS;
    switch (def.terrain) {
        case Zone::Terrain::CAVE:    style = LevelGen::LayoutStyle::CAVERN;   break;
        case Zone::Terrain::TUNNEL:  style = LevelGen::LayoutStyle::GAUNTLET; break;
        case Zone::Terrain::STATION: style = LevelGen::LayoutStyle::HUB;      break;
        default:                     style = LevelGen::LayoutStyle::WILDERNESS; break;
    }
    const DungeonResult gen = LevelGen::generate(m_level.grid, zoneSeed, size, size, style);
    // Record what we actually built. Consumers read m_level.layoutStyle (the AI's open-floor
    // detection comp, the nav/autoplay style branches), and leaving the previous dungeon floor's
    // value in place made a zone behave like whatever floor you came from.
    m_level.layoutStyle = style;

    // SURFACE zones lose their ceiling so the sky shows (the town's trick). UNDERGROUND zones keep
    // it — after a whole act of open country, the roof coming down is the tonal shift into Act 2, and
    // it is also what makes the tunnels claustrophobic rather than merely dark.
    if (!def.underground) {
        for (u32 z = 1; z < size - 1; z++)
            for (u32 x = 1; x < size - 1; x++) {
                GridCell& c = LevelGridSystem::getCell(m_level.grid, x, z);
                if (c.flags & CELL_FLOOR) c.flags &= static_cast<u8>(~CELL_CEILING);
            }
    }

    // Outdoor dressing: grass underfoot, warm brick walls. Dark stone reads as a void in daylight,
    // which is why the town uses brick too.
    if (!def.underground) {
        const u8 grass = MaterialSystem::getIdByName("town_grass");
        const u8 wall  = MaterialSystem::getIdByName("brick_wall");
        for (u32 z = 0; z < size; z++)
            for (u32 x = 0; x < size; x++) {
                GridCell& c = LevelGridSystem::getCell(m_level.grid, x, z);
                c.floorMaterialId = grass;
                c.wallMaterialId  = wall;
            }
    }

    // RUINS dressing: a settlement that lost. Deliberately built from HUT SHELLS rather than the
    // town's solid blocks — each is a rectangle of wall with its interior hollowed and one side
    // punched open, so you can walk into every one of them and they read as roofless, gutted
    // buildings instead of scenery you bump into. That matters here because TristRAM is OVERRUN:
    // the ruins have to be fightable space, and doorways you can be chased through are the whole
    // texture of the place. The ground goes blood-dark and the walls stay brick, which reads as
    // scorched rather than merely old.
    if (def.ruins) {
        const u8 dead  = MaterialSystem::getIdByName("blood_floor");
        const u8 brick = MaterialSystem::getIdByName("brick_wall");
        for (u32 z = 1; z < size - 1; z++)
            for (u32 x = 1; x < size - 1; x++)
                LevelGridSystem::getCell(m_level.grid, x, z).floorMaterialId = dead;

        // Shells on a loose lattice, jittered off the seed so a ruin never looks stamped. Skips the
        // middle band, which is where the waypoint and gates live.
        DressRNG dressRng{zoneSeed ^ 0x51E9A17Bu};
        for (u32 row = 0; row < 3; row++) {
            for (u32 col = 0; col < 3; col++) {
                if (row == 1 && col == 1) continue;             // leave the centre clear
                const u32 hw = dressRng.range(5, 9), hd = dressRng.range(5, 9);
                const u32 bx = 4 + col * ((size - 8) / 3) + dressRng.range(0, 3);
                const u32 bz = 4 + row * ((size - 8) / 3) + dressRng.range(0, 3);
                if (bx + hw >= size - 2 || bz + hd >= size - 2) continue;
                // Walls only — the interior is left open floor.
                for (u32 z = bz; z < bz + hd; z++) {
                    for (u32 x = bx; x < bx + hw; x++) {
                        const bool edge = (x == bx || z == bz || x == bx + hw - 1 || z == bz + hd - 1);
                        GridCell& c = LevelGridSystem::getCell(m_level.grid, x, z);
                        if (!edge) { c.flags = CELL_FLOOR; c.floorMaterialId = dead; continue; }
                        c.flags          = CELL_SOLID;
                        c.wallMaterialId = brick;
                    }
                }
                // Punch a doorway, and collapse a second span so the shell is visibly broken open.
                const u32 doorX = bx + 1 + dressRng.range(0, hw - 2);
                LevelGridSystem::getCell(m_level.grid, doorX, bz).flags = CELL_FLOOR;
                const u32 gapZ = bz + 1 + dressRng.range(0, hd - 2);
                LevelGridSystem::getCell(m_level.grid, bx + hw - 1, gapZ).flags = CELL_FLOOR;
                LevelGridSystem::getCell(m_level.grid, bx + hw - 1,
                                         (gapZ + 1 < bz + hd - 1) ? gapZ + 1 : gapZ).flags = CELL_FLOOR;
            }
        }
    }

    // Landmarks, stamped AFTER the carve so terrain can never bury them.
    for (u8 d = 0; d < static_cast<u8>(Zone::Dir::COUNT); d++)
        if (def.neighbour[d] != Zone::NO_LINK) zoneOpenGate(static_cast<Zone::Dir>(d));

    // Both fixtures ride ROOM CENTRES (see zoneAnchorRoom) rather than raw grid fractions: a pad
    // cleared in the middle of a tunnel wall is an isolated pocket, not a reachable landmark.
    if (def.hasWaypoint) {
        const Vec3 c = zoneRoomCentre(gen, zoneAnchorRoom(gen, zoneSeed, WAYPOINT_FRAC_X, WAYPOINT_FRAC_Z));
        m_zoneWaypointPos = c;
        zoneClearPad(static_cast<u32>(c.x), static_cast<u32>(c.z), 2);
    }
    if (def.poiFloor != Zone::NO_LINK) {
        const Vec3 c = zoneRoomCentre(gen, zoneAnchorRoom(gen, zoneSeed, POI_FRAC_X, POI_FRAC_Z));
        m_zonePoiPos = c;
        zoneClearPad(static_cast<u32>(c.x), static_cast<u32>(c.z), 2);
    }
    // An interior's way out is its own landmark; put it at the centre so it is never behind you.
    if (def.returnFloor != Zone::NO_LINK)
        zoneClearPad(size / 2, size / 2, 3);   // the boss pad — radius 3, matching spawnZoneContents

    // THE ARRIVAL, and the way back out. Both were cleared in spawnZoneContents, which runs AFTER
    // the mesh is built — so they behaved as floor while still RENDERING as rock — and for a zone
    // with no return gate the arrival was never cleared at all. That is the reported "I get revived
    // somewhere else and sometimes out of bounds": waypoint travel arrives at centre+10m south,
    // which on a road zone is whatever the generator put there. Landing inside geometry makes THAT
    // the respawn anchor, and reviving into rock shoves the body out through the border.
    // Cleared UNCONDITIONALLY, never keyed on how the player arrived: the grid is rebuilt from the
    // shared seed on every peer, and a pad that depends on `fromFloor` would have the host and a
    // guest carve DIFFERENT geometry from the same seed. Edge arrivals need nothing here — the gate
    // corridors above already cover them, deterministically. This is the fallback arrival (waypoint
    // travel, and the portal into an interior), which is a fixed spot per zone.
    {
        const Vec3 back = zoneReturnPos(def);
        zoneClearPad(static_cast<u32>(back.x), static_cast<u32>(back.z), 3);          // the gate
        zoneClearPad(static_cast<u32>(back.x), static_cast<u32>(back.z + ARRIVAL_BACKOFF), 2);   // and where you land
    }

    m_level.sectionCount = LevelMeshSystem::buildAll(m_level.grid, zoneSeed,
                                                     m_level.sections, MAX_LEVEL_SECTIONS);
    LevelGridSystem::buildClearanceField(m_level.grid);
    Minimap::init(m_level.grid.width, m_level.grid.depth);

    const Vec3 center = { static_cast<f32>(size) * 0.5f, 0.0f, static_cast<f32>(size) * 0.5f };
    LevelGridSystem::buildFlowField(m_level.grid, center);
    m_zoneGen = gen;   // the population pass needs the room rects
    return center;
}

// Where a player entering `def` from `fromFloor` should stand. Arriving through an edge puts them at
// the OPPOSITE gate so travel reads as continuous; anything else (a waypoint jump, entering a POI)
// drops them at the zone's own arrival point.
// Where the RETURN portal stands in a zone you can only reach by portal, and — offset a little
// further out — where an arriving player is put down.
//
// Both used to be the zone CENTRE, and so is the boss: `spawnZoneContents` clears a pad at the
// centre and puts the named boss on it. So stepping through the portal into the Deprecated
// Graveyard or (now) TristRAM dropped the player ON TOP of the boss, with the way back underneath
// them. Standing the pair off to the south makes the arrival read the way it should: you come out
// of the portal, the way home is at your back, and the thing you came for is across the ruins.
// The point a traveller must WALK TO in order to leave by `dir` — inside the trigger band, unlike
// zoneGatePos, which is where an ARRIVING player is put down and is deliberately clear of it.
Vec3 Engine::zoneEdgeCrossPos(Zone::Dir dir) const {
    const f32 W = static_cast<f32>(m_level.grid.width), D = static_cast<f32>(m_level.grid.depth);
    const f32 cx = W * 0.5f, cz = D * 0.5f;
    const f32 in = Zone::EDGE_CROSS_DEPTH;
    switch (dir) {
        case Zone::Dir::NORTH: return { cx,     0.0f, in };
        case Zone::Dir::SOUTH: return { cx,     0.0f, D - in };
        case Zone::Dir::WEST:  return { in,     0.0f, cz };
        case Zone::Dir::EAST:  return { W - in, 0.0f, cz };
        default:               return { cx,     0.0f, cz };
    }
}

Vec3 Engine::zoneReturnPos(const Zone::ZoneDef& def) const {
    const f32 half = static_cast<f32>(def.gridSize) * 0.5f;
    return { half, 0.0f, half + RETURN_GATE_OFFSET };
}

Vec3 Engine::zoneArrivalPos(const Zone::ZoneDef& def, u8 fromFloor) {
    Zone::Dir edge;
    if (fromFloor != 0 && Zone::arrivalEdge(fromFloor, def.floor, edge))
        return zoneGatePos(edge);
    // Coming from the town into the first zone: the town is south of it, so arrive at the south gate
    // even though the town has no ZoneDef row of its own.
    if (fromFloor == Zone::TOWN_FLOOR) {
        for (u8 d = 0; d < static_cast<u8>(Zone::Dir::COUNT); d++)
            if (def.neighbour[d] == Zone::TOWN_FLOOR) return zoneGatePos(static_cast<Zone::Dir>(d));
    }
    const f32 size = static_cast<f32>(def.gridSize);
    // Portal arrival (no shared border to come through): stand just outside the return gate rather
    // than on the zone's centre, which the boss occupies.
    (void)size;
    const Vec3 back = zoneReturnPos(def);
    return { back.x, 0.0f, back.z + ARRIVAL_BACKOFF };
}


// Record what is left alive here, so coming back does not undo the fight.
//
// Called every frame the player is in a zone rather than on the way out: there is no single "leaving
// a zone" choke (an edge crossing, a portal, a waypoint jump and a town exit are four different
// paths), and a rule that has to be remembered at four call sites is one that will be missed at a
// fifth. Counting the active list is a few dozen iterations — cheaper than the bookkeeping to avoid
// it.
void Engine::zoneRememberState() {
    if (!m_level.inZone || m_netRole == NetRole::CLIENT) return;
    const u32 slot = static_cast<u32>(m_level.zoneFloor) - Zone::FLOOR_MIN;
    if (slot >= ZONE_SLOTS) return;

    u32 hostiles = 0;
    bool bossAlive = false;
    for (u32 a = 0; a < m_entities.activeCount; a++) {
        const Entity& e = m_entities.entities[m_entities.activeList[a]];
        if (e.flags & ENT_DEAD)     continue;
        if (e.flags & ENT_FRIENDLY) continue;
        if (e.enemyType == EnemyType::PROP) continue;
        hostiles++;
        if (e.isBoss) bossAlive = true;
    }
    m_zoneHostilesLeft[slot] = static_cast<u8>(hostiles > 254 ? 254 : hostiles);

    // The boss bit LATCHES on the transition from spawned-and-alive to gone, so it can only be set
    // by the boss actually dying. Setting it from "no boss present" alone would mark every zone
    // whose boss has not spawned yet — including, on the first frame after entry, the one we just
    // walked into.
    const Zone::ZoneDef* def = Zone::find(m_level.zoneFloor);
    if (def && def->boss && def->boss[0]) {
        if (bossAlive) m_zoneBossSeenAlive |= (1ull << slot);
        else if (m_zoneBossSeenAlive & (1ull << slot)) m_zoneBossDead |= (1ull << slot);
    }
}

// Cull a freshly generated roster back to what the player left behind.
//
// The zone is regenerated from its seed, so the SAME enemies cannot be restored — only how many.
// That is the honest limit of a seed-rebuilt world without a per-zone roster in the save, and it is
// enough for what reads as broken: ground you cleared stays clear, and a half-fought zone does not
// come back at full strength.
void Engine::zoneApplyRemembered(const Zone::ZoneDef& def) {
    if (m_netRole == NetRole::CLIENT) return;
    const u32 slot = static_cast<u32>(def.floor) - Zone::FLOOR_MIN;
    if (slot >= ZONE_SLOTS) return;
    const u8 remembered = m_zoneHostilesLeft[slot];
    if (remembered == 0xFF) return;                     // never been here — a full roster is right

    // Walk the pool backwards and despawn ordinary hostiles until the count matches. The BOSS is
    // never culled here: it is handled by its own bit, and losing it to a cull would silently make a
    // SLAY quest uncompletable.
    u32 live = 0;
    for (u32 a = 0; a < m_entities.activeCount; a++) {
        const Entity& e = m_entities.entities[m_entities.activeList[a]];
        if ((e.flags & ENT_DEAD) || (e.flags & ENT_FRIENDLY)) continue;
        if (e.enemyType == EnemyType::PROP) continue;
        live++;
    }
    for (u32 a = m_entities.activeCount; a-- > 0 && live > remembered; ) {
        const u32 idx = m_entities.activeList[a];
        Entity& e = m_entities.entities[idx];
        if ((e.flags & ENT_DEAD) || (e.flags & ENT_FRIENDLY)) continue;
        if (e.enemyType == EnemyType::PROP) continue;
        if (e.isBoss) continue;
        // Marked DEAD rather than hard-despawned: ENT_DEAD is the state every consumer already
        // understands (targeting, counting, the corpse-raise scan), and it goes through the same
        // path an ordinary kill does instead of tearing an entity out from under the pool.
        e.flags |= ENT_DEAD;
        e.health = 0.0f;
        live--;
    }
    LOG_INFO("[ZONEX] zone %u remembered: %u hostiles kept (boss %s)",
             static_cast<u32>(def.floor), live,
             (m_zoneBossDead & (1ull << slot)) ? "already dead" : "alive/none");
}

// The zone's fixtures: its waypoint, and the gate into/out of a POI. Both are WORLD ITEMS on
// sentinel defIds, which buys spawning, snapshot replication and server-authoritative interaction
// for free — the alternative was a parallel object system, which is exactly the trade the shrine and
// stash already made. Host AND client build their own copies (they are deterministic fixtures, not
// loot), so a guest sees them without waiting for a snapshot.
void Engine::spawnZoneContents(const Zone::ZoneDef& def, Vec3 center) {
    const f32 size = static_cast<f32>(def.gridSize);

    // Hostiles. TIER 5 — the deepest band — because the overworld is post-INFERNO content: anyone
    // out here finished the whole ladder, so tier-1 wildlife would be scenery. A PEACEFUL zone
    // (Null Terminus, the Act 2 survivor station) spawns none at all, which is what makes it read as
    // a refuge rather than a lull. TristRAM is deliberately NOT one: D2's Tristram is a massacre you
    // walk into, and a safe TristRAM would be the one place the parody could not afford to be safe.
    // Clients skip it: enemies are server-authoritative and arrive over snapshots, exactly as on a
    // dungeon floor.
    if (!def.peaceful && m_netRole != NetRole::CLIENT)
        spawnFloorEnemies(m_zoneGen, /*tier=*/5, Quest::actOf(def.floor));

    // ...then cull it back to what the player left here last time (see zoneApplyRemembered).
    if (!def.peaceful) zoneApplyRemembered(def);

    // THE ZONE BOSS. Spawned by NAME from the enemy table rather than through the dungeon's
    // spawnFloorBoss path, which keys off bosses.json by FLOOR and expands a room into an arena —
    // neither of which a zone has. A named lookup keeps the boss a piece of zone DATA (one field in
    // ZoneDef) instead of a second table that has to agree with the first about which floor is which.
    // Placed at the zone centre: it is the thing the place is about, and it must not be missable.
    const u32 bossSlot = static_cast<u32>(def.floor) - Zone::FLOOR_MIN;
    const bool bossAlreadyDead = bossSlot < ZONE_SLOTS && (m_zoneBossDead & (1ull << bossSlot));
    if (def.boss && def.boss[0] && !bossAlreadyDead && m_netRole != NetRole::CLIENT) {
        s32 defIdx = -1;
        for (u32 i = 0; i < m_enemyDefs.count; i++)
            if (std::strcmp(m_enemyDefs.defs[i].name, def.boss) == 0) { defIdx = static_cast<s32>(i); break; }
        if (defIdx < 0) {
            // Loud, because a silently absent boss is an unwinnable quest: the SLAY objective would
            // simply never fire and the act would have no ending.
            LOG_ERROR("zone '%s' names boss '%s', which is not in enemies.json", def.name, def.boss);
        } else {
            const EnemyDef& bd = m_enemyDefs.defs[defIdx];
            const Vec3 bossPos = { center.x, 0.0f, center.z };
            EntityHandle bh = EntitySystem::spawn(m_entities, bossPos, bd.halfExtents, bd.flying,
                                                  bd.health, bd.moveSpeed, bd.detectionRange,
                                                  bd.attackRange, bd.attackCooldown, bd.damage);
            if (Entity* be = handleGet(m_entities, bh)) {
                be->meshId       = bd.meshId;
                be->materialId   = bd.materialId;
                be->enemyType    = bd.enemyType;
                be->enemyRole    = bd.role;
                be->aiPreference = bd.aiPreference;
                be->enemyDefIdx  = static_cast<u8>(defIdx);   // names it, and drives the pet drop
                be->baseMoveSpeed      = be->moveSpeed;
                be->baseAttackCooldown = be->attackCooldown;
                // SCALE IT like everything else on the floor. The original decision here — skip the
                // curve because the def is "already authored at post-Inferno numbers" — was simply
                // wrong, and badly so: zone TRASH goes through floorHealthMult at the ladder end
                // (~2025x), so a scaled mob lands near 217k HP while an unscaled Griswald sat at
                // 4200. The act's climax had TWO PERCENT of a trash mob's health and would have
                // died to a single hit. Aaron found the symptom from the other end ("monsters are
                // much tankier than Inferno") — same root cause, opposite direction.
                //
                // Running the boss through the identical hpMult/dmgMult makes its authored value a
                // BOSS-TIER BASE relative to the ~107 trash baseline: Griswald 4200 is 39x a trash
                // mob, Signal Failure 5200 is 49x. That ratio is the thing worth authoring; the
                // absolute number never was.
                const u32 effFloor = scalingEffectiveFloor();
                const f32 bHp  = GameConst::floorHealthMult(effFloor)
                               * GameConst::difficultyHealthBump(m_difficulty)
                               * GameConst::overworldHpMult(true);
                const f32 bDmg = GameConst::floorDamageMult(effFloor)
                               * GameConst::difficultyDamageBump(m_difficulty)
                               * GameConst::overworldDamageMult(true);
                be->health    *= bHp;
                be->maxHealth  = be->health;
                be->damage    *= bDmg;
                be->level      = static_cast<u16>(effFloor);

                // isBoss drives the health bar, the nameplate and the loot guarantee.
                be->isBoss = true;
                LOG_INFO("Zone boss spawned: %s (%.0f HP, base %.0f x%.0f)", def.boss,
                         static_cast<double>(be->health), static_cast<double>(bd.health),
                         static_cast<double>(bHp));
            }
        }
    }

    if (def.hasWaypoint) {
        ItemInstance wp{};
        wp.defId     = WAYPOINT_ID;
        wp.itemLevel = def.floor;          // which zone this waypoint belongs to
        wp.uid       = m_worldItems.nextUid++;
        const Vec3 pos = m_zoneWaypointPos;   // the room centre buildZoneLevel cleared
        WorldItemSystem::spawn(m_worldItems, wp, pos, &m_level.grid, 0xFF);
        LOG_INFO("Zone fixture: WAYPOINT at (%.1f, %.1f)", (double)pos.x, (double)pos.z);
    }

    // A POI mouth leads IN; an interior's gate leads back OUT. Both are the same object with a
    // different destination byte, so there is one code path and one thing to get right.
    if (def.poiFloor != Zone::NO_LINK) {
        ItemInstance gate{};
        gate.defId     = ZONE_GATE_ID;
        gate.itemLevel = def.poiFloor;
        gate.uid       = m_worldItems.nextUid++;
        const Vec3 pos = m_zonePoiPos;        // the room centre buildZoneLevel cleared
        WorldItemSystem::spawn(m_worldItems, gate, pos, &m_level.grid, 0xFF);
        LOG_INFO("Zone fixture: POI GATE -> floor %u at (%.1f, %.1f)",
                 (u32)def.poiFloor, (double)pos.x, (double)pos.z);
    }
    if (def.returnFloor != Zone::NO_LINK) {
        ItemInstance gate{};
        gate.defId     = ZONE_GATE_ID;
        gate.itemLevel = def.returnFloor;
        gate.uid       = m_worldItems.nextUid++;
        const Vec3 rpos = zoneReturnPos(def);   // ground cleared in buildZoneLevel, before the mesh
        WorldItemSystem::spawn(m_worldItems, gate, rpos, &m_level.grid, 0xFF);
        LOG_INFO("Zone fixture: RETURN GATE -> floor %u", (u32)def.returnFloor);
    }

    // Snapshot the roster AFTER the remembered-state cull, so the Journal's n/m row counts against
    // what this visit actually spawned rather than what a first visit would have.
    m_zoneHostilesAtEntry = zoneHostilesAlive();
}

// --- Act 1 quests ------------------------------------------------------------------------------
// The chat line IS the journal. That is a deliberate scope choice, not an omission: a journal screen
// is a UI project, and the act reads perfectly well as "you are told what this place wants, and told
// when you have done it". Everything below is three small hooks on events the engine already fires.

// The mask is a CACHE of m_questProgress. Every mutation goes through here, so the two cannot
// drift — the failure shape this codebase keeps rediscovering (see CLAUDE.md's "one fact stored
// twice" thread).
void Engine::refreshQuestMask(u8 lane) {
    if (lane >= MAX_LOCAL_PLAYERS) return;
    // ADOPT before deriving. Two writers still set the mask directly and neither can be reached
    // from here: the save load (until SAVE_VERSION 7 carries Progress itself) and the
    // --quests-done dev door. Deriving straight from an empty Progress would silently UN-complete
    // their acts on the first zone entry — measured as `[ZONEX] refused 56 -> 58 (quest gate)`,
    // i.e. the onward road sealed — and the next autosave would write that loss back permanently.
    // migrateFromMask only ever ADDS completions, so the mask is monotonic across a refresh and
    // this can never lose progress; it becomes a no-op once both writers hand over Progress.
    Quest::migrateFromMask(m_questProgress[lane], m_questMask[lane]);
    m_questMask[lane] = Quest::completionMask(m_questProgress[lane]);
}

// Announce a quest's completion once, when it crosses into COMPLETE. Callers mutate progress and
// then call this; it is a no-op if the quest did not just finish.
void Engine::questAnnounce(u8 questIdx, bool wasComplete) {
    if (questIdx >= Quest::COUNT) return;
    const u8 lane = m_localPlayerIndex;
    const bool nowComplete = Quest::isComplete(m_questMask[lane], Quest::QUESTS[questIdx].zoneFloor);
    if (!nowComplete || wasComplete) return;

    const Quest::QuestDef& q = Quest::QUESTS[questIdx];
    addChatMessage("", q.name, Vec3{1.0f, 0.85f, 0.35f});
    LOG_INFO("[QUEST] complete: %s", q.name);
    AudioSystem::play(SfxId::LEVEL_UP);

    const u8 act = Quest::actOf(q.zoneFloor);
    if (Quest::actComplete(m_questMask[lane], act)) {
        addChatMessage("", act == 1 ? "Act 1 complete - the platform is open."
                                    : "Act 2 complete - the gate is closed.",
                       Vec3{1.0f, 0.95f, 0.6f});
        LOG_INFO("[QUEST] ACT %u COMPLETE", static_cast<u32>(act));
    }
}

// Offered on arrival. REACH quests take their objective from the same event — finding the place
// WAS the task — so arrival both offers and advances.
void Engine::questOnZoneEnter(u8 zoneFloor) {
    const u8 idx = Quest::indexForZone(zoneFloor);
    if (idx == 0xFF) return;
    const u8 lane = m_localPlayerIndex;
    const bool wasComplete = Quest::isComplete(m_questMask[lane], zoneFloor);

    Quest::offer(m_questProgress[lane], idx);
    Quest::noteReached(m_questProgress[lane], idx);
    refreshQuestMask(lane);

    if (!wasComplete && !Quest::isComplete(m_questMask[lane], zoneFloor)) {
        const Quest::QuestDef& q = Quest::QUESTS[idx];
        addChatMessage("", q.blurb, Vec3{0.75f, 0.8f, 0.9f});
        // Logged as well as shown. A chat-only offer is invisible to a soak and to any
        // after-the-fact check of whether the chain actually armed — the blind spot that hid the
        // credits park and the dead legendaries until a log line was added.
        LOG_INFO("[QUEST] offered: %s (giver %s)", q.name, Quest::GIVERS[q.giverIdx].name);
    }
    questAnnounce(idx, wasComplete);
}

// SLAY. Called from handleDeathPreamble — the ONE choke every enemy death funnels through — so no
// kill route (a proc, a pet, a thorns reflect) can miss an objective.
void Engine::questOnEnemyKilled(const char* enemyName) {
    if (!m_level.inZone || !enemyName) return;
    const u8 idx = Quest::indexForZone(m_level.zoneFloor);
    if (idx == 0xFF) return;
    const u8 lane = m_localPlayerIndex;
    const bool wasComplete = Quest::isComplete(m_questMask[lane], m_level.zoneFloor);

    Quest::noteKill(m_questProgress[lane], idx, enemyName);
    refreshQuestMask(lane);
    questAnnounce(idx, wasComplete);
}

// Live hostile count in the current zone. Used by BOTH the CLEAR_ZONE completion poll and the
// Journal's progress row, so the number the player reads and the number that completes the quest
// are the same number.
u16 Engine::zoneHostilesAlive() const {
    u16 n = 0;
    for (u32 a = 0; a < m_entities.activeCount; a++) {
        const Entity& e = m_entities.entities[m_entities.activeList[a]];
        if (e.flags & (ENT_DEAD | ENT_FRIENDLY)) continue;
        if (e.npcClass != NpcClass::NONE) continue;   // friendly class NPCs are not hostiles
        n++;
    }
    return n;
}

// CLEAR_ZONE. Polled rather than event-driven because "no hostiles left" is a property of the
// pool, not of any one death — a summoner's last minion and the summoner itself can die on the
// same tick, and an event-per-death would have to re-scan anyway.
void Engine::questCheckZoneCleared() {
    if (!m_level.inZone) return;
    const u8 idx = Quest::indexForZone(m_level.zoneFloor);
    if (idx == 0xFF) return;
    const u8 lane = m_localPlayerIndex;
    if (Quest::isComplete(m_questMask[lane], m_level.zoneFloor)) return;
    if (zoneHostilesAlive() > 0) return;

    Quest::noteCleared(m_questProgress[lane], idx);
    refreshQuestMask(lane);
    questAnnounce(idx, /*wasComplete*/ false);
}

// --- Waypoints -----------------------------------------------------------------------------------
// Discovery is PER CHARACTER (Aaron's call — D2's model): each hero finds their own network. The set
// is a u64 bitmask indexed by the zone's position in the table, which is why ZONES is append-only in
// practice — reordering it would silently reassign every saved hero's discovered set.

u8 Engine::zoneBitFor(u8 zoneFloor) {
    for (u32 i = 0; i < Zone::COUNT; i++)
        if (Zone::ZONES[i].floor == zoneFloor) return static_cast<u8>(i);
    return 0xFF;
}

bool Engine::waypointDiscovered(u8 zoneFloor) const {
    const u8 bit = const_cast<Engine*>(this)->zoneBitFor(zoneFloor);
    return bit != 0xFF && (m_waypointMask[m_localPlayerIndex] & (1ull << bit)) != 0;
}

// Touching a waypoint: record the discovery, then open the travel list. The fixture is deliberately
// LEFT ACTIVE — every other sentinel is consumed on use, and a waypoint that vanished after one use
// would be the single most confusing thing in the overworld.
void Engine::touchWaypoint(s32 worldItemIdx) {
    if (worldItemIdx < 0 || worldItemIdx >= static_cast<s32>(MAX_WORLD_ITEMS)) return;
    const WorldItem& wi = m_worldItems.items[static_cast<u32>(worldItemIdx)];
    if (!wi.active || !isWaypoint(wi.item)) return;

    const u8 zoneFloor = static_cast<u8>(wi.item.itemLevel);
    const u8 bit = zoneBitFor(zoneFloor);
    if (bit == 0xFF) return;

    const bool isNew = (m_waypointMask[m_localPlayerIndex] & (1ull << bit)) == 0;
    m_waypointMask[m_localPlayerIndex] |= (1ull << bit);
    if (isNew) {
        const Zone::ZoneDef* def = Zone::find(zoneFloor);
        addChatMessage("", def ? def->name : "Waypoint", Vec3{0.55f, 0.85f, 1.0f});
        LOG_INFO("Waypoint discovered: %s", def ? def->name : "?");
        AudioSystem::play(SfxId::LEVEL_UP);
    }
    openWaypointUI();
}

// Walking into a POI mouth (or an interior's exit). The destination rides in the fixture's itemLevel
// byte, so one object serves both directions.
void Engine::enterZoneGate(s32 worldItemIdx) {
    if (worldItemIdx < 0 || worldItemIdx >= static_cast<s32>(MAX_WORLD_ITEMS)) return;
    const WorldItem& wi = m_worldItems.items[static_cast<u32>(worldItemIdx)];
    if (!wi.active || !isZoneGate(wi.item)) return;
    if (m_netRole == NetRole::CLIENT) return;   // the host owns world changes; we follow its broadcast

    const u8 dest = static_cast<u8>(wi.item.itemLevel);
    const u8 from = m_level.inZone ? m_level.zoneFloor : Zone::TOWN_FLOOR;
    if (dest == Zone::TOWN_FLOOR) { enterTown(); return; }
    if (!Zone::isZoneFloor(dest)) {
        LOG_WARN("zone gate points at floor %u, which is not a zone", static_cast<u32>(dest));
        return;
    }
    if (!zoneLinkAllowed(from, dest)) return;
    // Same reason the edge crossing logs: a world change is the one event worth a line, and a soak
    // that counts only edge crossings undercounts every interior in both acts.
    LOG_INFO("[ZONEX] portal %u -> %u", static_cast<u32>(from), static_cast<u32>(dest));
    enterZone(dest, from);
}

// The travel list. Opened OVER the live world exactly as the town portal opens the Free-Play
// select: the zone is never torn down, m_gameState flips to MENU, and BACK flips it straight back to
// IN_GAME. Reusing that flow rather than inventing an overlay means the menu already has working
// keyboard/controller nav and the world is still there behind it.
void Engine::openWaypointUI() {
    m_menu.subState     = 25;   // waypoint travel list (23/24 are the Auto-Loot choosers)
    m_menu.subSelection = 0;
    m_gameState         = GameState::MENU;
    Input::setRelativeMouseMode(false);
}

// The destinations this character may travel to: the town (always — it is the anchor of the network
// and a hero who found any waypoint has by definition been there) plus every DISCOVERED zone.
// Returns how many were written.
u32 Engine::waypointDestinations(u8* outFloors, u32 maxOut) const {
    u32 n = 0;
    if (n < maxOut) outFloors[n++] = Zone::TOWN_FLOOR;
    for (u32 i = 0; i < Zone::COUNT && n < maxOut; i++) {
        if (!Zone::ZONES[i].hasWaypoint) continue;
        if (!waypointDiscovered(Zone::ZONES[i].floor)) continue;
        if (m_level.inZone && m_level.zoneFloor == Zone::ZONES[i].floor) continue;  // already here
        outFloors[n++] = Zone::ZONES[i].floor;
    }
    return n;
}


// May the player cross from `from` to `to` right now, and if not, say so.
//
// The acts' road is gated on their quests (see game/zone_route.h for the rule and why the two
// onward cases differ). A gate that simply refuses is indistinguishable from a bug — that lesson is
// already written into the town's north gate — so a refusal always explains itself, throttled by the
// same timer that gate uses.
bool Engine::zoneLinkAllowed(u8 from, u8 to) {
    if (ZoneRoute::linkOpen(from, to, m_questMask[m_localPlayerIndex])) return true;

    if (m_zoneGateHintTimer <= 0.0f) {
        LOG_INFO("[ZONEX] refused %u -> %u (quest gate)", static_cast<u32>(from), static_cast<u32>(to));
        // Name the OUTSTANDING quest rather than a generic refusal: "something is unfinished" sends
        // a player wandering, and the whole point of the chain is that it tells you where to go.
        const Zone::ZoneDef* z = Zone::find(from);
        const Quest::QuestDef* blocking = nullptr;
        if (z) {
            if (!ZoneRoute::zoneSettled(from, m_questMask[m_localPlayerIndex]))
                blocking = Quest::forZone(from);
            else if (z->poiFloor != Zone::NO_LINK
                     && !ZoneRoute::zoneSettled(z->poiFloor, m_questMask[m_localPlayerIndex]))
                blocking = Quest::forZone(z->poiFloor);
        }
        if (blocking) {
            char line[128];
            snprintf(line, sizeof(line), "Unfinished business: %s", blocking->name);
            addChatMessage("", line, Vec3{0.85f, 0.7f, 0.4f});
        } else {
            addChatMessage("", "The way onward is not open yet.", Vec3{0.85f, 0.7f, 0.4f});
        }
        m_zoneGateHintTimer = 6.0f;
    }
    return false;
}

// Walking into a border band with a linked neighbour hands off to the next world. Host/SP only —
// clients follow the SV_LEVEL_SEED broadcast, exactly as they do for the town portal, so the party
// travels together and no client can move itself between worlds unilaterally.
void Engine::updateZoneTransitions() {
    if (m_netRole == NetRole::CLIENT) return;   // the host owns world changes; guests follow the seed

    zoneRememberState();   // survivors + boss, so re-entering does not undo the fight

    // THE TOWN'S NORTH GATE — the front door to Act 1, and the only way into the overworld in normal
    // play (the --zone dev door aside). Gated on the INFERNO clear here rather than in the town's
    // geometry, so every peer still builds the identical town (see buildTownLevel).
    if (m_level.inTown) {
        if (m_localPlayer.position.z > EDGE_TRIGGER_BAND) return;
        if (!FreePlay::overworldUnlocked(m_level.savedFloor, m_difficulty)) {
            // Say why, once in a while — a gate you can walk to and not through is otherwise just a
            // bug as far as the player is concerned.
            if (m_zoneGateHintTimer <= 0.0f) {
                addChatMessage("", "The road north is closed until Inferno is broken.",
                               Vec3{0.8f, 0.7f, 0.45f});
                m_zoneGateHintTimer = 6.0f;
            }
            return;
        }
        enterZone(52, Zone::TOWN_FLOOR);
        return;
    }

    if (!m_level.inZone) return;
    const Zone::ZoneDef* def = Zone::find(m_level.zoneFloor);
    if (!def) return;

    const f32 W = static_cast<f32>(m_level.grid.width), D = static_cast<f32>(m_level.grid.depth);
    const Vec3 p = m_localPlayer.position;

    // DISARMED until the player stands clear of every border band. Placing the arrival deeper (above)
    // fixes the arithmetic, but only this makes the rule robust: any future gate position, grid size
    // or knockback that leaves a player inside a band on arrival would otherwise re-open the loop.
    // The RE-ARM test uses a wider band than the trigger. Re-arming the moment you are technically
    // out of the trigger band leaves you one step from being sent back, which is how a fight next to
    // a border turns into a world rebuild. Arming only once genuinely clear means a transition is
    // always something you travelled to.
    constexpr f32 REARM_BAND = EDGE_TRIGGER_BAND * 2.0f;
    const f32 armBand = m_zoneEdgeArmed ? EDGE_TRIGGER_BAND : REARM_BAND;
    const bool inAnyBand = (p.z <= armBand) || (p.z >= D - armBand) ||
                           (p.x <= armBand) || (p.x >= W - armBand);
    if (!m_zoneEdgeArmed) {
        if (inAnyBand) return;          // still standing where we arrived — do not re-fire
        m_zoneEdgeArmed = true;         // clear of the border: transitions are live again
    }
    // Which border band are we standing in, if any? The gate opening is the only walkable part of a
    // border wall, so simply being this close to an edge means being IN its gate.
    Zone::Dir dir = Zone::Dir::COUNT;
    if      (p.z <= EDGE_TRIGGER_BAND)      dir = Zone::Dir::NORTH;
    else if (p.z >= D - EDGE_TRIGGER_BAND)  dir = Zone::Dir::SOUTH;
    else if (p.x <= EDGE_TRIGGER_BAND)      dir = Zone::Dir::WEST;
    else if (p.x >= W - EDGE_TRIGGER_BAND)  dir = Zone::Dir::EAST;
    if (dir == Zone::Dir::COUNT) return;

    const u8 dest = def->neighbour[static_cast<u8>(dir)];
    if (dest == Zone::NO_LINK) return;

    const u8 from = m_level.zoneFloor;
    if (dest == Zone::TOWN_FLOOR) { enterTown(); return; }
    if (!zoneLinkAllowed(from, dest)) return;
    // Logged HERE, not at the proximity test: standing in a border band is a per-tick condition, so
    // logging the attempt produced a dozen identical lines a second in the first live run. What is
    // worth a line is the world actually changing.
    LOG_INFO("[ZONEX] crossing %u -> %u  p=(%.1f,%.1f) dir=%u",
             static_cast<u32>(from), static_cast<u32>(dest),
             static_cast<f64>(p.x), static_cast<f64>(p.z), static_cast<u32>(dir));
    enterZone(dest, from);
}

// Host/SP entry. `fromFloor` is where the player came from (0 = unknown, e.g. a waypoint jump).
void Engine::enterZone(u8 zoneFloor, u8 fromFloor) {
    const Zone::ZoneDef* def = Zone::find(zoneFloor);
    if (!def) {   // corrupt save, hostile packet, or a table edit that lost a row
        LOG_WARN("enterZone: floor %u is not a zone — staying put", static_cast<u32>(zoneFloor));
        return;
    }

    worldResetPools();
    // CLEAR BEFORE BUILDING. worldClearLevelFlags() resets m_level.layoutStyle to BSP_ROOMS (it is
    // the shared "which special world am I in" reset), so running it AFTER buildZoneLevel threw away
    // the style the zone had just recorded — every zone reported `rooms` for the rest of its life.
    // Harmless by luck (wilderness and rooms are both FLAT, so every consumer that branches on style
    // — the AI's open-floor bubble, the bot's stacked-floor tests — happened to want the same answer)
    // but wrong, and it made the stall telemetry lie about which floor you were looking at.
    worldClearLevelFlags();
    const Vec3 center = buildZoneLevel(*def);
    m_level.inZone    = true;
    m_level.zoneFloor = zoneFloor;
    m_zoneEdgeArmed   = false;   // re-arms once the player steps clear of the border

    const Vec3 arrive = zoneArrivalPos(*def, fromFloor);
    worldPlaceLocalPlayers(arrive, 0.0f);
    worldSeedHostSlot();      // never omit — see engine_world.cpp
    // ARRIVING somewhere and REVIVING there are different questions, and spawnPosition used to
    // answer both with the arrival point. Arriving wants continuity of travel, so an edge crossing
    // puts you in the gate you walked through — 5 m from a border whose transition band is 2.5 m.
    // That is fine to walk out of, and a terrible place to be dropped by a death: you come back in
    // the doorway, take two steps the wrong way under whatever killed you, and cross straight back —
    // landing at the neighbour's gate, also 5 m from the same seam, which bounces you again. That is
    // the "respawning teleports me from zone to zone" report.
    // So a death always returns you to the zone's OWN interior arrival point (the one waypoint
    // travel uses), which is a deterministic cleared pad 12-16 m from the nearest border.
    worldSeatNetPlayers(arrive, zoneArrivalPos(*def, /*fromFloor=*/0));

    spawnZoneContents(*def, center);
    worldFinishEntry(zoneFloor, def->peaceful);
    questOnZoneEnter(zoneFloor);
    LOG_INFO("Entered zone %u '%s' (host).", static_cast<u32>(zoneFloor), def->name);
}

// Client mirror, driven by the sentinel-floor SV_LEVEL_SEED (see onLevelSeed).
void Engine::enterZoneClient(u8 zoneFloor) {
    const Zone::ZoneDef* def = Zone::find(zoneFloor);
    if (!def) {
        LOG_WARN("enterZoneClient: unknown zone floor %u", static_cast<u32>(zoneFloor));
        return;
    }
    worldResetPools();
    // CLEAR BEFORE BUILDING. worldClearLevelFlags() resets m_level.layoutStyle to BSP_ROOMS (it is
    // the shared "which special world am I in" reset), so running it AFTER buildZoneLevel threw away
    // the style the zone had just recorded — every zone reported `rooms` for the rest of its life.
    // Harmless by luck (wilderness and rooms are both FLAT, so every consumer that branches on style
    // — the AI's open-floor bubble, the bot's stacked-floor tests — happened to want the same answer)
    // but wrong, and it made the stall telemetry lie about which floor you were looking at.
    worldClearLevelFlags();
    const Vec3 center = buildZoneLevel(*def);
    m_level.inZone    = true;
    m_level.zoneFloor = zoneFloor;
    m_zoneEdgeArmed   = false;   // re-arms once the player steps clear of the border

    // The client does not know which edge the host came through; the server's snapshot moves it to
    // the right place on the first tick, so any in-bounds spot will do until then.
    const Vec3 arrive = zoneArrivalPos(*def, 0);
    worldPlaceLocalPlayers(arrive, 0.0f);
    // Seat the anchors too, even though the server owns respawns. A guest PREDICTS its own revive
    // (sendRespawnRequest + an immediate local teleport), and spawnPosition is not on the wire — so
    // without this the prediction reads an anchor left over from the world the guest was in BEFORE,
    // i.e. a coordinate from a different grid. The server corrects it a tick later, but a frame
    // spent inside rock or off the map is exactly the artefact players report as a bad respawn.
    worldSeatNetPlayers(arrive);
    spawnZoneContents(*def, center);
    worldFinishEntry(zoneFloor, def->peaceful);
    questOnZoneEnter(zoneFloor);
    LOG_INFO("Entered zone %u '%s' (client).", static_cast<u32>(zoneFloor), def->name);
}
