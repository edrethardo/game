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
        // WORLD FIXTURES never expire. Expressed as "every sentinel EXCEPT the globe" rather than
        // as a hand-listed set, because the hand-listed set has now been wrong three times: shrines
        // and Source shards were each added retroactively after they evaporated in play, and the
        // overworld's WAYPOINTS and ZONE GATES were still missing — so every waypoint and every POI
        // mouth in both acts vanished 60 s after the zone loaded, quietly killing fast travel and
        // making the Den of Evil and the Act 2 descent unreachable to anyone who did not sprint
        // there. A sentinel added tomorrow is now exempt BY DEFAULT, which is the safe direction.
        // The health globe is the one sentinel that is genuinely consumable loot and must expire.
        const bool fixture = isSentinelItem(wi.item) && !isGlobe(wi.item);
        if (!isLegendaryOrBetter(wi.item.rarity) && !fixture && !isPet) {
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

namespace {
// The one slot-value ranking, shared by spawn() and spawnEssential() so "what is the most
// expendable thing on this floor" cannot be answered two different ways.
//
// Returns the pool index of the cheapest evictable item, or -1 if nothing may be taken. Never
// touches a sentinel (shrine / chest / stash / source shard / waypoint / zone gate), a pet
// consumable, or anything whose rarity is at least `minRarityToBeat` — so an ordinary drop can
// never displace something better than itself. Ties break on the shortest remaining lifetime: that
// item was closest to vanishing on its own anyway.
s32 findEvictable(WorldItemPool& pool, Rarity minRarityToBeat,
                  const ItemDef* defs, u32 defCount) {
    s32  victim = -1;
    u8   worstRarity = 0xFF;
    f32  worstLifetime = 1e9f;
    for (u32 i = 0; i < MAX_WORLD_ITEMS; i++) {
        WorldItem& wi = pool.items[i];
        if (!wi.active) continue;
        if (isSentinelItem(wi.item)) continue;
        if (defs && wi.item.defId < defCount && defs[wi.item.defId].petSummon) continue;
        if (static_cast<u8>(wi.item.rarity) >= static_cast<u8>(minRarityToBeat)) continue;

        const u8 r = static_cast<u8>(wi.item.rarity);
        if (r < worstRarity || (r == worstRarity && wi.lifetime < worstLifetime)) {
            worstRarity = r; worstLifetime = wi.lifetime; victim = static_cast<s32>(i);
        }
    }
    return victim;
}
} // namespace

bool WorldItemSystem::spawn(WorldItemPool& pool, const ItemInstance& item, Vec3 position,
                              const LevelGrid* grid, u8 ownerSlot, f32 exclusiveSeconds,
                              const ItemDef* defs, u32 defCount) {
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
        // MYTHIC is rare by construction (a quarter of Inferno's legendary slice, ~1.9% of drops,
        // and nothing at all below Inferno), so logging every one is quiet — and it is the only way
        // a soak can answer "did the new tier actually pay out?". The AutoEquip line alone cannot:
        // a mythic shares its DEF, and therefore its name, with the legendary it upgraded from.
        if (item.rarity == Rarity::MYTHIC)
            LOG_INFO("[MYTHIC] drop: defId=%u itemLevel=%u affixes=%u",
                     (u32)item.defId, (u32)item.itemLevel, (u32)item.affixCount);
        return true;
    }

    // POOL FULL. Losing whatever just dropped is the worst possible answer, because legendaries and
    // mythics NEVER despawn: at Inferno they accumulate on the floor until all 64 slots are taken,
    // and from then on every new drop was discarded — including the mythics — while sixty commons
    // sat there waiting out a 60 s timer. Measured: 1448 losses in a 3 h soak, entirely on deep
    // floors (a class that never got past Normal saw zero).
    //
    // So make room by dropping the cheapest thing on the floor, but ONLY if it is strictly worse
    // than what is arriving. That keeps the exchange a strict upgrade, exactly as the backpack's
    // autoEvictWorst does — and for the same reason: an exchange that is not an upgrade can churn.
    // Nothing is re-dropped here (the victim leaves the world outright), so there is no pickup loop
    // to worry about, only the value ordering.
    const s32 victim = findEvictable(pool, item.rarity, defs, defCount);
    if (victim >= 0) {
        pool.items[victim].active = false;
        if (pool.activeCount > 0) pool.activeCount--;
        // Recurse ONCE into the now-guaranteed free slot. Safe: the slot is free, so the loop above
        // returns before reaching this branch again.
        return spawn(pool, item, position, grid, ownerSlot, exclusiveSeconds, defs, defCount);
    }

    // Nothing on the floor is worse than the incoming item, so declining it IS the correct answer —
    // it is genuinely the least valuable thing in play. Logged with the rarity so a full pool can be
    // told apart from a pool full of junk, and throttled: the old unconditional line produced 1448
    // entries in one soak, which is noise rather than signal.
    static u32 s_declined = 0;
    if ((s_declined++ % 64) == 0)
        LOG_WARN("WorldItemSystem: pool full of equal-or-better loot; declined a %s drop (%u so far)",
                 rarityName(item.rarity), s_declined);
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

    // Evict the cheapest expendable drop. Shares findEvictable with spawn() so there is ONE answer
    // to "what is the most expendable thing here"; LEGENDARY as the bar reproduces this function's
    // original rule (never evict a legendary or better for a key) while also preferring a common
    // over a rare, which the old lifetime-only scan did not do.
    const s32 victim = findEvictable(pool, Rarity::LEGENDARY, defs, defCount);
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
