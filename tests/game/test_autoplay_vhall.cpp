// test_autoplay_vhall.cpp — the VERTICAL_HALL two-story travel field (game/autoplay_vhall.h).
//
// The field replaced a flat 2D flow field plus a pile of ramp heuristics that could not represent
// two stories and routed a bot that had climbed to a balcony straight back off its edge. The
// properties pinned here are the ones that make a (cell, story) field the right answer: it connects
// a GROUND spawn to an UPPER door via the ramp, and it NEVER connects a balcony cell to the ground
// beside it (so it can never steer the bot off an edge).
//
// Built on a synthetic VHALL-shaped grid: a flat ground plane, a graduated-slab RAMP rising from the
// ground to a 3 m balcony, and a balcony slab holding the door. Same construction pattern as the
// other nav tests (LevelGridSystem::init/shutdown, cells z*width+x).

#include <doctest/doctest.h>
#include "game/autoplay_vhall.h"
#include "world/level_grid.h"
#include "world/level_gen.h"

namespace {
// A flat all-ground grid at height 0.
LevelGrid makeGroundGrid(u32 w, u32 d) {
    LevelGrid g;
    LevelGridSystem::init(g, w, d, 1.0f);
    for (u32 z = 0; z < d; z++)
        for (u32 x = 0; x < w; x++) {
            GridCell& c = g.cells[z * w + x];
            c.flags = CELL_FLOOR; c.floorHeight = 0; c.ceilingHeight = 48;
        }
    return g;
}
void addSlab(LevelGrid& g, u32 x, u32 z, u8 topQ) { LevelGridSystem::addPlatform(g.cells[z * g.width + x], topQ, 0); }

// A DungeonResult with no recorded jump links: these synthetic grids carve none, and the field must
// behave exactly as before on a link-free floor.
const DungeonResult kNoLinks{};

// Follow the field from `start` (given its feet height) one cell per step; report whether the chain
// arrives at the door. The property that matters: the route exists and terminates at the goal.
bool fieldReachesDoor(const Autoplay::VHallField& f, const LevelGrid& g, Vec3 start, u32 maxSteps) {
    Vec3 p = start;
    for (u32 i = 0; i < maxSteps; i++) {
        if (Autoplay::atVHallGoal(f, g, p)) return true;
        const Vec3 dir = Autoplay::vhallDirection(f, g, p);
        if (lengthSq(dir) < 1e-6f) return false;
        p = p + dir * g.cellSize;
        // Mimic the physics: the feet settle onto whatever surface the destination cell offers within
        // a step (the ramp/balcony slab if present and reachable, else the ground).
        u32 gx, gz;
        if (LevelGridSystem::worldToGrid(g, p, gx, gz))
            p.y = LevelGridSystem::effectiveFloorHeight(g, gx, gz, p.y);
    }
    return false;
}
} // namespace

TEST_CASE("vhall field: routes a ground spawn up a ramp to an upper-story door") {
    // Layout (20x8): ground everywhere. A ramp climbs along +x at row z=4 from the foot (x=5, slab
    // 0.25 m) to the top (x=16, slab 3 m), each cell 0.25 m higher than the last. A 3 m balcony slab
    // fills x=16..18 at z=4 (the ramp top plus the door tile). The door sits on the balcony at x=18.
    LevelGrid g = makeGroundGrid(20, 8);
    for (u32 x = 5; x <= 16; x++) addSlab(g, x, 4, static_cast<u8>((x - 5 + 1)));   // 1..12 q = 0.25..3 m
    for (u32 x = 16; x <= 18; x++) addSlab(g, x, 4, 12);                            // balcony @ 3 m
    // widen the ramp/balcony to 2 rows so the body has room (z=3 and z=4)
    for (u32 x = 5; x <= 16; x++) addSlab(g, x, 3, static_cast<u8>((x - 5 + 1)));
    for (u32 x = 16; x <= 18; x++) addSlab(g, x, 3, 12);

    const Vec3 door{18.5f, 3.0f, 4.5f};
    Autoplay::VHallField f;
    REQUIRE(Autoplay::ensureVHallField(f, g, kNoLinks, door, 1, /*useJumpLinks=*/true));

    // From a far ground corner the field must produce a route that climbs the ramp and reaches the
    // door — a flat field could never (the door cell's node is 3 m up, disconnected from the ground).
    const Vec3 spawn{2.5f, 0.0f, 1.5f};
    CHECK(lengthSq(Autoplay::vhallDirection(f, g, spawn)) > 1e-6f);   // it has a plan on the ground
    CHECK(fieldReachesDoor(f, g, spawn, 400));

    Autoplay::freeVHallField(f);
    LevelGridSystem::shutdown(g);
}

TEST_CASE("vhall field: a balcony cell never routes toward the open ground beside it") {
    // Standing on the balcony top, the field must point ALONG the balcony toward the door, never off
    // its edge onto the 0 m ground — that off-the-edge route is exactly what the old flat field took.
    LevelGrid g = makeGroundGrid(20, 8);
    for (u32 x = 5; x <= 16; x++) { addSlab(g, x, 4, static_cast<u8>((x - 5 + 1))); addSlab(g, x, 3, static_cast<u8>((x - 5 + 1))); }
    for (u32 x = 16; x <= 18; x++) { addSlab(g, x, 4, 12); addSlab(g, x, 3, 12); }
    const Vec3 door{18.5f, 3.0f, 4.5f};
    Autoplay::VHallField f;
    REQUIRE(Autoplay::ensureVHallField(f, g, kNoLinks, door, 1, /*useJumpLinks=*/true));

    // On the balcony at (17,4) @ 3 m: the heading must have a +x component (toward the door at x=18),
    // and must NOT push the bot off the balcony in +z/-z toward the bare ground rows (z=2 / z=5).
    const Vec3 onBalcony{17.5f, 3.0f, 4.5f};
    const Vec3 dir = Autoplay::vhallDirection(f, g, onBalcony);
    REQUIRE(lengthSq(dir) > 1e-6f);
    CHECK(dir.x > 0.3f);                 // toward the door
    // The next cell it steers to must itself be a balcony (has a slab), never bare ground.
    u32 nx, nz;
    REQUIRE(LevelGridSystem::worldToGrid(g, onBalcony + dir * g.cellSize, nx, nz));
    CHECK(LevelGridSystem::hasPlatform(g, nx, nz));

    Autoplay::freeVHallField(f);
    LevelGridSystem::shutdown(g);
}

TEST_CASE("vhall field: a fresh floor stamp invalidates a stale field") {
    LevelGrid g = makeGroundGrid(12, 12);
    for (u32 x = 3; x <= 8; x++) { addSlab(g, x, 6, static_cast<u8>((x - 3 + 1) * 2)); }   // a ramp
    addSlab(g, 8, 6, 12);
    Autoplay::VHallField f;
    REQUIRE(Autoplay::ensureVHallField(f, g, kNoLinks, Vec3{8.5f, 3.0f, 6.5f}, /*stamp=*/1, /*useJumpLinks=*/true));
    const bool wasValid = f.valid;
    // Same grid, new floor stamp: must rebuild (not silently reuse the old routes). We can only
    // observe that it stays valid and does not crash; the stamp field is what forces the rebuild.
    REQUIRE(Autoplay::ensureVHallField(f, g, kNoLinks, Vec3{8.5f, 3.0f, 6.5f}, /*stamp=*/2, /*useJumpLinks=*/true));
    CHECK(wasValid);
    CHECK(f.stamp == 2);
    Autoplay::freeVHallField(f);
    LevelGridSystem::shutdown(g);
}

// ---------------------------------------------------------------------------------------------
// Jump-link edges + per-node distance + the node-committed follower.

namespace {
// A broken-catwalk grid: two 3 m arms along z=4/5 with a 2-cell void gap between them, the door on
// the far (west) arm, ground everywhere else. Mirrors the real carve's shape: east arm x 10..16,
// GAP x 8..9, west arm x 5..7, all at 12 q = 3.0 m, two rows wide.
struct CatwalkRig {
    LevelGrid g;
    DungeonResult dg{};
    CatwalkRig() {
        g = makeGroundGrid(20, 10);
        for (u32 z = 4; z <= 5; z++) {
            for (u32 x = 10; x <= 16; x++) addSlab(g, x, z, 12);   // east arm
            for (u32 x = 5;  x <= 7;  x++) addSlab(g, x, z, 12);   // west arm (holds the door)
        }
        // Recorded links, one per row — lips at x=7 (west) and x=10 (east), exactly as the carve
        // records them from its own loop variables.
        for (u32 z = 4; z <= 5; z++)
            dg.jumpLinks[dg.jumpLinkCount++] = { {7.5f, 3.0f, z + 0.5f}, {10.5f, 3.0f, z + 0.5f} };
    }
    ~CatwalkRig() { LevelGridSystem::shutdown(g); }
};
} // namespace

TEST_CASE("vhall field: a jump link routes across the broken catwalk when it shortens the route") {
    CatwalkRig r;
    const Vec3 door{5.5f, 3.0f, 4.5f};   // west arm — UNREACHABLE from the east arm without the link
    Autoplay::VHallField f;

    SUBCASE("with links: the east arm routes to the gap lip and the lip node says JUMP") {
        REQUIRE(Autoplay::ensureVHallField(f, r.g, r.dg, door, 1, /*useJumpLinks=*/true));
        // Standing mid-east-arm at 3 m: a route exists (dist finite) and walking it westward
        // arrives at the lip, whose step is a JUMP targeting the west lip.
        const Vec3 onArm{14.5f, 3.0f, 4.5f};
        Autoplay::VHallStep st = Autoplay::vhallNextStep(f, r.g, r.dg, onArm);
        REQUIRE(st.valid);
        CHECK_FALSE(st.jump);                       // mid-arm: an ordinary walk step...
        CHECK(st.distHere != 0xFFFF);
        const Vec3 onLip{10.5f, 3.0f, 4.5f};
        st = Autoplay::vhallNextStep(f, r.g, r.dg, onLip);
        REQUIRE(st.valid);
        CHECK(st.jump);                             // ...the lip commits the jump
        CHECK(st.x == 7); CHECK(st.z == 4);
        CHECK(st.story == 1);
        CHECK(st.target.y == doctest::Approx(3.0f));
    }
    SUBCASE("without links: the east arm cannot reach the door at all") {
        REQUIRE(Autoplay::ensureVHallField(f, r.g, r.dg, door, 2, /*useJumpLinks=*/false));
        const Vec3 onArm{14.5f, 3.0f, 4.5f};
        const Autoplay::VHallStep st = Autoplay::vhallNextStep(f, r.g, r.dg, onArm);
        CHECK_FALSE(st.valid);
        CHECK(st.distHere == 0xFFFF);               // pins WHY the link edge exists
    }
    Autoplay::freeVHallField(f);
}

TEST_CASE("vhall field: the jump is COSTED — a shorter walk route is preferred over it") {
    // Same two arms, but a 3 m BRIDGE row at z=7 connects them the long way round: east arm -> bridge
    // -> west arm, all walkable. From deep on the east arm the walk (via the bridge) and the jump
    // both reach the door; the field must take whichever is genuinely cheaper per node, so the lip
    // itself still jumps (2 steps + jump beats the bridge detour) while the bridge row routes along
    // itself rather than doubling back to the gap.
    CatwalkRig r;
    for (u32 x = 5; x <= 16; x++) addSlab(r.g, x, 7, 12);         // the long-way bridge
    for (u32 z = 4; z <= 7; z++) { addSlab(r.g, 16, z, 12); addSlab(r.g, 5, z, 12); }  // connect arms<->bridge
    const Vec3 door{5.5f, 3.0f, 4.5f};
    Autoplay::VHallField f;
    REQUIRE(Autoplay::ensureVHallField(f, r.g, r.dg, door, 3, /*useJumpLinks=*/true));

    // dist sanity: strictly decreasing along any followed route, door node = 0.
    const Vec3 onLip{10.5f, 3.0f, 4.5f};
    const Autoplay::VHallStep atLip = Autoplay::vhallNextStep(f, r.g, r.dg, onLip);
    REQUIRE(atLip.valid);
    CHECK(atLip.jump);                              // from the lip the jump is 2+3 vs a ~15-step walk
    const Autoplay::VHallStep atDoor = Autoplay::vhallNextStep(f, r.g, r.dg, door);
    CHECK_FALSE(atDoor.valid);                      // at the door: nothing to commit
    CHECK(atDoor.distHere == 0);

    // On the bridge, far from the gap: the route must WALK (never detour back to jump).
    const Vec3 onBridge{9.5f, 3.0f, 7.5f};
    const Autoplay::VHallStep st = Autoplay::vhallNextStep(f, r.g, r.dg, onBridge);
    REQUIRE(st.valid);
    CHECK_FALSE(st.jump);
    Autoplay::freeVHallField(f);
}

TEST_CASE("vhall field: dist decreases along a followed route (walk grid)") {
    LevelGrid g = makeGroundGrid(20, 8);
    for (u32 x = 5; x <= 16; x++) { addSlab(g, x, 4, (u8)(x - 4)); addSlab(g, x, 3, (u8)(x - 4)); }
    for (u32 x = 16; x <= 18; x++) { addSlab(g, x, 4, 12); addSlab(g, x, 3, 12); }
    const DungeonResult none{};
    const Vec3 door{18.5f, 3.0f, 4.5f};
    Autoplay::VHallField f;
    REQUIRE(Autoplay::ensureVHallField(f, g, none, door, 1, true));
    Vec3 p{2.5f, 0.0f, 1.5f};
    u16 prev = 0xFFFF;
    for (u32 i = 0; i < 200; i++) {
        const Autoplay::VHallStep st = Autoplay::vhallNextStep(f, g, none, p);
        if (!st.valid) break;                        // arrived
        REQUIRE(st.distHere != 0xFFFF);
        CHECK(st.distHere < prev);
        prev = st.distHere;
        p = st.target;                               // teleport-follow node to node
    }
    CHECK(prev != 0xFFFF);                           // the loop actually advanced
    Autoplay::freeVHallField(f);
    LevelGridSystem::shutdown(g);
}

// ---------------------------------------------------------------------------------------------
// The follower.

namespace {
// Advance the follower kinematically: walk `dt`-steps along out.dir at `speed`, settling the feet on
// the destination surface each step (the same physics mimicry fieldReachesDoor uses); a wantJump on
// a grounded tick launches a flat ballistic hop that covers `jumpReach` metres along jumpDir. The
// yaw always faces the commanded direction (the eased aim's lag is exercised by the dedicated
// alignment test, not here).
struct SimBot {
    Vec3 pos; bool onGround = true; f32 yaw = 0.0f;
    void face(Vec3 d) { if (lengthSq(d) > 1e-6f) yaw = std::atan2(-d.x, -d.z); }
};
bool followReachesDoor(Autoplay::VHallFollow& fw, const Autoplay::VHallField& f, const LevelGrid& g,
                       const DungeonResult& dg, SimBot& b, u32 maxTicks) {
    const f32 dt = 1.0f / 60.0f, speed = 6.0f, jumpReach = 4.0f;
    for (u32 i = 0; i < maxTicks; i++) {
        if (Autoplay::atVHallGoal(f, g, b.pos)) return true;
        const Autoplay::VHallFollowOut out =
            Autoplay::vhallFollowTick(fw, f, g, dg, b.pos, b.pos.y, b.yaw, b.onGround, dt);
        if (out.wantJump && b.onGround) {
            // A committed takeoff: carry the body across the gap to the landing in one hop (the
            // real arc is the collision system's job; the follower only owns WHERE and WHEN).
            b.pos = b.pos + out.jumpDir * jumpReach;
            u32 gx, gz;
            if (LevelGridSystem::worldToGrid(g, b.pos, gx, gz))
                b.pos.y = LevelGridSystem::effectiveFloorHeight(g, gx, gz, 3.0f);
            continue;
        }
        if (lengthSq(out.dir) < 1e-6f) continue;    // no heading this tick (backoff/at-door)
        b.face(out.dir);
        b.pos = b.pos + out.dir * (speed * dt);
        u32 gx, gz;
        if (LevelGridSystem::worldToGrid(g, b.pos, gx, gz))
            b.pos.y = LevelGridSystem::effectiveFloorHeight(g, gx, gz, b.pos.y);
    }
    return false;
}
} // namespace

TEST_CASE("vhall follower: walks a ground spawn up the ramp to the upper door (parity with the field)") {
    LevelGrid g = makeGroundGrid(20, 8);
    for (u32 x = 5; x <= 16; x++) { addSlab(g, x, 4, (u8)(x - 4)); addSlab(g, x, 3, (u8)(x - 4)); }
    for (u32 x = 16; x <= 18; x++) { addSlab(g, x, 4, 12); addSlab(g, x, 3, 12); }
    const DungeonResult none{};
    const Vec3 door{18.5f, 3.0f, 4.5f};
    Autoplay::VHallField f;
    REQUIRE(Autoplay::ensureVHallField(f, g, none, door, 1, true));
    Autoplay::VHallFollow fw;
    SimBot b; b.pos = {2.5f, 0.0f, 1.5f};
    CHECK(followReachesDoor(fw, f, g, none, b, 4000));
    Autoplay::freeVHallField(f);
    LevelGridSystem::shutdown(g);
}

TEST_CASE("vhall follower: crosses the broken catwalk with a committed jump") {
    CatwalkRig r;
    const Vec3 door{5.5f, 3.0f, 4.5f};
    Autoplay::VHallField f;
    REQUIRE(Autoplay::ensureVHallField(f, r.g, r.dg, door, 1, true));
    Autoplay::VHallFollow fw;
    SimBot b; b.pos = {15.5f, 3.0f, 4.5f};          // deep on the east arm
    CHECK(followReachesDoor(fw, f, r.g, r.dg, b, 4000));
    Autoplay::freeVHallField(f);
}

TEST_CASE("vhall follower: jump press gating truth table") {
    CatwalkRig r;
    const Vec3 door{5.5f, 3.0f, 4.5f};
    Autoplay::VHallField f;
    REQUIRE(Autoplay::ensureVHallField(f, r.g, r.dg, door, 1, true));
    const f32 dt = 1.0f / 60.0f;
    const Vec3 onLip{10.5f, 3.0f, 4.5f};
    const f32 yawWest = std::atan2(1.0f, 0.0f);     // facing -x is yaw with -sin=−1 → atan2(-dx,-dz): dx=-1,dz=0 → atan2(1,0)

    SUBCASE("on the lip, grounded, aligned: pressed") {
        Autoplay::VHallFollow fw;
        const Autoplay::VHallFollowOut out =
            Autoplay::vhallFollowTick(fw, f, r.g, r.dg, onLip, 3.0f, yawWest, true, dt);
        CHECK(out.wantJump);
        CHECK(out.jumpDir.x < -0.9f);               // toward the west lip
    }
    SUBCASE("misaligned yaw: NOT pressed (the eased facing has not arrived)") {
        Autoplay::VHallFollow fw;
        const Autoplay::VHallFollowOut out =
            Autoplay::vhallFollowTick(fw, f, r.g, r.dg, onLip, 3.0f, yawWest + 1.2f, true, dt);
        CHECK_FALSE(out.wantJump);
        CHECK(lengthSq(out.dir) > 1e-6f);           // still steering; the press waits for the turn
    }
    SUBCASE("airborne: NOT pressed, steering held at the landing, no re-pick over the gap") {
        Autoplay::VHallFollow fw;
        // First grounded tick commits the jump...
        (void)Autoplay::vhallFollowTick(fw, f, r.g, r.dg, onLip, 3.0f, yawWest, true, dt);
        REQUIRE(fw.mode == Autoplay::VHF_JUMP_ALIGN);
        // ...then mid-air over the GAP cells (whose story reads GROUND — the trap this pins):
        const Vec3 overGap{8.5f, 3.2f, 4.5f};
        const Autoplay::VHallFollowOut out =
            Autoplay::vhallFollowTick(fw, f, r.g, r.dg, overGap, 3.2f, yawWest, false, dt);
        CHECK_FALSE(out.wantJump);
        CHECK(fw.mode == Autoplay::VHF_JUMP_AIR);
        CHECK(out.dir.x < -0.9f);                   // still at the landing, not a re-read
    }
    SUBCASE("off the lip, grounded: JUMP commitment releases (slid away)") {
        Autoplay::VHallFollow fw;
        (void)Autoplay::vhallFollowTick(fw, f, r.g, r.dg, onLip, 3.0f, yawWest, true, dt);
        REQUIRE(fw.mode == Autoplay::VHF_JUMP_ALIGN);
        const Vec3 slidBack{12.5f, 3.0f, 4.5f};     // two cells back along the arm
        (void)Autoplay::vhallFollowTick(fw, f, r.g, r.dg, slidBack, 3.0f, yawWest, true, dt);
        CHECK(fw.mode != Autoplay::VHF_JUMP_AIR);   // released + re-picked as an ordinary walk
        CHECK_FALSE(fw.mode == Autoplay::VHF_JUMP_ALIGN);
    }
    SUBCASE("blocked jump times out, backs off, and does not instantly re-commit") {
        Autoplay::VHallFollow fw;
        Autoplay::VHallFollowOut out{};
        // Pinned on the lip, aligned, grounded, but the sim never moves it (a body block): after the
        // timeout the commitment must drop and the next picks must refuse a jump for the backoff.
        for (u32 i = 0; i < 200; i++)                // > 2.5 s at 60 Hz
            out = Autoplay::vhallFollowTick(fw, f, r.g, r.dg, onLip, 3.0f, yawWest, true, dt);
        CHECK(fw.mode != Autoplay::VHF_JUMP_ALIGN);  // released...
        CHECK(fw.jumpCd > 0.0f);                     // ...and cooling down
        CHECK_FALSE(out.wantJump);
    }
    Autoplay::freeVHallField(f);
}

TEST_CASE("vhall follower: displacement releases — leash, story change, advanced-past") {
    LevelGrid g = makeGroundGrid(20, 8);
    for (u32 x = 5; x <= 16; x++) { addSlab(g, x, 4, (u8)(x - 4)); addSlab(g, x, 3, (u8)(x - 4)); }
    for (u32 x = 16; x <= 18; x++) { addSlab(g, x, 4, 12); addSlab(g, x, 3, 12); }
    const DungeonResult none{};
    const Vec3 door{18.5f, 3.0f, 4.5f};
    Autoplay::VHallField f;
    REQUIRE(Autoplay::ensureVHallField(f, g, none, door, 1, true));
    const f32 dt = 1.0f / 60.0f;

    SUBCASE("leash: combat drags the bot far sideways -> re-pick from the new node, no walk-back") {
        Autoplay::VHallFollow fw;
        const Vec3 start{10.5f, 0.0f, 1.5f};
        (void)Autoplay::vhallFollowTick(fw, f, g, none, start, 0.0f, 0.0f, true, dt);
        REQUIRE(fw.x >= 0);
        const s16 oldX = fw.x, oldZ = fw.z;
        const Vec3 dragged{14.5f, 0.0f, 6.5f};      // > 2.5 m from the old commit
        (void)Autoplay::vhallFollowTick(fw, f, g, none, dragged, 0.0f, 0.0f, true, dt);
        REQUIRE(fw.x >= 0);
        CHECK((fw.x != oldX || fw.z != oldZ));      // a FRESH commitment near the new position
        const f32 ddx = (fw.x + 0.5f) - dragged.x, ddz = (fw.z + 0.5f) - dragged.z;
        CHECK(ddx * ddx + ddz * ddz < 2.5f * 2.5f); // ...and it is near the bot, not the stale node
    }
    SUBCASE("story change: knocked off the ramp to the ground -> immediate re-pick from the ground") {
        Autoplay::VHallFollow fw;
        const Vec3 onRamp{10.5f, 1.5f, 4.5f};       // mid-ramp (slab 1.5 m) = story 1
        (void)Autoplay::vhallFollowTick(fw, f, g, none, onRamp, 1.5f, 0.0f, true, dt);
        REQUIRE(fw.fromStory == 1);
        const Vec3 onGroundBeside{10.5f, 0.0f, 6.5f};
        (void)Autoplay::vhallFollowTick(fw, f, g, none, onGroundBeside, 0.0f, 0.0f, true, dt);
        CHECK(fw.fromStory == 0);                   // the commitment was remade from the ground node
    }
    Autoplay::freeVHallField(f);
    LevelGridSystem::shutdown(g);
}
