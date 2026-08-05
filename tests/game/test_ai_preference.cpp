// Authored combat openers (EnemyDef.aiPreference → Entity.aiPreference).
//
// The field spent months parsed and thrown away — every enemy opened with a hardcoded CHASE,
// flattening authored rosters (worst: Spider Caverns, 7 of 8 defs non-chase) into charge-only.
// These tests pin the pure opener map (preferredCombatState) and lint enemies.json so an
// authored preference can never be nonsense for the enemy's stats: STRAFE is a firing state
// (ranged only), SURROUND walks an encircle slot (grounded melee only).

#include <doctest/doctest.h>
#include "game/entity.h"
#include "game/enemy_def.h"
#include "game/zone_def.h"
#include "game/quest_def.h"

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


// --- Named bosses must not be in the random roster -------------------------------------------
//
// An act boss is an ordinary enemies.json entry (it needs a mesh, a skin, roles) placed BY NAME at
// one fixed spot. Nothing about that stops its tier's spawn pool from ALSO rolling it, and nothing
// did: a single TristRAM once held two Griswalds, and Act 2's final boss turned up in an Act 1
// field as trash. `unique` is the opt-out and collectTierDefs is the one choke that honours it, so
// both halves are pinned here — the mechanism, and the data that has to use it.

TEST_CASE("collectTierDefs never hands back a unique def") {
    EnemyDefTable t{};
    t.count = 3;
    t.defs[0].tier = 5; t.defs[0].unique = false;
    t.defs[1].tier = 5; t.defs[1].unique = true;    // the boss
    t.defs[2].tier = 5; t.defs[2].unique = false;

    const EnemyDef* out[8];
    const u32 n = collectTierDefs(t, 5, out, 8, /*act=*/0);
    CHECK(n == 2);
    for (u32 i = 0; i < n; i++) CHECK_FALSE(out[i]->unique);

    // A tier made ENTIRELY of bosses collects nothing rather than leaking one — the caller's
    // empty-roster fallback is the right answer there, a boss as trash never is.
    t.defs[0].unique = true; t.defs[2].unique = true;
    CHECK(collectTierDefs(t, 5, out, 8, /*act=*/0) == 0);
}

TEST_CASE("collectTierDefs keeps each act's bestiary to itself") {
    EnemyDefTable t{};
    t.count = 3;
    t.defs[0].tier = 5; t.defs[0].act = 0;   // dungeon
    t.defs[1].tier = 5; t.defs[1].act = 1;   // Act 1 overworld
    t.defs[2].tier = 5; t.defs[2].act = 2;   // Act 2 Underground

    const EnemyDef* out[8];
    for (u8 a = 0; a < 3; a++) {
        CAPTURE(a);
        const u32 n = collectTierDefs(t, 5, out, 8, a);
        REQUIRE(n == 1);
        CHECK(out[0]->act == a);   // never a neighbour act's monster, in either direction
    }
}

TEST_CASE("every zone boss is a unique enemy, and every unique enemy is a zone boss") {
    std::ifstream f(DUNGEON_REPO_ROOT "/assets/config/enemies.json");
    REQUIRE(f.good());
    nlohmann::json doc = nlohmann::json::parse(f);

    std::set<std::string> uniques, all;
    for (const auto& e : doc["enemies"]) {
        const std::string n = e.value("name", "?");
        all.insert(n);
        if (e.value("unique", false)) uniques.insert(n);
    }

    std::set<std::string> zoneBosses;
    for (u32 i = 0; i < Zone::COUNT; i++) {
        const char* b = Zone::ZONES[i].boss;
        if (!b || !b[0]) continue;
        zoneBosses.insert(b);
        CAPTURE(Zone::ZONES[i].name); CAPTURE(b);
        // Named but absent = a boss that never spawns, i.e. a SLAY quest nobody can finish.
        CHECK(all.count(b) == 1);
        // Named but not unique = the boss ALSO rolls as trash in its own zone.
        CHECK(uniques.count(b) == 1);
    }

    // The converse: a unique def nothing places is dead content — it can never appear at all.
    for (const std::string& u : uniques) {
        CAPTURE(u);
        CHECK(zoneBosses.count(u) == 1);
    }

    // And every SLAY objective must name one of them, or the quest has no guaranteed target.
    for (u32 i = 0; i < Quest::COUNT; i++) {
        const Quest::QuestDef& q = Quest::QUESTS[i];
        if (q.trigger != Quest::Trigger::SLAY) continue;
        CAPTURE(q.name); CAPTURE(q.target);
        CHECK(zoneBosses.count(q.target) == 1);
    }
}


// --- The overworld bestiary: act scoping and the two new roles --------------------------------
//
// These lint enemies.json rather than exercise behaviour, because every failure mode here is
// SILENT at runtime: a typo'd role parses to NORMAL, an act with an empty rollable pool spawns an
// empty zone, and a splitter whose half does not exist simply dies without splitting. None of
// those crash, log, or look wrong in review — they just quietly make content not happen.

TEST_CASE("enemies.json: every role string is one the loader knows") {
    std::ifstream f(DUNGEON_REPO_ROOT "/assets/config/enemies.json");
    REQUIRE(f.good());
    nlohmann::json doc = nlohmann::json::parse(f);

    const std::set<std::string> kKnownRoles = {
        "normal", "ambush", "summoner", "healer", "aura", "ranged_caster",
        "charger", "bomber", "shield_bearer", "rout", "splitter"};
    for (const auto& e : doc["enemies"]) {
        const std::string name = e.value("name", "?");
        CAPTURE(name);
        if (!e.contains("role")) continue;
        if (e["role"].is_array()) {
            for (const auto& r : e["role"]) {
                const std::string s2 = r.get<std::string>();
                CAPTURE(s2);
                CHECK(kKnownRoles.count(s2) == 1);
            }
        } else {
            const std::string s2 = e["role"].get<std::string>();
            CAPTURE(s2);
            CHECK(kKnownRoles.count(s2) == 1);
        }
    }
}

TEST_CASE("enemies.json: a SPLITTER names a half that exists and does not itself split") {
    std::ifstream f(DUNGEON_REPO_ROOT "/assets/config/enemies.json");
    REQUIRE(f.good());
    nlohmann::json doc = nlohmann::json::parse(f);

    auto hasRole = [](const nlohmann::json& e, const char* want) {
        if (!e.contains("role")) return false;
        if (e["role"].is_array()) {
            for (const auto& r : e["role"]) if (r.get<std::string>() == want) return true;
            return false;
        }
        return e["role"].get<std::string>() == want;
    };

    std::map<std::string, const nlohmann::json*> byName;
    for (const auto& e : doc["enemies"]) byName[e.value("name", "?")] = &e;

    for (const auto& e : doc["enemies"]) {
        if (!hasRole(e, "splitter")) continue;
        const std::string name = e.value("name", "?");
        const std::string half = e.value("spawnEnemy", std::string());
        CAPTURE(name); CAPTURE(half);
        // No half named = the role is inert: it dies like anything else and the gimmick is gone.
        REQUIRE_FALSE(half.empty());
        REQUIRE(byName.count(half) == 1);
        // The recursion terminates in ONE step by construction, not by a depth counter that a
        // future JSON edit could quietly mis-set — so the half must not be a splitter itself.
        CHECK_FALSE(hasRole(*byName[half], "splitter"));
        // …and the half must be in the same act, or killing it spawns monsters the zone's own
        // roster would never produce.
        CHECK(byName[half]->value("act", 0) == e.value("act", 0));
    }
}

TEST_CASE("enemies.json: every act has a rollable roster of its own") {
    std::ifstream f(DUNGEON_REPO_ROOT "/assets/config/enemies.json");
    REQUIRE(f.good());
    nlohmann::json doc = nlohmann::json::parse(f);

    // Count what collectTierDefs would actually hand a zone: right act, right tier, not unique.
    // An act whose pool is empty spawns a zone with nothing in it — which reads as a broken level,
    // not as a design choice, and nothing else would catch it.
    for (u8 act = 1; act <= 2; act++) {
        u32 rollable = 0;
        for (const auto& e : doc["enemies"]) {
            if (e.value("act", 0) != act) continue;
            if (e.value("unique", false)) continue;
            if (e.value("tier", 0) != 5) continue;   // zones spawn tier 5 (post-Inferno content)
            rollable++;
        }
        CAPTURE(act);
        CHECK(rollable >= 3);   // fewer than three and every zone in the act looks identical
    }

    // The dungeon must keep a deep-tier roster of its OWN after the acts were carved out of it.
    u32 dungeonTier5 = 0;
    for (const auto& e : doc["enemies"])
        if (e.value("act", 0) == 0 && !e.value("unique", false) && e.value("tier", 0) == 5)
            dungeonTier5++;
    CHECK(dungeonTier5 >= 5);
}

// --- The overworld must not out-scale the dungeon it sits above -----------------------------------
//
// Zones evaluate the difficulty curve at the LADDER END (Inferno floor 50, effective floor 200) —
// see Engine::scalingEffectiveFloor. Two things went wrong there and both were invisible until
// someone played it. The engine fed the SENTINEL floor (52-96) into the curve, so a compounding HP
// term put TristRAM at 1.31x and Hellgate: Localhost at 1.84x the health of anything in Inferno; and
// the act rosters were authored 3.6-4.6x fatter than the dungeon's own tier-5 enemies on top of that.
//
// The engine half is now single-sourced. This pins the DATA half: since every zone scales at exactly
// the same effective floor as Inferno 50, an act enemy's authored stats are directly comparable to a
// dungeon tier-5 enemy's, and the acts inherit the balance lab's measured Inferno band for free.
// Author an act mob at twice the norm and it is twice as tough as the hardest thing in the game.

TEST_CASE("act rosters sit alongside the dungeon's deepest tier, not above it") {
    std::ifstream f(DUNGEON_REPO_ROOT "/assets/config/enemies.json");
    REQUIRE(f.good());
    nlohmann::json doc = nlohmann::json::parse(f);

    auto meanOf = [&](u8 act, const char* field) {
        f32 sum = 0.0f; u32 n = 0;
        for (const auto& e : doc["enemies"]) {
            if (e.value("act", 0) != act) continue;
            if (e.value("unique", false)) continue;      // bosses are a different scale by design
            if (e.value("tier", 0) != 5) continue;
            sum += e.value(field, 0.0f); n++;
        }
        REQUIRE(n > 0);
        return sum / static_cast<f32>(n);
    };

    const f32 dungeonHp  = meanOf(0, "health");
    const f32 dungeonDmg = meanOf(0, "damage");

    for (u8 act = 1; act <= 2; act++) {
        CAPTURE(act);
        // A generous band: Act 2 is allowed a step up on Act 1, and neither may run away from the
        // dungeon roster the way they did (4.6x) before this was measured.
        CHECK(meanOf(act, "health") <= dungeonHp  * 1.6f);
        CHECK(meanOf(act, "health") >= dungeonHp  * 0.5f);
        CHECK(meanOf(act, "damage") <= dungeonDmg * 1.6f);
        CHECK(meanOf(act, "damage") >= dungeonDmg * 0.5f);
    }

    // A zone boss goes through the SAME curve as the trash around it, so its authored health is a
    // ratio, not an absolute. It must read as a boss (well above a mob) without being unkillable.
    // Before this, bosses were spawned UNSCALED — Griswald had 2% of a trash mob's health.
    for (const auto& e : doc["enemies"]) {
        if (!e.value("unique", false)) continue;
        const std::string name = e.value("name", "?");
        const f32 hp = e.value("health", 0.0f);
        CAPTURE(name); CAPTURE(hp);
        CHECK(hp >= dungeonHp * 8.0f);      // unmistakably a boss
        CHECK(hp <= dungeonHp * 80.0f);     // still something a post-Inferno hero can chew through
    }
}

// A zone boss must fight like the dungeon's bosses do ------------------------------------------
//
// Zone bosses run the SAME difficulty curve as the trash around them, so their authored numbers are
// RATIOS against that trash. The dungeon's own bosses set the convention, and it is not the obvious
// one: they carry 3.7-37.4x trash HP but only 0.78-2.22x trash DAMAGE — the late ones hit for LESS
// per swing than a mob. That is deliberate. A boss is an attrition fight, and the game is balanced
// to roughly 1.8 hits-to-die from ordinary trash, so a boss swinging 4x would simply one-shot you.
//
// The first pass at the zone bosses did exactly that (2.36-4.03x damage) and nothing caught it,
// because they had been spawning UNSCALED and were harmless for the wrong reason.
TEST_CASE("zone bosses hit like bosses, not like a one-shot") {
    std::ifstream f(DUNGEON_REPO_ROOT "/assets/config/enemies.json");
    REQUIRE(f.good());
    nlohmann::json doc = nlohmann::json::parse(f);

    f32 trashHp = 0.0f, trashDmg = 0.0f; u32 n = 0;
    for (const auto& e : doc["enemies"]) {
        if (e.value("act", 0) != 0 || e.value("unique", false) || e.value("tier", 0) != 5) continue;
        trashHp += e.value("health", 0.0f); trashDmg += e.value("damage", 0.0f); n++;
    }
    REQUIRE(n > 0);
    trashHp /= static_cast<f32>(n); trashDmg /= static_cast<f32>(n);

    for (const auto& e : doc["enemies"]) {
        if (!e.value("unique", false)) continue;
        const std::string name = e.value("name", "?");
        const f32 hpRatio  = e.value("health", 0.0f) / trashHp;
        const f32 dmgRatio = e.value("damage", 0.0f) / trashDmg;
        CAPTURE(name); CAPTURE(hpRatio); CAPTURE(dmgRatio);
        // The bands are the dungeon roster's own, with a little headroom.
        CHECK(dmgRatio <= 2.3f);    // above this a boss one-shots at the balanced hits-to-die
        CHECK(dmgRatio >= 0.7f);    // below it the fight has no teeth at all
        CHECK(hpRatio  <= 40.0f);   // The Dungeon Engine, the final superboss, is 37.4x
        CHECK(hpRatio  >= 8.0f);    // must still read as a boss
    }
}
