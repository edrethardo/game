// test_chest.cpp — a real chest must behave like furniture, not loot.
//
// Real chests exist so the dormant mimic has something to hide among — before them, a chest
// mesh simply WAS a mimic and the disguise fooled nobody. Three silent failure modes pinned:
//   1. The chest expires like ordinary loot (60 s lifetime): rooms reached late would hold
//      only mimics, and a vanishing "chest" beside a permanent mimic is a free mimic detector.
//   2. The sentinel leaks into an inventory (isSentinelItem must cover it).
//   3. Sentinel uids collide with rolled-loot uids: ItemGen's counter and pool.nextUid both
//      used to restart at 1 every floor, and a guest's CL_PICKUP_ITEM is matched by uid,
//      first hit wins — a collision could open the wrong object.

#include <doctest/doctest.h>
#include "game/item.h"

static ItemInstance makeChest(u8 lootLevel, u32 uid) {
    ItemInstance c;
    c.defId     = CHEST_ID;
    c.itemLevel = lootLevel;
    c.uid       = uid;
    return c;   // rarity defaults COMMON — the expiry exemption must not lean on rarity
}

TEST_CASE("Chest: does NOT despawn") {
    WorldItemPool pool;
    REQUIRE(WorldItemSystem::spawn(pool, makeChest(5, pool.nextUid++), {0, 0, 0}, nullptr));

    for (int i = 0; i < 60 * 120; i++)          // two minutes at 60 Hz
        WorldItemSystem::update(pool, 1.0f / 60.0f);

    CHECK(pool.items[0].active);                // still waiting, however long you take
    CHECK(pool.activeCount == 1);
}

TEST_CASE("Chest: is a sentinel and only a chest") {
    const ItemInstance c = makeChest(1, 1);
    CHECK(isChest(c));
    CHECK(isSentinelItem(c));   // can never enter a backpack or be dropped
    CHECK_FALSE(isShrine(c));
    CHECK_FALSE(isGlobe(c));
    CHECK_FALSE(isSourceShard(c));
}

TEST_CASE("Chest: sentinel uids live in the high half of u32") {
    WorldItemPool pool;
    CHECK(pool.nextUid >= 0x80000000u);
}
