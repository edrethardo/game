// Shared loot: every ground drop is free-for-all from the instant it lands.
//
// Kill drops used to spawn with ownerSlot = killerSlot and a 3 s exclusivity window;
// the non-killer walking to a fresh drop and pressing E was silently rejected — which
// read as "we don't get the same loot" in co-op (while globes, always FFA, worked for
// both). WorldItemSystem::spawn's exclusiveSeconds now defaults to 0 so the PARTY
// decides who takes a drop, not a kill-credit timer. These tests pin that default and
// the pickup gate it feeds; the exclusivity mechanism itself stays intact for a future
// FFA/public-lobby mode (a spawn site can re-arm it explicitly).

#include "doctest/doctest.h"
#include "game/item.h"

namespace {
ItemInstance makeItem(u32 uid) {
    ItemInstance it{};
    it.defId  = 1;
    it.rarity = Rarity::COMMON;
    it.uid    = uid;
    return it;
}
} // namespace

TEST_CASE("shared loot: the non-killer can pick up a fresh kill drop immediately") {
    WorldItemPool pool{};
    WorldItemSystem::init(pool);

    // Killer is slot 1 — spawn with kill credit, default exclusivity (now 0 s).
    REQUIRE(WorldItemSystem::spawn(pool, makeItem(42), Vec3{5.0f, 0.3f, 5.0f},
                                   /*grid=*/nullptr, /*ownerSlot=*/1));

    // Slot 0 (NOT the killer) stands on the drop and grabs it with zero waiting.
    ItemInstance out{};
    CHECK(WorldItemSystem::tryPickup(pool, Vec3{5.0f, 0.5f, 5.0f}, /*playerSlot=*/0, out));
    CHECK(out.uid == 42);
}

TEST_CASE("shared loot: an explicitly re-armed exclusivity window still gates non-owners") {
    // The mechanism is kept, not deleted — a spawn site passing a real window must
    // still produce owner-only pickup until the timer runs out.
    WorldItemPool pool{};
    WorldItemSystem::init(pool);
    REQUIRE(WorldItemSystem::spawn(pool, makeItem(7), Vec3{2.0f, 0.3f, 2.0f},
                                   nullptr, /*ownerSlot=*/1, /*exclusiveSeconds=*/3.0f));

    ItemInstance out{};
    CHECK_FALSE(WorldItemSystem::tryPickup(pool, Vec3{2.0f, 0.5f, 2.0f}, /*playerSlot=*/0, out));
    CHECK(WorldItemSystem::tryPickup(pool, Vec3{2.0f, 0.5f, 2.0f}, /*playerSlot=*/1, out));
}
