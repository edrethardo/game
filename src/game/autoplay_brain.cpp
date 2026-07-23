// autoplay_brain.cpp — see autoplay_brain.h. Priority state machine; each branch returns a full
// BotIntent. TRAVEL faces the flow direction and walks; the per-style vertical goal (ramp/hole/
// pad) is folded into flowDir by the engine driver before the view is built, so the brain stays
// 2D here and the driver owns story routing (it has StoryNav + DungeonResult).
#include "game/autoplay_brain.h"
#include "game/autoplay_doctrine.h"
#include "game/autoplay_combat.h"
#include "game/autoplay_nav.h"
#include "core/math.h"

namespace Autoplay {

static void faceAndGo(const BotView& v, BotIntent& out) {
    if (lengthSq(v.flowDir) < 0.0001f) return;    // at exit or unreachable: no heading
    f32 yaw, pitch; dirToAim(Vec3{v.flowDir.x, 0.0f, v.flowDir.z}, yaw, pitch);
    out.aimYaw = yaw; out.aimPitch = 0.0f;
    out.moveFwd = true;                            // driver vetoes the actual step if unsafe
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

    // FIGHT: any LOS target in reach takes priority over travelling.
    if (pickTarget(v) >= 0) return decideCombat(v, d);

    // DESCEND: at an eligible door, ask to descend.
    DescendCtx dc; dc.doorActive = v.doorActive; dc.distToDoor = v.distToDoor;
    dc.hasBoss = v.hasBoss; dc.bossAlive = v.bossAlive;
    if (mayDescend(dc)) { out.descend = true; return out; }

    // TRAVEL: walk the flow field toward the exit.
    faceAndGo(v, out);
    return out;
}

} // namespace Autoplay
