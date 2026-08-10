// World-item pool pressure — what happens when the floor is already carrying 64 drops.
//
// Legendaries and mythics NEVER despawn (they persist until the floor is left), so on a deep floor
// they accumulate until every slot is taken. From that moment the old spawn() refused whatever
// arrived NEXT — which is the worst possible choice, because the thing arriving is as likely to be
// a mythic as the sixty commons sitting there waiting out a 60 s timer. A 3 h soak measured 1448
// such losses, all on deep floors; a class that never left Normal saw zero.
//
// The rule now is a STRICT UPGRADE, mirroring the backpack's autoEvictWorst: make room by dropping
// the cheapest thing present, but only when it is genuinely worse than the incoming item. These
// tests pin the resulting guarantees — never lose the better item, never trade down, and never take
// a sentinel's or a pet's slot to do it.

#include <doctest/doctest.h>
#include "game/item.h"

namespace {

ItemInstance mk(Rarity r, u16 defId = 1, f32 /*unused*/ = 0.0f) {
    ItemInstance it{};
    it.defId  = defId;
    it.rarity = r;
    it.uid    = 1;
    return it;
}

// Fill every slot with `r`, returning the pool by reference. Lifetimes are staggered so "closest to
// expiring" is well-defined among equals.
void fillWith(WorldItemPool& pool, Rarity r) {
    pool = WorldItemPool{};
    for (u32 i = 0; i < MAX_WORLD_ITEMS; i++) {
        REQUIRE(WorldItemSystem::spawn(pool, mk(r, 1), Vec3{0, 0, 0}));
        pool.items[i].lifetime = 10.0f + static_cast<f32>(i);
    }
    REQUIRE(pool.activeCount == MAX_WORLD_ITEMS);
}

u32 countOf(const WorldItemPool& pool, Rarity r) {
    u32 n = 0;
    for (u32 i = 0; i < MAX_WORLD_ITEMS; i++)
        if (pool.items[i].active && pool.items[i].item.rarity == r) n++;
    return n;
}

} // namespace

TEST_CASE("a better drop is never lost to a pool full of worse loot") {
    static WorldItemPool pool;
    fillWith(pool, Rarity::COMMON);

    // The exact case measured in the soak: the floor is saturated and a mythic drops.
    CHECK(WorldItemSystem::spawn(pool, mk(Rarity::MYTHIC, 2), Vec3{1, 0, 1}));
    CHECK(pool.activeCount == MAX_WORLD_ITEMS);          // still full — it took a slot, not an extra
    CHECK(countOf(pool, Rarity::MYTHIC) == 1);           // …and the mythic is the one on the floor
    CHECK(countOf(pool, Rarity::COMMON) == MAX_WORLD_ITEMS - 1);
}

TEST_CASE("a worse drop is declined rather than trading down") {
    static WorldItemPool pool;
    fillWith(pool, Rarity::LEGENDARY);

    // Nothing here is worse than a common, so the common is genuinely the least valuable thing in
    // play and refusing it is correct. The failure this guards against is the opposite: evicting a
    // legendary to make room for trash, which would be a far worse bug than the one being fixed.
    CHECK_FALSE(WorldItemSystem::spawn(pool, mk(Rarity::COMMON, 3), Vec3{1, 0, 1}));
    CHECK(countOf(pool, Rarity::LEGENDARY) == MAX_WORLD_ITEMS);
    CHECK(countOf(pool, Rarity::COMMON) == 0);
}

TEST_CASE("an equal-rarity drop is declined — the exchange must be a strict upgrade") {
    static WorldItemPool pool;
    fillWith(pool, Rarity::RARE);

    // Equal is not better. Allowing it would let a floor's worth of same-tier drops churn through
    // the pool endlessly, replacing each other for no gain — the same shape as the backpack
    // eviction loop that once respawned one item 699 times in 26 minutes.
    CHECK_FALSE(WorldItemSystem::spawn(pool, mk(Rarity::RARE, 4), Vec3{1, 0, 1}));
    CHECK(countOf(pool, Rarity::RARE) == MAX_WORLD_ITEMS);
}

TEST_CASE("eviction takes the cheapest slot, not merely any slot") {
    static WorldItemPool pool;
    pool = WorldItemPool{};
    // A mixed floor: mostly rares, one common. The common must be the one that goes.
    for (u32 i = 0; i < MAX_WORLD_ITEMS; i++) {
        const Rarity r = (i == 7) ? Rarity::COMMON : Rarity::RARE;
        REQUIRE(WorldItemSystem::spawn(pool, mk(r, 1), Vec3{0, 0, 0}));
        pool.items[i].lifetime = 30.0f;      // equal lifetimes, so only rarity can decide
    }

    CHECK(WorldItemSystem::spawn(pool, mk(Rarity::LEGENDARY, 5), Vec3{1, 0, 1}));
    CHECK(countOf(pool, Rarity::COMMON) == 0);                       // the cheapest one left
    CHECK(countOf(pool, Rarity::RARE) == MAX_WORLD_ITEMS - 1);       // every rare survived
    CHECK(countOf(pool, Rarity::LEGENDARY) == 1);
}

TEST_CASE("a sentinel's slot is never taken") {
    static WorldItemPool pool;
    pool = WorldItemPool{};
    for (u32 i = 0; i < MAX_WORLD_ITEMS; i++) {
        ItemInstance it = mk(Rarity::COMMON, 1);
        if (i == 3) it.defId = SOURCE_SHARD_ID;   // the superboss key — irreplaceable
        if (i == 4) it.defId = WAYPOINT_ID;       // a fixture, not loot
        REQUIRE(WorldItemSystem::spawn(pool, it, Vec3{0, 0, 0}));
        pool.items[i].lifetime = (i == 3 || i == 4) ? 1.0f : 50.0f;   // make them look expendable
    }

    // Both sentinels have the SHORTEST lifetimes here, so a rule that ranked on lifetime alone
    // would take one of them first. Losing the shard silently costs the run its secret boss.
    CHECK(WorldItemSystem::spawn(pool, mk(Rarity::MYTHIC, 6), Vec3{1, 0, 1}));
    bool shard = false, waypoint = false;
    for (u32 i = 0; i < MAX_WORLD_ITEMS; i++) {
        if (!pool.items[i].active) continue;
        if (isSourceShard(pool.items[i].item)) shard = true;
        if (isWaypoint(pool.items[i].item))    waypoint = true;
    }
    CHECK(shard);
    CHECK(waypoint);
}

// --- World fixtures must outlive their 60 s loot timer -------------------------------------------
//
// A waypoint, a POI mouth, a shrine, a chest, the stash and the Source shard are FIXTURES you walk
// to, not loot lying on the floor. They spawn with the same 60 s lifetime as a dropped item, so the
// despawn pass has to exempt them explicitly — and that hand-maintained list has been wrong three
// times. Shrines and Source shards were each added after they evaporated in play; the overworld's
// waypoints and ZONE GATES were still missing, so every waypoint and every act entrance vanished a
// minute after the zone loaded. Fast travel silently died, and the Den of Evil became unreachable.
//
// A 6-second smoke test cannot see this. The rule is now "every sentinel except the globe", so a
// new sentinel is safe by default; these pin both halves of that.

TEST_CASE("fixtures never despawn, globes and loot still do") {
    static WorldItemPool pool;
    pool = WorldItemPool{};

    struct Case { u16 defId; const char* what; bool expires; };
    const Case cases[] = {
        { WAYPOINT_ID,     "waypoint",     false },
        { ZONE_GATE_ID,    "zone gate",    false },
        { CAIRN_STONE_ID,  "cairn stone",  false },
        { SHRINE_POWER_ID, "shrine",       false },
        { CHEST_ID,        "chest",        false },
        { STASH_ID,        "stash",        false },
        { GLOBE_HEALTH_ID, "health globe", true  },
        { SOURCE_SHARD_ID, "source shard", false },
        { 1,               "ordinary loot", true  },
    };

    for (const Case& c : cases) {
        pool = WorldItemPool{};
        ItemInstance it{};
        it.defId  = c.defId;
        it.rarity = Rarity::COMMON;          // nothing here may rely on rarity to survive
        it.uid    = 1;
        REQUIRE(WorldItemSystem::spawn(pool, it, Vec3{0, 0, 0}));

        // Two full lifetimes of simulated time, a second at a go.
        for (u32 i = 0; i < 120; i++) WorldItemSystem::update(pool, 1.0f, nullptr, 0);

        CAPTURE(c.what);
        CHECK(pool.items[0].active == !c.expires);
    }
}
