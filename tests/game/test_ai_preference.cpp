// Authored combat openers (EnemyDef.aiPreference → Entity.aiPreference).
//
// The field spent months parsed and thrown away — every enemy opened with a hardcoded CHASE,
// flattening authored rosters (worst: Spider Caverns, 7 of 8 defs non-chase) into charge-only.
// These tests pin the pure opener map (preferredCombatState) and lint enemies.json so an
// authored preference can never be nonsense for the enemy's stats: STRAFE is a firing state
// (ranged only), SURROUND walks an encircle slot (grounded melee only).

#include <doctest/doctest.h>
#include "game/entity.h"

#include <json/nlohmann/json.hpp>
#include <fstream>
#include <set>
#include <string>

namespace {
Entity makeEnemy(AIState pref, f32 attackRange, bool flying = false) {
    Entity e{};
    e.aiPreference = static_cast<u8>(pref);
    e.attackRange  = attackRange;
    if (flying) e.flags |= ENT_FLYING;
    return e;
}
} // namespace

TEST_CASE("preferredCombatState: safe pure opener map") {
    // Ranged strafer opens firing-and-sidestepping; a (mis-authored) melee strafer degrades
    // to CHASE because STRAFE fires projectiles.
    CHECK(preferredCombatState(makeEnemy(AIState::STRAFE, 10.0f)) == AIState::STRAFE);
    CHECK(preferredCombatState(makeEnemy(AIState::STRAFE, 2.5f))  == AIState::CHASE);

    // Grounded melee surrounder takes an encircle slot; ranged or flying degrade to CHASE.
    CHECK(preferredCombatState(makeEnemy(AIState::SURROUND, 2.5f))        == AIState::SURROUND);
    CHECK(preferredCombatState(makeEnemy(AIState::SURROUND, 11.0f))       == AIState::CHASE);
    CHECK(preferredCombatState(makeEnemy(AIState::SURROUND, 2.5f, true))  == AIState::CHASE);

    // Everything else — chase, retreat (roles handle keep-away), dormant, unauthored — CHASE.
    CHECK(preferredCombatState(makeEnemy(AIState::CHASE, 2.5f))   == AIState::CHASE);
    CHECK(preferredCombatState(makeEnemy(AIState::RETREAT, 10.0f)) == AIState::CHASE);
    CHECK(preferredCombatState(makeEnemy(AIState::DORMANT, 2.0f)) == AIState::CHASE);
    CHECK(preferredCombatState(makeEnemy(AIState::IDLE, 2.0f))    == AIState::CHASE);
}

TEST_CASE("enemies.json: every aiPreference is known and fits the enemy's stats") {
    std::ifstream f(DUNGEON_REPO_ROOT "/assets/config/enemies.json");
    REQUIRE(f.good());
    nlohmann::json doc = nlohmann::json::parse(f);

    const std::set<std::string> kKnown = {"idle", "chase", "strafe", "flyby", "dormant",
                                          "flank", "retreat", "surround"};
    for (const auto& e : doc["enemies"]) {
        const std::string name = e.value("name", "?");
        const std::string pref = e.value("aiPreference", "chase");
        const f32  attackRange = e.value("attackRange", 0.0f);
        const bool flying      = e.value("flying", false);
        CAPTURE(name); CAPTURE(pref);

        // A typo'd preference parses to IDLE in the loader — the enemy would open CHASE
        // anyway, but the authored intent would be silently lost. Fail loudly here instead.
        CHECK(kKnown.count(pref) == 1);

        // Stat-fit: the opener gates in preferredCombatState would silently degrade these
        // to CHASE at runtime — authoring them is always a mistake, so catch it in CI.
        if (pref == "strafe")   CHECK(attackRange > 5.0f);
        if (pref == "surround") { CHECK(attackRange <= 5.0f); CHECK_FALSE(flying); }
    }
}
