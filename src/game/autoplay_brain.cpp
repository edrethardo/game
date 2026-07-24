// autoplay_brain.cpp — see autoplay_brain.h. Priority state machine; each branch returns a full
// BotIntent. TRAVEL faces the flow direction and walks; the per-style vertical goal (ramp/hole/
// pad) is folded into flowDir by the engine driver before the view is built, so the brain stays
// 2D here and the driver owns story routing (it has StoryNav + DungeonResult). The driver's hazard
// veto is applied to that flowDir (in buildBotView, before this runs) and covers ONLY it — the
// FIGHT branch's own kite/close headings are NOT vetoed. See autoplay_brain.h for why.
#include "game/autoplay_brain.h"
#include "game/autoplay_doctrine.h"
#include "game/autoplay_combat.h"
#include "game/autoplay_nav.h"
#include "core/math.h"
#include <math.h>   // fmaxf

namespace Autoplay {

// Engagement ceiling floor: an enemy within this radius is a real, immediate threat worth handling
// even for a short-reach build; BEYOND it the bot keeps pushing to the exit instead of chasing.
// Walking the flow field naturally brings in-band enemies into range, so the bot never has to
// backtrack for a distant straggler. Without this cap the FIGHT branch fired for ANY line-of-sight
// target regardless of distance, and on the dense 50+-enemy VERTICAL_HALL floors that let far
// targets (measured 16-21 m off the exit route) preempt travel indefinitely — the bot oscillated
// toward the swarm and never crossed. 12 m ~= the width of a couple of rooms.
static constexpr f32 THREAT_RADIUS = 12.0f;

static void faceAndGo(const BotView& v, BotIntent& out) {
    if (lengthSq(v.flowDir) < 0.0001f) return;    // at exit or unreachable: no heading
    f32 yaw, pitch; dirToAim(Vec3{v.flowDir.x, 0.0f, v.flowDir.z}, yaw, pitch);
    out.aimYaw = yaw; out.aimPitch = 0.0f;
    out.moveFwd = true;                            // flowDir was already hazard-vetoed by the driver
}

BotIntent decide(const BotView& v) {
    BotIntent out{};
    out.aimYaw = v.yaw; out.aimPitch = v.pitch;

    // Non-normal worlds (town, arena, source chamber): the bot does nothing.
    if (!v.onNormalFloor) return out;

    // Stun: CC correctness — emit nothing actionable (the input layer would suppress it anyway,
    // but keeping the brain honest means the HUD/telemetry never shows a "stunned but acting" bot).
    if (v.stunned) return out;

    const Doctrine d = doctrineFor(v.buildCell);

    // SURVIVE: drink at the doctrine threshold. (Globe detours are a driver concern — it steers
    // toward v.globes when low; here we just handle the potion press.)
    if (v.hp <= v.maxHp * d.potionHpFrac && v.potionReady) { out.potion = true; return out; }

    // FIGHT: engage the nearest LOS target, but ONLY within the engagement ceiling — the wider of
    // the doctrine's own fire band (engageMax x weaponRange, so a long-range build still commits at
    // its true reach) and THREAT_RADIUS (so even a short-reach build handles anything genuinely
    // close). A target beyond the ceiling falls through to DESCEND/TRAVEL: the bot keeps heading for
    // the exit rather than being dragged across the floor toward a distant straggler.
    const s32 ti = pickTarget(v);
    if (ti >= 0) {
        const f32 ceil = fmaxf(d.engageMax * v.weaponRange, THREAT_RADIUS);
        if (v.targets[(u32)ti].dist <= ceil) return decideCombat(v, d);
    }

    // DESCEND: at an eligible door, ask to descend.
    DescendCtx dc; dc.doorActive = v.doorActive; dc.distToDoor = v.distToDoor;
    dc.hasBoss = v.hasBoss; dc.bossAlive = v.bossAlive;
    if (mayDescend(dc)) { out.descend = true; return out; }

    // TRAVEL: walk the flow field toward the exit.
    faceAndGo(v, out);
    return out;
}

} // namespace Autoplay
