// tests/game/test_bot_input.cpp — the pure synthetic-input overlay the Autoplay bot drives.
// Verifies held-vs-pressed edge semantics mirror the engine's once-per-render-frame model.
#include "doctest/doctest.h"
#include "game/bot_input.h"
#include "platform/input.h"   // GameAction

TEST_CASE("BotInput: held reports down every frame; pressed fires once per edge") {
    BotInput b;
    b.setHeld(GameAction::FIRE, true);
    CHECK(b.down(GameAction::FIRE));
    CHECK(b.pressed(GameAction::FIRE));      // first frame the bit is up: an edge
    b.rollEdges();                            // end-of-render-frame roll (mirrors consumePressedState)
    CHECK(b.down(GameAction::FIRE));          // still held
    CHECK_FALSE(b.pressed(GameAction::FIRE)); // no longer an edge
}

TEST_CASE("BotInput: releasing clears down and pressed") {
    BotInput b;
    b.setHeld(GameAction::JUMP, true);
    b.rollEdges();
    b.setHeld(GameAction::JUMP, false);
    CHECK_FALSE(b.down(GameAction::JUMP));
    CHECK_FALSE(b.pressed(GameAction::JUMP));
}

TEST_CASE("BotInput: a fresh press after release is a new edge") {
    BotInput b;
    b.setHeld(GameAction::DODGE, true); b.rollEdges();
    b.setHeld(GameAction::DODGE, false); b.rollEdges();
    b.setHeld(GameAction::DODGE, true);
    CHECK(b.pressed(GameAction::DODGE));      // up->down again = edge
}

TEST_CASE("BotInput: clear() drops all held bits (used when the bot yields control)") {
    BotInput b;
    b.setHeld(GameAction::MOVE_FORWARD, true);
    b.setHeld(GameAction::FIRE, true);
    b.clear();
    CHECK_FALSE(b.down(GameAction::MOVE_FORWARD));
    CHECK_FALSE(b.down(GameAction::FIRE));
}

TEST_CASE("BotInput: an out-of-range action never reports down (bounds safety)") {
    BotInput b;
    CHECK_FALSE(b.down(GameAction::COUNT));
    CHECK_FALSE(b.pressed(GameAction::COUNT));
}
