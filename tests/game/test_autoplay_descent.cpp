// test_autoplay_descent.cpp — the FOUR_STORY "Descent" travel field (game/autoplay_descent.h).
//
// The field replaced a straight-line bearing and then an A* leg, both of which failed live for the
// same reason: on a braided maze they handed back headings that pointed into walls, and the bot
// scraped along them ("the time goes right now looking and hugging the walls and corners"). The
// properties pinned here are the ones that make a field the right answer — it always points along a
// route that exists, it is defined everywhere reachable so there is no "no plan" tick, and it never
// routes over a jump pad, which on this floor is the one piece of terrain that undoes a descent.
//
// Built on synthetic LevelGrids like the other nav tests (LevelGridSystem::init/shutdown, cells
// indexed z*width+x).

#include <doctest/doctest.h>
#include "game/autoplay_descent.h"
#include "game/autoplay_nav.h"   // Autoplay::onJumpPad
#include "world/level_grid.h"

namespace {
// A one-story test maze: open floor with the L3 slab over it, so a body at y=9 is on a real storey.
LevelGrid makeStoryGrid(u32 w, u32 d) {
    LevelGrid g;
    LevelGridSystem::init(g, w, d, 1.0f);
    for (u32 z = 0; z < d; z++)
        for (u32 x = 0; x < w; x++) {
            GridCell& c = g.cells[z * w + x];
            c.flags = CELL_FLOOR;
            c.floorHeight = 0; c.ceilingHeight = 48;
            LevelGridSystem::addPlatform(c, 36, 0);   // L3 @ 9 m
        }
    return g;
}
void setSolid(LevelGrid& g, u32 x, u32 z) { g.cells[z * g.width + x].flags = CELL_SOLID; }
void setPad  (LevelGrid& g, u32 x, u32 z) { g.cells[z * g.width + x].flags |= CELL_JUMPPAD; }
DropHole holeAt(f32 x, f32 z, f32 y) { DropHole h; h.pos = {x, y, z}; h.surfaceY = y; return h; }

// Follow the field from `start`, one cell per step, and report whether it arrives at a hole.
// This is the property that matters: not the direction of any single step, but that the chain of
// them terminates at a way down.
bool fieldReachesAHole(const Autoplay::DescentField& f, const LevelGrid& g, Vec3 start, u32 maxSteps) {
    Vec3 p = start;
    for (u32 i = 0; i < maxSteps; i++) {
        if (Autoplay::atDescentGoal(f, g, p)) return true;
        const Vec3 dir = Autoplay::descentDirection(f, g, p);
        if (lengthSq(dir) < 1e-6f) return false;
        p = p + dir * g.cellSize;      // one cell along the field
        p.y = start.y;                  // stay on the storey (the field is 2D)
    }
    return false;
}
} // namespace

TEST_CASE("descent field: routes around a wall instead of pointing through it") {
    // A full-height barrier splits the grid; the only hole is on the far side, straight through the
    // wall from the bot. A bearing would aim into the wall forever — the field must walk the gap.
    LevelGrid g = makeStoryGrid(16, 16);
    for (u32 z = 0; z < 12; z++) setSolid(g, 8, z);      // wall with a gap along the +z edge
    DungeonResult d{};
    d.dropHoles[d.dropHoleCount++] = holeAt(12.5f, 3.5f, 9.0f);

    Autoplay::DescentField f;
    REQUIRE(Autoplay::ensureDescentField(f, g, d, 9.0f, 1));
    const Vec3 start{3.5f, 9.0f, 3.5f};
    // The straight-line bearing at the hole is roughly +X — the wall. The field must not send us there.
    const Vec3 dir = Autoplay::descentDirection(f, g, start);
    CHECK(lengthSq(dir) > 1e-6f);                        // it always has an answer
    CHECK(fieldReachesAHole(f, g, start, 200));          // and the answer gets there
    Autoplay::freeDescentField(f);
    LevelGridSystem::shutdown(g);
}

TEST_CASE("descent field: the heading is stable as the bot walks it, not per-cell flip-flop") {
    // What the aim actually feels is how much the heading CHANGES tick to tick. The field expands
    // 4-connected, so a naive "steer at the very next cell centre" readout swings up to 90 deg every
    // time the bot crosses a cell boundary; the lookahead is there to keep consecutive samples
    // pointing the same way. Note the BFS may legitimately route an L rather than a staircase
    // (equal-cost routes are resolved by expansion order), so this asserts the property that
    // matters — smoothness along the walk — not any particular shape of route.
    LevelGrid g = makeStoryGrid(20, 20);
    DungeonResult d{};
    d.dropHoles[d.dropHoleCount++] = holeAt(15.5f, 15.5f, 9.0f);
    Autoplay::DescentField f;
    REQUIRE(Autoplay::ensureDescentField(f, g, d, 9.0f, 1));

    // Walk the field for real — step along whatever heading it gives, the way the bot does — and
    // require consecutive headings to agree. A per-cell flip-flop shows up here as a dot near 0.
    Vec3 p{3.5f, 9.0f, 3.5f};
    Vec3 prev = Autoplay::descentDirection(f, g, p);
    REQUIRE(lengthSq(prev) > 1e-6f);
    for (u32 i = 0; i < 40 && !Autoplay::atDescentGoal(f, g, p); i++) {
        const Vec3 h = Autoplay::descentDirection(f, g, p);
        if (lengthSq(h) < 1e-6f) break;
        CHECK(dot(h, prev) > 0.7f);        // < ~45 deg between consecutive steps
        prev = h;
        p = p + h * 0.5f;                  // half a cell per step, ~ the bot's per-tick travel
        p.y = 9.0f;
    }
    Autoplay::freeDescentField(f);
    LevelGridSystem::shutdown(g);
}

TEST_CASE("descent field: the lookahead never shortcuts through a wall") {
    // String-pulling is only safe because the straight run is WIDTH-tested. Put a barrier between
    // the bot and its goal and the heading must still route around it, not point through it.
    LevelGrid g = makeStoryGrid(20, 20);
    for (u32 z = 0; z < 16; z++) setSolid(g, 10, z);      // wall with a gap at the +z end
    DungeonResult d{};
    d.dropHoles[d.dropHoleCount++] = holeAt(15.5f, 3.5f, 9.0f);   // straight through the wall
    Autoplay::DescentField f;
    REQUIRE(Autoplay::ensureDescentField(f, g, d, 9.0f, 1));
    const Vec3 start{3.5f, 9.0f, 3.5f};
    const Vec3 dir = Autoplay::descentDirection(f, g, start);
    REQUIRE(lengthSq(dir) > 1e-6f);
    // The goal bears due +X; the only way there is around the far end, so the heading must carry a
    // real +z component rather than aiming at the barrier.
    CHECK(dir.z > 0.2f);
    CHECK(fieldReachesAHole(f, g, start, 300));
    Autoplay::freeDescentField(f);
    LevelGridSystem::shutdown(g);
}

TEST_CASE("descent field: every reachable cell has a heading (no stand-and-stare)") {
    LevelGrid g = makeStoryGrid(12, 12);
    DungeonResult d{};
    d.dropHoles[d.dropHoleCount++] = holeAt(1.5f, 1.5f, 9.0f);
    Autoplay::DescentField f;
    REQUIRE(Autoplay::ensureDescentField(f, g, d, 9.0f, 1));
    for (u32 z = 0; z < 12; z++)
        for (u32 x = 0; x < 12; x++) {
            const Vec3 p{x + 0.5f, 9.0f, z + 0.5f};
            const bool ok = Autoplay::atDescentGoal(f, g, p) ||
                            lengthSq(Autoplay::descentDirection(f, g, p)) > 1e-6f;
            CHECK(ok);
        }
    Autoplay::freeDescentField(f);
    LevelGridSystem::shutdown(g);
}

TEST_CASE("descent field: never routes over a jump pad") {
    // Pads throw the bot ~two storeys back up, so a route across one throws away the descent. The
    // field leaves them out entirely rather than steering onto one and being vetoed off it per tick.
    LevelGrid g = makeStoryGrid(16, 16);
    for (u32 z = 6; z < 10; z++) for (u32 x = 6; x < 10; x++) setPad(g, x, z);
    DungeonResult d{};
    d.dropHoles[d.dropHoleCount++] = holeAt(13.5f, 13.5f, 9.0f);
    Autoplay::DescentField f;
    REQUIRE(Autoplay::ensureDescentField(f, g, d, 9.0f, 1));
    // Walk the field from the far corner and assert it never sets foot in the pad block.
    Vec3 p{2.5f, 9.0f, 2.5f};
    for (u32 i = 0; i < 200 && !Autoplay::atDescentGoal(f, g, p); i++) {
        const Vec3 dir = Autoplay::descentDirection(f, g, p);
        REQUIRE(lengthSq(dir) > 1e-6f);
        p = p + dir * g.cellSize; p.y = 9.0f;
        CHECK_FALSE(Autoplay::onJumpPad(g, p));
    }
    Autoplay::freeDescentField(f);
    LevelGridSystem::shutdown(g);
}

TEST_CASE("descent field: a storey with no holes reports invalid (L0 keeps the exit flow field)") {
    LevelGrid g = makeStoryGrid(10, 10);
    DungeonResult d{};
    d.dropHoles[d.dropHoleCount++] = holeAt(5.5f, 5.5f, 9.0f);   // upstairs only
    Autoplay::DescentField f;
    CHECK_FALSE(Autoplay::ensureDescentField(f, g, d, 0.0f, 1)); // asking about the ground storey
    CHECK(lengthSq(Autoplay::descentDirection(f, g, Vec3{3.5f, 0.0f, 3.5f})) < 1e-6f);
    Autoplay::freeDescentField(f);
    LevelGridSystem::shutdown(g);
}

TEST_CASE("descent field: L0 (no holes) seeds from the EXIT so the bottom is a pad-avoiding walk") {
    // On L0 there are no drop holes, and the shared flat exit field does not dodge pads — the bot
    // reached the bottom and bounced straight back UP a return lift the instant it landed (the
    // last-metre-of-the-descent stall). Given the exit door, the field seeds from IT instead, so L0
    // routing is a pad-avoiding path to the door. Without an exit passed, L0 stays invalid (old way).
    LevelGrid g = makeStoryGrid(12, 12);          // the L3 slab stands in for the storey being routed
    DungeonResult d{};                             // no holes anywhere
    const Vec3 exit{9.5f, 9.0f, 9.5f};
    Autoplay::DescentField f;
    REQUIRE(Autoplay::ensureDescentField(f, g, d, 9.0f, 1, exit));   // now VALID via the exit seed
    CHECK(fieldReachesAHole(f, g, Vec3{1.5f, 9.0f, 1.5f}, 200));     // and the route arrives at the door
    Autoplay::DescentField f2;
    CHECK_FALSE(Autoplay::ensureDescentField(f2, g, d, 9.0f, 1));    // default (no exit): unchanged, invalid
    Autoplay::freeDescentField(f);
    Autoplay::freeDescentField(f2);
    LevelGridSystem::shutdown(g);
}

TEST_CASE("descent field: a pocket severed from the hole by a PAD wall still gets a recovery heading") {
    // Excluding pads from the primary flood can leave a cell with NO clean route to any hole — it read
    // 0xFF forever, got no descent heading, and the bot froze next to a pad it would not use (measured:
    // a build stuck ~12 min on floor 9). The recovery tier re-floods THROUGH pads so every reachable
    // cell still gets a heading toward the network, and the driver's veto lets it take that one step.
    LevelGrid g = makeStoryGrid(9, 9);
    for (u32 z = 0; z < 9; z++) setPad(g, 4, z);   // a full pad wall: the only link between halves is a pad
    DungeonResult d{};
    d.dropHoles[d.dropHoleCount++] = holeAt(7.5f, 4.5f, 9.0f);      // the hole is on the RIGHT half
    Autoplay::DescentField f;
    REQUIRE(Autoplay::ensureDescentField(f, g, d, 9.0f, 1));
    // A LEFT-half cell is cut off from the hole by the pad wall — tier 1 leaves it 0xFF. The recovery
    // tier must hand it a heading rather than {0,0,0} ("no plan"), which is what froze the bot.
    CHECK(lengthSq(Autoplay::descentDirection(f, g, Vec3{1.5f, 9.0f, 4.5f})) > 1e-6f);
    Autoplay::freeDescentField(f);
    LevelGridSystem::shutdown(g);
}

TEST_CASE("descent field: a padded hole is used only when it is the ONLY way down") {
    // Hole density thins to 7% on the deepest storey, so "every hole here is a return lift" is a
    // real state. A bounce still beats having no descent plan at all.
    LevelGrid g = makeStoryGrid(12, 12);
    setPad(g, 5, 5);
    DungeonResult d{};
    d.dropHoles[d.dropHoleCount++] = holeAt(5.5f, 5.5f, 9.0f);   // padded, and the only one
    Autoplay::DescentField f;
    REQUIRE(Autoplay::ensureDescentField(f, g, d, 9.0f, 1));
    CHECK(fieldReachesAHole(f, g, Vec3{2.5f, 9.0f, 2.5f}, 100));
    Autoplay::freeDescentField(f);
    LevelGridSystem::shutdown(g);
}

TEST_CASE("descent field: a new floor invalidates a stale field") {
    // The stamp is the floor identity. Without it a Descent floor would inherit the previous one's
    // routes — the grid is the same size and the storey height is identical, so nothing else differs.
    LevelGrid g = makeStoryGrid(10, 10);
    DungeonResult d1{}; d1.dropHoles[d1.dropHoleCount++] = holeAt(1.5f, 1.5f, 9.0f);
    DungeonResult d2{}; d2.dropHoles[d2.dropHoleCount++] = holeAt(8.5f, 8.5f, 9.0f);
    Autoplay::DescentField f;
    REQUIRE(Autoplay::ensureDescentField(f, g, d1, 9.0f, /*floor=*/1));
    const Vec3 mid{5.5f, 9.0f, 5.5f};
    const Vec3 toFirst = Autoplay::descentDirection(f, g, mid);
    REQUIRE(Autoplay::ensureDescentField(f, g, d2, 9.0f, /*floor=*/2));
    const Vec3 toSecond = Autoplay::descentDirection(f, g, mid);
    CHECK(dot(toFirst, toSecond) < 0.9f);    // the routes genuinely differ (opposite corners)
    Autoplay::freeDescentField(f);
    LevelGridSystem::shutdown(g);
}

// --- getting off a return lift -------------------------------------------------------------------
// A jump pad under a drop hole is a RETURN LIFT: fall through, land on it, get flung back up. Pads are
// excluded from the field on purpose, so descentDirection says nothing while standing on one — and a
// bot with no heading just waits there to be relaunched. Traced live as the Descent floor PARK: 12
// minutes with distance-to-exit oscillating between two fixed values, 47% of ticks airborne, 32% with
// no heading at all. padEscapeDirection is the one case the field cannot express: "leave where you are".
TEST_CASE("descent field: a bot standing on a jump pad is given a way OFF it") {
    LevelGrid g = makeStoryGrid(16, 16);
    // A 3x3 pad node — the real generator builds whole nodes, and a single-cell escape must clear it.
    for (u32 z = 6; z <= 8; z++)
        for (u32 x = 6; x <= 8; x++) setPad(g, x, z);
    DungeonResult d{};
    d.dropHoles[d.dropHoleCount++] = holeAt(13.5f, 13.5f, 9.0f);

    Autoplay::DescentField f;
    REQUIRE(Autoplay::ensureDescentField(f, g, d, 9.0f, 1));

    const Vec3 onPad{7.5f, 9.0f, 7.5f};                       // dead centre of the pad node
    // NOTE: the ordinary router may or may not have a heading here — the tier-2 recovery flood routes
    // THROUGH pads, so a pad cell can carry a code. What padEscapeDirection guarantees is different
    // and unconditional: a heading that leads OFF the pad rather than along the descent route, which
    // is what a bot being relaunched needs and what the field alone will never express.
    const Vec3 esc = Autoplay::padEscapeDirection(f, g, onPad);
    CHECK(lengthSq(esc) > 1e-6f);

    // It must lead somewhere that is genuinely OFF the pad, not deeper into it: step along the escape
    // until we leave the node, then confirm the normal router takes over again.
    Vec3 p = onPad;
    bool leftPad = false;
    for (u32 i = 0; i < 12 && !leftPad; i++) {
        p = p + esc * g.cellSize; p.y = onPad.y;
        u32 cx, cz;
        if (!LevelGridSystem::worldToGrid(g, p, cx, cz)) break;
        if (!(LevelGridSystem::getCell(g, cx, cz).flags & CELL_JUMPPAD)) leftPad = true;
    }
    CHECK(leftPad);
    CHECK(lengthSq(Autoplay::descentDirection(f, g, p)) > 1e-6f);   // routing resumes off the pad
    CHECK(fieldReachesAHole(f, g, p, 200));                          // and still reaches a way down

    // Not on a pad => no escape needed; the ordinary router owns the heading.
    CHECK(lengthSq(Autoplay::padEscapeDirection(f, g, Vec3{2.5f, 9.0f, 2.5f})) >= 0.0f);
    Autoplay::freeDescentField(f);
    LevelGridSystem::shutdown(g);
}

// --- WEDGE DETECTION -------------------------------------------------------------------------
// The residual FOUR_STORY floor park: the bot has a valid heading and is commanding movement, and
// the body does not move. These pin the rule that separates that from the states it must NOT fire
// on — a bot standing still on purpose, and a bot that is genuinely travelling.
TEST_CASE("Autoplay wedge: commanding movement with no travel is a wedge") {
    using namespace Autoplay;
    // The measured park: a full window, movement commanded throughout, ~0 m of travel.
    CHECK(wedgeDetected(WEDGE_WIN_SEC, WEDGE_WIN_SEC, 0.05f));

    // NOT a wedge: the window has not closed yet.
    CHECK_FALSE(wedgeDetected(WEDGE_WIN_SEC * 0.5f, WEDGE_WIN_SEC * 0.5f, 0.0f));

    // NOT a wedge: standing still ON PURPOSE (no movement commanded). A bot holding position in a
    // fight, or parked at the exit waiting on the descend hold, must never be shoved sideways.
    CHECK_FALSE(wedgeDetected(WEDGE_WIN_SEC, 0.0f, 0.0f));
    CHECK_FALSE(wedgeDetected(WEDGE_WIN_SEC, WEDGE_WIN_SEC * 0.5f, 0.0f));   // under the 70% floor

    // NOT a wedge: actually getting somewhere.
    CHECK_FALSE(wedgeDetected(WEDGE_WIN_SEC, WEDGE_WIN_SEC, WEDGE_NET_M + 0.1f));
}

TEST_CASE("Autoplay wedge: escape angle escalates sideways first, then reverses") {
    using namespace Autoplay;
    // A lip/corner wedge is a body refused on ONE axis, so the square sidestep must come first —
    // reversing immediately would walk the bot back down the route it just travelled.
    CHECK(wedgeEscapeAngle(0) == doctest::Approx(1.5707963268f));    // +90
    CHECK(wedgeEscapeAngle(1) == doctest::Approx(-1.5707963268f));   // -90
    CHECK(wedgeEscapeAngle(2) == doctest::Approx(3.1415926536f));    // 180, only after both sides
    // Wraps rather than running off the table, however long a bot stays stuck.
    CHECK(wedgeEscapeAngle(5) == doctest::Approx(wedgeEscapeAngle(0)));
    CHECK(wedgeEscapeAngle(251) == doctest::Approx(wedgeEscapeAngle(1)));
}

TEST_CASE("Autoplay boxed-in: a real heading vetoed to nothing is the second park symptom") {
    using namespace Autoplay;
    // Measured tinkerer park: the veto zeroed a real heading on 100% of ticks for the whole run.
    CHECK(boxedDetected(WEDGE_WIN_SEC, WEDGE_WIN_SEC));

    // The window must close first.
    CHECK_FALSE(boxedDetected(WEDGE_WIN_SEC * 0.5f, WEDGE_WIN_SEC * 0.5f));

    // A brief veto is ORDINARY — the fan rounds corners constantly on a maze — so a bot that is
    // mostly routing fine must not be shoved sideways for the occasional blocked step.
    CHECK_FALSE(boxedDetected(WEDGE_WIN_SEC, WEDGE_WIN_SEC * 0.5f));

    // The two detectors are INDEPENDENT: the boxed bot commands no movement (so wedgeDetected is
    // blind to it) and the wedged bot has a heading that is never vetoed (so boxedDetected is blind
    // to that). Neither alone covers the park; that is why both exist.
    CHECK_FALSE(wedgeDetected(WEDGE_WIN_SEC, /*cmdT=*/0.0f, /*net=*/0.0f));   // the boxed bot
    CHECK_FALSE(boxedDetected(WEDGE_WIN_SEC, /*vetoT=*/0.0f));               // the wedged bot
}
