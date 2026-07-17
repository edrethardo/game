// test_crowd_control.cpp — pure CC math: tenacity duration scaling, the 60% resist cap,
// and the PvP stun diminishing-returns ladder. No engine deps (arena.h/stash.h pattern).
#include <doctest/doctest.h>
#include "game/crowd_control.h"
#include "game/item.h"     // Inventory:: on-demand helpers + ItemInstance/AffixType/ItemSlot

static ItemInstance mkResistItem(f32 resist) {
    ItemInstance it{};
    it.defId = 0;                    // any valid def; ccResist only reads affixes
    it.affixCount = 1;
    it.affixes[0].type  = AffixType::CC_RESIST;
    it.affixes[0].value = resist;
    return it;
}

TEST_CASE("CC: tenacity scales duration, cap clamps at 0.60") {
    CHECK(CrowdControl::scaleDuration(2.0f, 0.0f)  == doctest::Approx(2.0f));
    CHECK(CrowdControl::scaleDuration(2.0f, 0.30f) == doctest::Approx(1.4f));
    CHECK(CrowdControl::scaleDuration(2.0f, 0.60f) == doctest::Approx(0.8f));
    // Over-cap resist is clamped by capResist, never by scaleDuration itself:
    CHECK(CrowdControl::capResist(0.95f) == doctest::Approx(0.60f));
    CHECK(CrowdControl::capResist(0.25f) == doctest::Approx(0.25f));
}

TEST_CASE("CC: stun DR ladder 1.0 -> 0.5 -> 0.25 -> 0.0, then window reset") {
    CrowdControl::StunDr dr;                       // fresh: count 0, timer 0
    CHECK(CrowdControl::advanceStunDr(dr, 8.0f) == doctest::Approx(1.0f));  // 1st
    CHECK(CrowdControl::advanceStunDr(dr, 8.0f) == doctest::Approx(0.5f));  // 2nd
    CHECK(CrowdControl::advanceStunDr(dr, 8.0f) == doctest::Approx(0.25f)); // 3rd
    CHECK(CrowdControl::advanceStunDr(dr, 8.0f) == doctest::Approx(0.0f));  // 4th: immune
    CHECK(dr.count == 4);
    // Window lapses -> count resets, next stun is full again.
    CrowdControl::tickStunDr(dr, 9.0f);            // 9s > 8s window
    CHECK(dr.count == 0);
    CHECK(CrowdControl::advanceStunDr(dr, 8.0f) == doctest::Approx(1.0f));
}

TEST_CASE("Inventory::ccResist sums equipped CC_RESIST and clamps to 0.60") {
    PlayerInventory inv{};
    CHECK(Inventory::ccResist(inv) == doctest::Approx(0.0f));           // nothing equipped
    inv.equipped[(u32)ItemSlot::BOOTS] = mkResistItem(0.30f);
    CHECK(Inventory::ccResist(inv) == doctest::Approx(0.30f));
    inv.equipped[(u32)ItemSlot::HELMET] = mkResistItem(0.10f);          // stacks
    CHECK(Inventory::ccResist(inv) == doctest::Approx(0.40f));
    inv.equipped[(u32)ItemSlot::ARMOR] = mkResistItem(0.50f);           // would be 0.90
    CHECK(Inventory::ccResist(inv) == doctest::Approx(0.60f));          // clamped
}

TEST_CASE("resolveCC: resist scales, immunity/dodge negate, stun DR only in PvP") {
    using CrowdControl::resolveCC; using CrowdControl::CcKind; using CrowdControl::StunDr;
    StunDr dr;

    // Slow scales by resist (0.5), never diminishes:
    auto slow = resolveCC(CcKind::SLOW, 2.0f, 0.50f, false, false, dr, true);
    CHECK(slow.apply);
    CHECK(slow.duration == doctest::Approx(1.0f));

    // Immunity fully negates — and must NOT burn a DR stack (dr untouched):
    auto immune = resolveCC(CcKind::STUN, 2.0f, 0.0f, /*immune=*/true, false, dr, true);
    CHECK_FALSE(immune.apply);
    CHECK(dr.count == 0);

    // Dodge i-frame negate — also no DR stack spent:
    auto dodged = resolveCC(CcKind::STUN, 2.0f, 0.0f, false, /*dodgeNegate=*/true, dr, true);
    CHECK_FALSE(dodged.apply);
    CHECK(dr.count == 0);

    // PvP stun: resist (0.5) then DR first-hit (1.0) -> 1.0s, and NOW a stack is spent:
    auto stun = resolveCC(CcKind::STUN, 2.0f, 0.50f, false, false, dr, true);
    CHECK(stun.apply);
    CHECK(stun.duration == doctest::Approx(1.0f));
    CHECK(dr.count == 1);

    // PvE stun (isPvp=false): resist only, DR untouched (DR is PvP-victims-only):
    StunDr dr2;
    auto pve = resolveCC(CcKind::STUN, 2.0f, 0.0f, false, false, dr2, false);
    CHECK(pve.apply);
    CHECK(pve.duration == doctest::Approx(2.0f));
    CHECK(dr2.count == 0);
}
