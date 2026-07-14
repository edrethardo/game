// test_source_shard.cpp — the secret superboss key must survive long enough to be picked up.
//
// A source shard is an IRREPLACEABLE key. It drops once, from a milestone boss, and the drop is
// gated on not already holding that shard — so once the boss is dead there is no second chance. If
// the shard can vanish, the run silently loses access to the superboss with no feedback at all: the
// portal simply never opens on floor 50 and the player cannot know why.
//
// Two ways it can vanish, both silent, both pinned here:
//   1. It expires like ordinary loot (60 s lifetime).
//   2. WorldItemSystem::spawn fails on a full pool and the caller ignores the return.

#include <doctest/doctest.h>
#include "game/item.h"

static ItemInstance makeShard(u8 floor) {
    ItemInstance s;
    s.defId     = SOURCE_SHARD_ID;
    s.itemLevel = floor;
    s.uid       = 1;
    return s;   // rarity defaults to COMMON — which is exactly the trap
}

static ItemInstance makeCommonItem(u16 defId, u32 uid) {
    ItemInstance it;
    it.defId  = defId;
    it.rarity = Rarity::COMMON;
    it.uid    = uid;
    return it;
}

TEST_CASE("SourceShard: a shard does NOT despawn") {
    // The shard drops at the boss's corpse. Kill a milestone boss from range — a skill, a bow, the
    // far side of the arena — loot the rest of the haul, and take longer than a minute to wander
    // back over it, and the key to the entire secret is gone for the run.
    WorldItemPool pool;
    REQUIRE(WorldItemSystem::spawn(pool, makeShard(5), {0, 0, 0}, nullptr));

    for (int i = 0; i < 60 * 120; i++)          // two minutes at 60 Hz
        WorldItemSystem::update(pool, 1.0f / 60.0f);

    CHECK(pool.items[0].active);                // still there to be picked up
    CHECK(pool.activeCount == 1);
}

TEST_CASE("SourceShard: ordinary loot still despawns (the control)") {
    // Proves the test above is actually testing something — the expiry it survives is real, and a
    // blanket "nothing ever despawns" change would light this up.
    WorldItemPool pool;
    REQUIRE(WorldItemSystem::spawn(pool, makeCommonItem(0, 7), {0, 0, 0}, nullptr));

    for (int i = 0; i < 60 * 120; i++)
        WorldItemSystem::update(pool, 1.0f / 60.0f);

    CHECK_FALSE(pool.items[0].active);
    CHECK(pool.activeCount == 0);
}

TEST_CASE("SourceShard: a legendary still never despawns") {
    WorldItemPool pool;
    ItemInstance leg = makeCommonItem(1, 8);
    leg.rarity = Rarity::LEGENDARY;
    REQUIRE(WorldItemSystem::spawn(pool, leg, {0, 0, 0}, nullptr));

    for (int i = 0; i < 60 * 120; i++)
        WorldItemSystem::update(pool, 1.0f / 60.0f);

    CHECK(pool.items[0].active);
}

TEST_CASE("SourceShard: spawn REPORTS failure on a full pool") {
    // The boss loot path spawns the shard LAST — after the guaranteed haul, the bonus drops and the
    // globe — so it is the first thing lost when the pool fills, which is exactly the situation a
    // boss floor with a champion pack creates. The caller must therefore check this return; it is
    // the only signal that the key was never created.
    WorldItemPool pool;
    for (u32 i = 0; i < MAX_WORLD_ITEMS; i++)
        REQUIRE(WorldItemSystem::spawn(pool, makeCommonItem(0, i + 1), {0, 0, 0}, nullptr));

    CHECK(pool.activeCount == MAX_WORLD_ITEMS);
    CHECK_FALSE(WorldItemSystem::spawn(pool, makeShard(5), {0, 0, 0}, nullptr));
}

TEST_CASE("SourceShard: spawnEssential places the key even on a full pool") {
    WorldItemPool pool;
    for (u32 i = 0; i < MAX_WORLD_ITEMS; i++)
        REQUIRE(WorldItemSystem::spawn(pool, makeCommonItem(0, i + 1), {0, 0, 0}, nullptr));
    REQUIRE(pool.activeCount == MAX_WORLD_ITEMS);

    CHECK(WorldItemSystem::spawnEssential(pool, makeShard(10), {1, 0, 1}, nullptr));

    bool found = false;
    for (u32 i = 0; i < MAX_WORLD_ITEMS; i++)
        if (pool.items[i].active && isSourceShard(pool.items[i].item)) found = true;
    CHECK(found);
    CHECK(pool.activeCount == MAX_WORLD_ITEMS);   // it took a slot, it didn't grow the pool
}

TEST_CASE("SourceShard: spawnEssential evicts the drop nearest to expiring") {
    // The cheapest thing to lose is the one that was about to vanish by itself.
    WorldItemPool pool;
    for (u32 i = 0; i < MAX_WORLD_ITEMS; i++)
        REQUIRE(WorldItemSystem::spawn(pool, makeCommonItem(0, i + 1), {0, 0, 0}, nullptr));

    const u32 doomed = 17;
    pool.items[doomed].lifetime = 0.5f;           // nearly gone anyway
    const u32 doomedUid = pool.items[doomed].item.uid;

    REQUIRE(WorldItemSystem::spawnEssential(pool, makeShard(15), {2, 0, 2}, nullptr));

    bool doomedStillHere = false;
    for (u32 i = 0; i < MAX_WORLD_ITEMS; i++)
        if (pool.items[i].active && pool.items[i].item.uid == doomedUid) doomedStillHere = true;
    CHECK_FALSE(doomedStillHere);
}

TEST_CASE("SourceShard: spawnEssential never evicts a legendary or another sentinel") {
    // Trading a legendary (which never despawns) or another key for this one would just move the
    // loss somewhere else.
    WorldItemPool pool;
    for (u32 i = 0; i < MAX_WORLD_ITEMS; i++) {
        ItemInstance leg = makeCommonItem(1, i + 1);
        leg.rarity = Rarity::LEGENDARY;
        REQUIRE(WorldItemSystem::spawn(pool, leg, {0, 0, 0}, nullptr));
    }

    // Nothing expendable exists → it must refuse and say so, not quietly eat a legendary.
    CHECK_FALSE(WorldItemSystem::spawnEssential(pool, makeShard(20), {0, 0, 0}, nullptr));
    for (u32 i = 0; i < MAX_WORLD_ITEMS; i++)
        CHECK(pool.items[i].active);              // every legendary survived
}

TEST_CASE("SourceShard: the sentinel is recognised and is never ordinary loot") {
    const ItemInstance s = makeShard(25);
    CHECK(isSourceShard(s));
    CHECK(isSentinelItem(s));      // must never enter a backpack
    CHECK_FALSE(isGlobe(s));
    CHECK_FALSE(isShrine(s));
}

TEST_CASE("SourceShard: every milestone floor maps to a distinct bit, and 10 shards complete the set") {
    // collectSourceShard derives the bit as floor/5 - 1. Floors 5..50 step 5 → bits 0..9 → 0x3FF,
    // which is the exact value updateSourcePortal compares against to open the portal.
    u16 set = 0;
    int count = 0;
    for (u8 floor = 5; floor <= 50; floor += 5) {
        const u8 bit = static_cast<u8>(floor / 5 - 1);
        CHECK(bit < 10);
        CHECK((set & (1u << bit)) == 0);   // no two milestone floors collide on a bit
        set |= static_cast<u16>(1u << bit);
        count++;
    }
    CHECK(count == 10);
    CHECK(set == 0x03FFu);                 // the portal's open condition
}
