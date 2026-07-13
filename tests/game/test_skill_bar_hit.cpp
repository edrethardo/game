// test_skill_bar_hit.cpp — geometry for the skill bars drawn on the inventory (Tab) screen.
//
// This math is what turns a cursor position into "which skill is the player asking about", and it
// backs BOTH the mouse hover and the gamepad selection (the gamepad synthesizes a cursor position,
// so there is one path, not two). A silent off-by-one here shows the wrong skill's tooltip — which
// looks exactly like the tooltip bugs this feature exists to stop — so the arithmetic is pinned.

#include <doctest/doctest.h>
#include "game/inventory_ui.h"

// Centre of class-bar slot s, in HUD coords.
static void classSlotCentre(const InventoryUI::SkillBarRects& r, u32 s, s32& x, s32& y) {
    x = static_cast<s32>(r.classX + s * (r.slot + r.gap) + r.slot * 0.5f);
    y = static_cast<s32>(r.classY + r.slot * 0.5f);
}
static void equipSlotCentre(const InventoryUI::SkillBarRects& r, u32 s, s32& x, s32& y) {
    x = static_cast<s32>(r.equipX + s * (r.slot + r.gap) + r.slot * 0.5f);
    y = static_cast<s32>(r.equipY + r.slot * 0.5f);
}

TEST_CASE("SkillBar: every class slot centre hits its own slot") {
    const auto r = InventoryUI::skillBarLayout(1280, 720, 0);
    for (u32 s = 0; s < InventoryUI::CLASS_SKILL_SLOTS; s++) {
        s32 x, y; classSlotCentre(r, s, x, y);
        bool isClass = false; u8 idx = 0xFF;
        REQUIRE(InventoryUI::skillSlotAt(r, x, y, isClass, idx));
        CHECK(isClass);
        CHECK(idx == s);
    }
}

TEST_CASE("SkillBar: every equip slot centre hits its own slot") {
    const auto r = InventoryUI::skillBarLayout(1280, 720, 6);
    REQUIRE(r.equipCount == 6);
    for (u32 s = 0; s < 6; s++) {
        s32 x, y; equipSlotCentre(r, s, x, y);
        bool isClass = true; u8 idx = 0xFF;
        REQUIRE(InventoryUI::skillSlotAt(r, x, y, isClass, idx));
        CHECK_FALSE(isClass);      // equip bar, not class bar
        CHECK(idx == s);
    }
}

TEST_CASE("SkillBar: the gap between two slots belongs to neither") {
    // The tempting bug is to bound a slot by (slot + gap) instead of slot, which makes the dead
    // space between slots resolve to the slot on its left and pops a tooltip over empty pixels.
    const auto r = InventoryUI::skillBarLayout(1280, 720, 0);
    const s32 gapX = static_cast<s32>(r.classX + r.slot + r.gap * 0.5f);  // dead centre of gap 0-1
    const s32 y    = static_cast<s32>(r.classY + r.slot * 0.5f);
    bool isClass = false; u8 idx = 0;
    CHECK_FALSE(InventoryUI::skillSlotAt(r, gapX, y, isClass, idx));
}

TEST_CASE("SkillBar: positions outside the bars miss") {
    const auto r = InventoryUI::skillBarLayout(1280, 720, 4);
    bool isClass = false; u8 idx = 0;
    // Left of the class bar, right of it, below it, and in the band between the two bars.
    CHECK_FALSE(InventoryUI::skillSlotAt(r, static_cast<s32>(r.classX - 5.0f),
                                         static_cast<s32>(r.classY + r.slot * 0.5f), isClass, idx));
    CHECK_FALSE(InventoryUI::skillSlotAt(r, static_cast<s32>(r.classX + 4 * (r.slot + r.gap) + 5.0f),
                                         static_cast<s32>(r.classY + r.slot * 0.5f), isClass, idx));
    CHECK_FALSE(InventoryUI::skillSlotAt(r, static_cast<s32>(r.classX + r.slot * 0.5f),
                                         static_cast<s32>(r.classY - 5.0f), isClass, idx));
    // Vertical gap between class bar top and equip bar bottom.
    CHECK_FALSE(InventoryUI::skillSlotAt(r, static_cast<s32>(r.classX + r.slot * 0.5f),
                                         static_cast<s32>(r.classY + r.slot + 4.0f), isClass, idx));
}

TEST_CASE("SkillBar: no equipment skills means no equip bar to hit") {
    // equipCount 0 must not leave a phantom bar at the origin that swallows clicks at (0,0)-ish.
    const auto r = InventoryUI::skillBarLayout(1280, 720, 0);
    CHECK(r.equipCount == 0);
    bool isClass = true; u8 idx = 0;
    CHECK_FALSE(InventoryUI::skillSlotAt(r, 0, 0, isClass, idx));
    CHECK_FALSE(InventoryUI::skillSlotAt(r, static_cast<s32>(r.equipX),
                                         static_cast<s32>(r.equipY), isClass, idx));
}

TEST_CASE("SkillBar: geometry scales with resolution, not just 720p") {
    // uiScale != 1. Slot size must scale, and the centres must still resolve — the split-screen and
    // 1080p paths depend on this, and a hardcoded 64px would pass every test above and fail here.
    const auto r = InventoryUI::skillBarLayout(2560, 1440, 3);
    CHECK(r.slot == doctest::Approx(InventoryUI::SKILL_SLOT * 2.0f));
    CHECK(r.gap  == doctest::Approx(InventoryUI::SKILL_GAP * 2.0f));
    for (u32 s = 0; s < InventoryUI::CLASS_SKILL_SLOTS; s++) {
        s32 x, y; classSlotCentre(r, s, x, y);
        bool isClass = false; u8 idx = 0xFF;
        REQUIRE(InventoryUI::skillSlotAt(r, x, y, isClass, idx));
        CHECK(idx == s);
    }
    for (u32 s = 0; s < 3; s++) {
        s32 x, y; equipSlotCentre(r, s, x, y);
        bool isClass = true; u8 idx = 0xFF;
        REQUIRE(InventoryUI::skillSlotAt(r, x, y, isClass, idx));
        CHECK_FALSE(isClass);
        CHECK(idx == s);
    }
}

TEST_CASE("SkillBar: equip bar is centred over the class bar and sits above it") {
    const auto r = InventoryUI::skillBarLayout(1280, 720, 2);
    const f32 classW = InventoryUI::CLASS_SKILL_SLOTS * r.slot + (InventoryUI::CLASS_SKILL_SLOTS - 1) * r.gap;
    const f32 equipW = 2 * r.slot + 1 * r.gap;
    CHECK(r.equipX + equipW * 0.5f == doctest::Approx(r.classX + classW * 0.5f));  // centred
    CHECK(r.equipY > r.classY + r.slot);                                            // strictly above
}
