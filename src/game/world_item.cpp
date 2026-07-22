// world_item.cpp — World item pool: spawn, update (lifetime/bob), and pickup logic.
#include "game/item.h"
#include "game/shrine.h"
#include "core/log.h"
#include "world/collision.h"

#include <cmath>

// ============================================================
//  WorldItemSystem
// ============================================================

void WorldItemSystem::init(WorldItemPool& pool) {
    pool = WorldItemPool{};
    for (u32 i = 0; i < MAX_WORLD_ITEMS; i++) {
        pool.items[i] = WorldItem{};
        pool.items[i].active = false;
    }
    pool.activeCount = 0;
    pool.nextUid     = 0x80000000u;   // high half — disjoint from ItemGen's rolled-item uids (see item.h)
    LOG_INFO("WorldItemSystem: pool initialized (%u slots)", MAX_WORLD_ITEMS);
}

void WorldItemSystem::update(WorldItemPool& pool, f32 dt,
                             const ItemDef* defs, u32 defCount) {
    for (u32 i = 0; i < MAX_WORLD_ITEMS; i++) {
        WorldItem& wi = pool.items[i];
        if (!wi.active) continue;

        // Pet consumables (COMMON rarity, 1-in-10000 drops) are exempt from decay like
        // legendaries: losing one to the trash timer while the player fights across the
        // room would be brutal. Def-aware here — see the header on why not a spawn flag.
        const bool isPet = defs && wi.item.defId < defCount && defs[wi.item.defId].petSummon;

        // Legendary items never despawn — persist until floor exit.
        // Shrines never despawn either: they are world FIXTURES you walk over to and activate, not
        // loot lying on the floor. Without this they would quietly evaporate 60 s after the floor
        // loaded — usually before the player had even found the room.
        // A SOURCE SHARD never despawns because it is an irreplaceable KEY: it drops once per
        // milestone boss, the drop is gated on not already holding it, and the boss is dead — so
        // there is no second chance. Kill that boss from range, loot the rest of the haul, and take
        // a minute to wander back over the corpse, and the run silently loses the superboss with no
        // feedback whatsoever: the portal just never opens on floor 50. Its rarity is COMMON, so it
        // was expiring exactly like the trash it drops next to.
        // Chests are furniture, not loot: they must wait unopened however long the player
        // takes to reach the room (and a despawning "chest" beside a permanent mimic would
        // be a free mimic detector).
        if (wi.item.rarity != Rarity::LEGENDARY && !isShrine(wi.item) && !isSourceShard(wi.item)
            && !isChest(wi.item) && !isStash(wi.item) && !isPet) {
            wi.lifetime -= dt;
        }
        wi.bobTimer       += dt;
        wi.exclusiveTimer -= dt;

        if (wi.lifetime <= 0.0f) {
            wi.active = false;
            if (pool.activeCount > 0)
                pool.activeCount--;
        }
    }
}

bool WorldItemSystem::spawn(WorldItemPool& pool, const ItemInstance& item, Vec3 position,
                              const LevelGrid* grid, u8 ownerSlot, f32 exclusiveSeconds) {
    // Nudge item out of walls if grid is provided
    if (grid) {
        Vec3 itemHalf = {0.15f, 0.15f, 0.15f}; // small AABB for item
        Collision::ensureNotInWall(position, itemHalf, *grid);

        // Rest the STORED position on the supporting surface under the spawn point (story-aware:
        // a balcony kill's loot stays on the balcony, a mid-air death's drop lands on whatever is
        // below). This is the same effectiveFloorHeight read the renderer resolves, and that
        // agreement is the bug-fix: an enemy killed AIRBORNE (a flying bat at 1.5-3 m, a
        // pad-launched or vaulting chaser mid-arc) used to store its drop at death height — the
        // model DREW on the floor at your feet, but interact/pickup measured against the stored
        // mid-air Y and refused anything past INTERACT_VERTICAL_REACH: visible loot that no mode
        // could ever grab, with nothing on screen to say why.
        u32 gx, gz;
        if (LevelGridSystem::worldToGrid(*grid, position, gx, gz) &&
            !LevelGridSystem::isSolid(*grid, gx, gz))
            position.y = LevelGridSystem::effectiveFloorHeight(*grid, gx, gz, position.y);
    }

    for (u32 i = 0; i < MAX_WORLD_ITEMS; i++) {
        WorldItem& wi = pool.items[i];
        if (wi.active) continue;

        wi.item          = item;
        wi.position      = position;
        wi.bobTimer      = 0.0f;
        wi.lifetime      = 60.0f;
        wi.exclusiveTimer = exclusiveSeconds;
        wi.ownerSlot     = ownerSlot;
        wi.active        = true;
        pool.activeCount++;
        return true;
    }

    LOG_WARN("WorldItemSystem: pool full, cannot spawn item");
    return false;
}

// See the header. A boss floor can genuinely fill all MAX_WORLD_ITEMS slots — the boss's guaranteed
// haul plus its bonus drops plus a champion pack's guaranteed leader drop plus globes — and the
// shard is spawned LAST of all of them, which makes it the first thing lost. Losing it costs the
// run the entire superboss, with no message and no way to tell it happened. An expiring common drop
// is a far cheaper thing to lose than the key.
bool WorldItemSystem::spawnEssential(WorldItemPool& pool, const ItemInstance& item, Vec3 position,
                                     const LevelGrid* grid, const ItemDef* defs, u32 defCount) {
    if (spawn(pool, item, position, grid)) return true;

    // Evict the expendable item closest to expiring: it was about to vanish on its own anyway.
    s32 victim = -1;
    f32 lowestLifetime = 1e9f;
    for (u32 i = 0; i < MAX_WORLD_ITEMS; i++) {
        WorldItem& wi = pool.items[i];
        if (!wi.active) continue;
        if (isSentinelItem(wi.item)) continue;              // never evict a key, shrine or globe
        if (wi.item.rarity == Rarity::LEGENDARY) continue;  // legendaries never despawn; don't start
        if (defs && wi.item.defId < defCount && defs[wi.item.defId].petSummon)
            continue;                                       // a 1-in-10000 companion outranks a key's slot claim
        if (wi.lifetime < lowestLifetime) { lowestLifetime = wi.lifetime; victim = static_cast<s32>(i); }
    }
    if (victim < 0) {
        LOG_ERROR("WorldItemSystem: pool full and nothing evictable — ESSENTIAL item LOST");
        return false;
    }

    pool.items[victim].active = false;
    if (pool.activeCount > 0) pool.activeCount--;
    LOG_WARN("WorldItemSystem: pool full — evicted an expiring drop to place an essential item");
    return spawn(pool, item, position, grid);
    return false;
}

bool WorldItemSystem::tryPickup(WorldItemPool& pool, Vec3 playerPos, u8 playerSlot,
                                  ItemInstance& outItem) {
    static constexpr f32 PICKUP_RADIUS = 3.5f;

    for (u32 i = 0; i < MAX_WORLD_ITEMS; i++) {
        WorldItem& wi = pool.items[i];
        if (!wi.active) continue;

        Vec3 delta = {
            playerPos.x - wi.position.x,
            playerPos.y - wi.position.y,
            playerPos.z - wi.position.z
        };
        f32 dist = sqrtf(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
        if (dist >= PICKUP_RADIUS) continue;

        // Check ownership: free-for-all, owned by this player, or exclusive timer expired
        bool canPickup = (wi.ownerSlot == 0xFF)
                      || (wi.ownerSlot == playerSlot)
                      || (wi.exclusiveTimer <= 0.0f);
        if (!canPickup) continue;

        outItem   = wi.item;
        wi.active = false;
        if (pool.activeCount > 0)
            pool.activeCount--;
        return true;
    }

    return false;
}
