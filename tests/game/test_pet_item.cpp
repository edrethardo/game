#include <doctest/doctest.h>
#include "game/item.h"
#include "world/level_grid.h"

#include <json/nlohmann/json.hpp>
#include <cstring>
#include <fstream>
#include <set>
#include <string>

// Pet consumables (the loot goblin's 1% drop + every enemy's 1-in-10000 "Mini <Enemy>") — invariants:
//
// 1. The pet def is UNROLLABLE. minLevel 255 excludes it from BOTH of rollItem's candidate
//    passes (wrappedLevel is 1-50), and dropWeight 0 would keep its pick probability at zero
//    even if it slipped in. The 1% roll in handleGoblinLootDrop is the item's ONLY source —
//    if it ever leaked into the normal loot table, every boss/champion/goblin legendary
//    re-roll loop ("roll until maxRarity >= LEGENDARY") could hand out pets like candy.
//
// 2. Inventory::equip refuses it. The def claims the RING slot purely to satisfy the loader;
//    "using" the item is intercepted before every equip path (Engine::tryUsePetItem), and the
//    in-equip guard is the backstop covering drag-to-equip and any future caller.

TEST_CASE("ItemGen: a petSummon def (minLevel 255, dropWeight 0) is never rolled") {
    ItemDef defs[2] = {};
    defs[0].slot = ItemSlot::WEAPON;
    defs[0].minLevel = 1;   defs[0].maxLevel = 50;
    defs[0].dropWeight = 1.0f;
    defs[1].slot = ItemSlot::RING;
    defs[1].minLevel = 255; defs[1].maxLevel = 255;   // outside every wrapped level (1-50)
    defs[1].dropWeight = 0.0f;
    defs[1].petSummon = true;

    AffixDef noAffixes[1] = {};   // count 0 — affix rolling isn't under test here

    // Sweep levels across the 1-50 wrap boundary (Nightmare/Hell re-use the same tables).
    for (u32 lvl = 1; lvl <= 120; lvl += 7) {
        for (u32 n = 0; n < 200; n++) {
            ItemInstance it = ItemGen::rollItem(static_cast<u8>(lvl), defs, 2, noAffixes, 0);
            REQUIRE(it.defId == 0);   // only the weapon def is ever eligible
        }
    }
}

TEST_CASE("Inventory: equip refuses a pet-summon consumable (stays in the bag)") {
    ItemDef defs[1] = {};
    defs[0].slot = ItemSlot::RING;
    defs[0].petSummon = true;

    PlayerInventory inv;
    Inventory::init(inv);

    ItemInstance pet;
    pet.defId  = 0;
    pet.rarity = Rarity::LEGENDARY;
    pet.uid    = 7;
    s32 slot = Inventory::addToBackpack(inv, pet);
    REQUIRE(slot >= 0);

    Inventory::equip(inv, static_cast<u8>(slot), defs);

    CHECK(inv.backpack[slot].uid == 7);                                    // never left the bag
    CHECK(isItemEmpty(inv.equipped[static_cast<u32>(ItemSlot::RING)]));    // ring slot untouched
}

// 3. items.json and enemies.json stay in SYNC: every enemy has exactly one "Mini <Enemy>"
//    pet def, correctly shaped (unrollable, COMMON, petSummon, petEnemy naming the enemy).
//    This is the sync trap made loud — the pet defs were generated FROM enemies.json once;
//    a new enemy added without its pet def would otherwise just silently never drop one.
TEST_CASE("Config: every enemies.json entry has a matching unrollable pet item def") {
    std::ifstream ef(DUNGEON_REPO_ROOT "/assets/config/enemies.json");
    REQUIRE(ef.good());
    nlohmann::json ejson = nlohmann::json::parse(ef, nullptr, /*allow_exceptions=*/false);
    REQUIRE(!ejson.is_discarded());

    // Load the real item table through the real loader — the same path the engine uses.
    static ItemDef defs[MAX_ITEM_DEFS];   // static: MAX_ITEM_DEFS × sizeof(ItemDef) is stack-hostile
    u32 defCount = 0;
    REQUIRE(ItemLoader::loadItemDefs(DUNGEON_REPO_ROOT "/assets/config/items.json", defs, defCount));

    // Index the pet defs by the enemy they miniaturize.
    std::set<std::string> petEnemies;
    for (u32 i = 0; i < defCount; i++) {
        if (!defs[i].petSummon || defs[i].petEnemyName[0] == '\0') continue;
        CHECK(defs[i].minLevel == 255);                  // unrollable — the jackpot is the only source
        CHECK(defs[i].dropWeight == 0.0f);
        CHECK(defs[i].maxRarity == Rarity::COMMON);      // "a common pet" — rate is the prestige
        CHECK(petEnemies.insert(defs[i].petEnemyName).second);   // one pet per enemy, no dupes
    }

    u32 enemyCount = 0;
    for (const auto& en : ejson["enemies"]) {
        const std::string name = en.value("name", "");
        REQUIRE(!name.empty());
        INFO("enemy without a pet item def: " << name);
        CHECK(petEnemies.count(name) == 1);
        enemyCount++;
    }
    CHECK(enemyCount > 0);
    CHECK(petEnemies.size() == enemyCount);   // no orphaned pet defs pointing at deleted enemies
}

// 4. A pet drop never despawns. It rolls COMMON (the trash tier the 60 s timer exists for),
//    so the def-aware exemption in WorldItemSystem::update is what keeps a 1-in-10000 drop
//    from rotting while the player is still fighting across the room.
TEST_CASE("WorldItemSystem: a petSummon drop outlives the 60 s trash timer") {
    ItemDef defs[1] = {};
    defs[0].slot = ItemSlot::RING;
    defs[0].petSummon = true;

    WorldItemPool pool;
    WorldItemSystem::init(pool);

    ItemInstance pet;
    pet.defId  = 0;
    pet.rarity = Rarity::COMMON;
    pet.uid    = pool.nextUid++;
    REQUIRE(WorldItemSystem::spawn(pool, pet, {1, 0, 1}, nullptr));

    ItemInstance trash;                     // control: an ordinary COMMON drop on the same floor
    trash.defId  = 0xBEEF;                  // out of def range → no exemption lookup
    trash.rarity = Rarity::COMMON;
    trash.uid    = pool.nextUid++;
    REQUIRE(WorldItemSystem::spawn(pool, trash, {2, 0, 2}, nullptr));

    for (u32 i = 0; i < 60 * 61; i++)       // 61 simulated seconds at 60 Hz
        WorldItemSystem::update(pool, 1.0f / 60.0f, defs, 1);

    CHECK(pool.items[0].active);            // the pet is still there
    CHECK(!pool.items[1].active);           // the control expired on schedule
}

// 5. A spawned drop's STORED position rests on its supporting surface (story-aware). This is the
//    guard against the airborne-death bug: a flying bat (or a pad-launched/vaulting chaser) dies at
//    1.5-3 m, and the raw death Y used to be stored verbatim — the renderer snapped the MODEL to
//    the floor while interact/pickup measured the stored mid-air Y against INTERACT_VERTICAL_REACH
//    (2 m), leaving visible loot that no mode could grab and no tooltip to say why. Spawn now snaps
//    the stored Y through the same effectiveFloorHeight read the renderer uses, so the two agree.
TEST_CASE("WorldItemSystem: spawn rests the stored position on the story surface under it") {
    LevelGrid g;
    LevelGridSystem::init(g, 8, 8, 1.0f);
    for (u32 z = 0; z < 8; z++)
        for (u32 x = 0; x < 8; x++) {
            GridCell& c = LevelGridSystem::getCell(g, x, z);
            c.flags = CELL_FLOOR | CELL_CEILING;
            c.floorHeight = 0;
            c.ceilingHeight = 32;   // 8 m — clears the balcony slab below
        }
    // Cell (5,5) carries a 3 m balcony slab (the vhall arcade shape).
    LevelGridSystem::setPlatform(LevelGridSystem::getCell(g, 5, 5), 12, 1);

    WorldItemPool pool;
    WorldItemSystem::init(pool);
    ItemInstance it;
    it.defId = 0; it.rarity = Rarity::COMMON;

    // A bat's death at 2.5 m over open floor: the drop lands on the ground.
    it.uid = pool.nextUid++;
    REQUIRE(WorldItemSystem::spawn(pool, it, {2.5f, 2.5f, 2.5f}, &g));
    CHECK(pool.items[0].position.y == doctest::Approx(0.0f));

    // A kill ON the balcony (feet 3 m, death pos ~3.4): the loot STAYS on the balcony slab.
    it.uid = pool.nextUid++;
    REQUIRE(WorldItemSystem::spawn(pool, it, {5.5f, 3.4f, 5.5f}, &g));
    CHECK(pool.items[1].position.y == doctest::Approx(3.0f));

    // An airborne death UNDER the balcony (walk-under arcade, 1.5 m): lands on the ground, not
    // teleported up onto the slab above.
    it.uid = pool.nextUid++;
    REQUIRE(WorldItemSystem::spawn(pool, it, {5.5f, 1.5f, 5.5f}, &g));
    CHECK(pool.items[2].position.y == doctest::Approx(0.0f));

    // No grid (legacy/net-mirror callers): the position is stored verbatim.
    it.uid = pool.nextUid++;
    REQUIRE(WorldItemSystem::spawn(pool, it, {3.5f, 2.5f, 3.5f}, nullptr));
    CHECK(pool.items[3].position.y == doctest::Approx(2.5f));

    LevelGridSystem::shutdown(g);
}
