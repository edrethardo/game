// autoplay_brain.h — the Autoplay decision core. One pure call per tick: survive > fight >
// descend > travel, plus the non-normal-world idle. (loot-settle + low-hp globe detours are NOT
// here — they live in the engine driver, which steers toward pickups before the view is built.)
// Composes autoplay_combat (fight) and the flow-field heading (travel) under the doctrine chosen
// by the build cell.
// Engine-free so it unit-tests on hand-built BotViews. The engine driver applies the hazard
// veto to the movement this emits and owns the per-style vertical routing (folded into flowDir
// before the view is built), so the brain stays 2D here.
#pragma once
#include "game/autoplay_intent.h"

namespace Autoplay {
BotIntent decide(const BotView& v);
}
