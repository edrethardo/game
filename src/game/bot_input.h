// bot_input.h — the Autoplay bot's synthetic-action overlay.
//
// A per-GameAction held bitset with edge tracking, consulted by Input::isActionDown /
// isActionPressed so the bot drives EVERY existing input consumer (movement, fire, dodge,
// skills, potion, interact) through the exact code paths a human's keypresses take. Pure and
// header-only so it unit-tests without SDL. `pressed` mirrors the engine's once-per-render-frame
// edge model: rollEdges() is called from Input::consumePressedState(), exactly like the real
// device previous<-current roll, so a held synthetic action fires isActionPressed on one frame only.
#pragma once
#include "core/types.h"
#include "platform/input.h"   // GameAction

struct BotInput {
    static constexpr u32 N = static_cast<u32>(GameAction::COUNT);
    bool held[N] = {};
    bool prev[N] = {};

    void setHeld(GameAction a, bool on) { const u32 i = idx(a); if (i < N) held[i] = on; }
    bool down(GameAction a)   const { const u32 i = idx(a); return i < N && held[i]; }
    bool pressed(GameAction a) const { const u32 i = idx(a); return i < N && held[i] && !prev[i]; }
    void rollEdges() { for (u32 i = 0; i < N; i++) prev[i] = held[i]; }   // end-of-render-frame
    void clear() { for (u32 i = 0; i < N; i++) held[i] = false; }         // bot yields control

private:
    static u32 idx(GameAction a) { return static_cast<u32>(a); }
};
