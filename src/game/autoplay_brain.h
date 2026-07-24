// autoplay_brain.h — the Autoplay decision core. One pure call per tick: survive > fight >
// descend > travel, plus the non-normal-world idle. (loot-settle + low-hp globe detours are NOT
// here — they live in the engine driver, which steers toward pickups before the view is built.)
// Composes autoplay_combat (fight) and the flow-field heading (travel) under the doctrine chosen
// by the build cell.
// Engine-free so it unit-tests on hand-built BotViews. The engine driver owns the per-style
// vertical routing (folded into flowDir before the view is built), so the brain stays 2D here.
// SCOPE OF THE HAZARD VETO: the driver applies `Autoplay::stepAllowed` to `BotView.flowDir` inside
// buildBotView — i.e. to the TRAVEL heading only, BEFORE this call. Nothing vetoes the movement
// the FIGHT branch emits (kite/close/strafe): that heading is derived from a live enemy the bot can
// see, so it is short, reactive, and would fight a veto more than it would benefit; a fight that
// backs into lava is caught by the SURVIVE potion rule and the driver's escape backstop instead.
#pragma once
#include "game/autoplay_intent.h"

namespace Autoplay {
BotIntent decide(const BotView& v);
}
