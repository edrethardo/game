// autoplay_vhall.h — the VERTICAL_HALL two-story travel field + node-committed follower for the
// Autoplay bot.
//
// VHALL is a TWO-STORY floor: the bot spawns on one story and the exit door is on the other, reached
// by a graduated-slab ramp. Every earlier attempt steered with the flat 2D flow field
// (LevelGridSystem::flowDirection) plus a pile of per-tick ramp heuristics (commit a ramp, latch
// "crossed", beeline the door, pulse a jump). They all fought the same root problem: the flat field
// treats each XZ cell as ONE node, so it CANNOT represent two stories — a balcony cell and the ground
// beneath it are the same node — and it routes a bot that has just climbed to the balcony straight
// back off the open inner edge to the ground ("drops afterward"). No heuristic on top of a 2D field
// fixes a 3D topology.
//
// The field is a flow field over the real topology, mirroring what DescentField did for FOUR_STORY:
// nodes are (cell, STORY) pairs — ground (story 0) and upper (story 1) — and two nodes are adjacent
// only when you could actually step between their walkable surfaces:
//   * A step from (cell,s) to a 4-connected (neighbour, ns) — ANY story ns — is legal when the
//     neighbour node is walkable AND its surface height is within one step (PLATFORM_STEP_TOLERANCE)
//     of ours, which the collision step-up/step-down handles. Cross-story steps are what model
//     MOUNTING the ramp. A balcony (3 m) is NOT within a step of the void/ground (0 m) beside it, so
//     the field can never route the bot off a balcony edge.
//   * A GROUND node is excluded where a slab roofs it below head height (the low part of a ramp) —
//     the body cannot fit and routing it there wedges it.
//   * The floor's recorded JUMP LINKS (DungeonResult::jumpLinks — the broken catwalk's 2-cell gap)
//     are extra EDGES between two upper lip nodes, cost ~3 walk-steps. This is load-bearing, not
//     flavour: the gap ISOLATES the west balcony at 3 m, so without the link any route to a
//     W-balcony exit from the wrong ramp must descend and re-climb — under combat displacement that
//     is the measured "climbs a ramp and comes straight back down" loop. With the link, "crest any
//     ramp -> walk the upper loop -> door" is the shortest route from every upper node, and the
//     down-the-ramp heading stops existing. The route across a link is a real JUMP the follower must
//     perform (~2 m of void against a ~4 m running-jump reach — the level's own risky shortcut).
//
// EXECUTION is a NODE-COMMITTED FOLLOWER (VHallFollow), not a per-tick direction re-read. Direction
// following is memoryless, and on a stacked floor that is fatal: a local error (combat shove, a
// missed step) changes the bot's STORY, which teleports it to a different part of the route graph,
// and a per-tick re-read silently redirects — the bot circles forever and no stall detector sees it
// (measured: 6000+ s on one floor, 18 m of travel per 15 s window, never arriving). The follower
// latches ONE target node and steers at its CENTRE — a point servo, so lateral drift self-corrects,
// which is what makes the old anti-drift ramp assist unnecessary — and releases the latch only on
// arrival, on a story change (fell / was knocked off: re-read from the new node — self-healing), on
// a leash breach (combat dragged it away), or when the field says it has already advanced PAST the
// commit (per-node distance monotonicity — never walk backwards to honour a stale latch).
#pragma once
#include "core/types.h"
#include "core/math.h"
#include "world/level_grid.h"
#include "world/level_gen.h"   // DungeonResult::jumpLinks — the recorded jumpable gaps

namespace Autoplay {

// Per-(cell,story) flow field. Owned by the engine driver (one per lane), rebuilt on demand.
// `dir` holds 2*width*depth entries: index = story*(width*depth) + z*width + x. Encoding matches the
// exit field, plus the jump code: 0-7 = step direction toward the door, 0xFD = the route from this
// node is the JUMP LINK whose lip is this cell, 0xFE = at the door, 0xFF = unreachable.
// `dist` is the remaining route cost to the door per node (walk step = 1, jump = 3; 0xFFFF =
// unreachable) — it feeds the follower's monotonicity release and the [STALL] progress telemetry.
struct VHallField {
    u8*  dir   = nullptr;    // 2*width*depth, or null before the first build
    u16* dist  = nullptr;    // same indexing; 0xFFFF = unreachable
    u32 cap    = 0;          // allocated entry count (grown, never shrunk)
    u32 width  = 0, depth = 0;
    u32 stamp  = 0xFFFFFFFFu; // floor identity — must be the floor's SEED fold, not its NUMBER: floor
                              // numbers repeat across runs/difficulty tiers while the forced 52-grid and
                              // a coin-flip doorY also match, which is the whole early-out comparison
    f32 doorY  = 0.0f;       // the door story this field was seeded for (rebuild if it changes)
    bool linksUsed = false;  // whether jump links were edges in this build (part of staleness)
    bool valid = false;
};

// Rebuild the field if it is stale for (doorPos, stamp, grid size, useJumpLinks); a no-op when
// already current, so it is safe to call every tick. `dg` supplies the recorded jump links;
// `useJumpLinks=false` builds the walk-only field (the pre-follower behaviour, kept for the A/B's
// control arm). Returns `valid`.
bool ensureVHallField(VHallField& f, const LevelGrid& g, const DungeonResult& dg,
                      Vec3 doorPos, u32 stamp, bool useJumpLinks);

// Unit XZ heading toward the door from the bot's current (cell, story), or {0,0,0} at the door / off
// the field / on an unreachable node / on a jump-link lip (the legacy readout cannot express a jump —
// the follower can; this stays only for the A/B control arm and dies with it).
Vec3 vhallDirection(const VHallField& f, const LevelGrid& g, Vec3 pos);

// True when the bot's (cell, story) node is the door itself (field code 0xFE) — it has arrived.
bool atVHallGoal(const VHallField& f, const LevelGrid& g, Vec3 pos);

void freeVHallField(VHallField& f);

// ---------------------------------------------------------------------------------------------
// Node-committed follower.

// The next route NODE from a position: the committed-able step, not just a bearing.
struct VHallStep {
    bool valid    = false;   // there is a next node to commit to
    u32  x = 0, z = 0;       // its cell
    u8   story    = 0;       // its story
    bool jump     = false;   // reaching it is a JUMP across a recorded link (target = landing lip)
    Vec3 target   = {};      // its centre at surface height (the point the servo steers at)
    u16  distHere = 0xFFFF;  // route cost remaining at the BOT's node (telemetry + monotonicity)
};
VHallStep vhallNextStep(const VHallField& f, const LevelGrid& g, const DungeonResult& dg, Vec3 pos);

// Per-lane follower state. POD; reset ({}) on floor change.
struct VHallFollow {
    s16 x = -1, z = -1;      // committed target cell (-1 = no commitment)
    s8  story = -1;          // its story
    s8  fromStory = -1;      // the bot's story when it committed (a change = fell/knocked off)
    u8  mode  = 0;           // VHF_* below
    f32 modeT = 0.0f;        // seconds in the current mode (JUMP_ALIGN timeout)
    s8  link  = -1;          // committed link index while jumping (-1 = none)
    f32 jumpCd = 0.0f;       // s left of the blocked-jump backoff (no jump re-commit while > 0)
};
enum : u8 { VHF_NONE = 0, VHF_WALK = 1, VHF_JUMP_ALIGN = 2, VHF_JUMP_AIR = 3 };

// One tick of the follower's output. `dir` is the point-servo bearing toward the committed node's
// centre ({0,0,0} = arrived / nothing to do). While executing a jump, `jumpDir` is the committed
// link's axis (unit XZ) and `wantJump` asks the driver to press JUMP — the driver still gates the
// press on grounded + feet alignment, and scopes its fall-veto exemption to this axis.
struct VHallFollowOut {
    Vec3 dir      = {};
    Vec3 jumpDir  = {};
    bool wantJump = false;
    u16  distHere = 0xFFFF;  // remaining route cost at the bot's node (the [STALL] progress metric)
};

// Advance the follower one tick. Pure: reads the field/grid/links and the bot's pose, mutates only
// `fw`. `yaw` is the REAL current facing (the eased aim lags the desired aim — a jump pressed before
// the facing arrives launches diagonally into the void, so takeoff waits for |yaw error| to close).
VHallFollowOut vhallFollowTick(VHallFollow& fw, const VHallField& f, const LevelGrid& g,
                               const DungeonResult& dg, Vec3 pos, f32 feetY, f32 yaw,
                               bool onGround, f32 dt);

} // namespace Autoplay
