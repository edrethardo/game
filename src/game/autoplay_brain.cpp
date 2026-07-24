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

// (The engagement ceiling — THREAT_RADIUS / engageCeiling — now lives in autoplay_combat.h so this
// gate and pickTarget's sticky-target release read the SAME number; see the comment there.)

// TRAVEL movement. The aim is pointed at the heading, but the WALK is resolved against the yaw the
// bot has RIGHT NOW rather than the one it is turning toward — which is why this is a full WASD
// decomposition and not just "hold W".
//
// applyBotIntent deliberately EASES the aim (gain 6/s under a 2.8 rad/s cap, for the camera's sake),
// so the facing lags the desired heading by a few tenths of a second after any turn. Movement rides
// the facing: hold W through that lag and the bot walks wherever it happened to be pointing, which
// on an open floor is a harmless wide arc but in a corridor is a wall. On the Descent's braided maze
// — 3-wide corridors, 90-degree turns every few metres — that is most corners, and move-and-slide
// then scrapes the bot along the wall for the rest of the turn. Observed directly ("the time goes
// right now looking and hugging the walls and corners") and visible in the trace as a bot walking
// forward 92% of the time while getting FARTHER from its goal three times as often as closer.
//
// Projecting the heading onto the current forward/right basis fixes it without touching the aim: the
// bot steps sideways out of the corner immediately and straightens up as the eased turn catches up,
// which is also what a player does. The basis matches player.cpp's movement exactly (forward
// {-sin,0,-cos}, right = cross(forward, up)) — it has to, or the decomposition steers into the wall
// it is trying to avoid.
static void faceAndGo(const BotView& v, BotIntent& out) {
    if (lengthSq(v.flowDir) < 0.0001f) return;    // at exit or unreachable: no heading
    const Vec3 dir = normalize(Vec3{v.flowDir.x, 0.0f, v.flowDir.z});
    f32 yaw, pitch; dirToAim(dir, yaw, pitch);
    out.aimYaw = yaw; out.aimPitch = 0.0f;

    const f32  cosY = cosf(v.yaw), sinY = sinf(v.yaw);
    const Vec3 fwd  { -sinY, 0.0f, -cosY };
    const Vec3 right{  cosY, 0.0f, -sinY };
    const f32  f = dot(dir, fwd), r = dot(dir, right);
    // ~20°: engage an axis as soon as the heading leans that way. Both components are always
    // available because dir is unit and the basis orthonormal (f² + r² == 1), so the larger of the
    // two is at least 0.707 and the bot can never end up holding nothing.
    constexpr f32 kAxis = 0.35f;
    out.moveFwd   = f >  kAxis;
    out.moveBack  = f < -kAxis;
    out.moveRight = r >  kAxis;
    out.moveLeft  = r < -kAxis;
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
    const s32 ti = pickTarget(v, d);
    if (ti >= 0 && v.targets[(u32)ti].dist <= engageCeiling(v, d)) return decideCombat(v, d);

    // DESCEND: at an eligible door, ask to descend.
    DescendCtx dc; dc.doorActive = v.doorActive; dc.distToDoor = v.distToDoor;
    dc.hasBoss = v.hasBoss; dc.bossAlive = v.bossAlive;
    if (mayDescend(dc)) { out.descend = true; return out; }

    // TRAVEL: walk the flow field toward the exit.
    faceAndGo(v, out);
    return out;
}

} // namespace Autoplay
