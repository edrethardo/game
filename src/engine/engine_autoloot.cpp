// engine_autoloot.cpp — the runtime half of Auto Loot & Equip (spec:
// docs/superpowers/specs/2026-07-22-auto-loot-equip-design.md; scorer: game/build_score.h).
//
// When a lane's PlayerInventory.autoMode is on, the game plays the inventory FOR that lane:
// nearby loot is vacuumed into the bag (through the SAME paths a manual grab uses — the client
// side rides the server-validated CL_PICKUP_ITEM, so nothing new touches the wire), upgrades
// under the selected 3x3 build cell are worn on the spot, and a full bag evicts its lowest-
// scoring item to make room (Aaron chose the self-managing bag over "pause when full").
//
// Everything here runs inside the per-player swap window (m_localPlayer / m_localPlayerIndex are
// the current lane), called from updatePlayerPickup. Classic lanes (autoMode 0) pay one branch.

#include "engine/engine.h"
#include "game/build_score.h"
#include "game/item.h"
#include "audio/audio.h"
#include "platform/steam.h"
#include "core/log.h"

#include <cmath>

// One auto-pickup per lane per tick. The radius reuses the interact reach so "close enough to
// grab" means the same thing in both modes, and the vertical bound stops the vacuum pulling loot
// through a floor — the exact story-blind bug the manual path had and fixed.
static constexpr f32 AUTO_LOOT_RADIUS = 2.5f;

// --- worn-slot lookup ---------------------------------------------------------------------------
// Score the item currently worn in `slot` under `cell` (0 when empty — an empty slot is always an
// upgrade target).
static f32 wornScore(const PlayerInventory& inv, ItemSlot slot, const ItemDef* defs, u32 defCount,
                     u8 cell) {
    const ItemInstance& worn = inv.equipped[static_cast<u32>(slot)];
    if (worn.defId == 0xFFFF || worn.defId >= defCount) return 0.0f;
    return BuildScore::score(worn, defs[worn.defId], cell);
}

// --- auto-equip ---------------------------------------------------------------------------------
// Try to wear backpack[bpIdx] if the build scores it above the worn piece (with hysteresis).
// Returns true if it equipped. The sendInventorySync after a successful equip is NOT optional —
// the server fires a client's weapon from its own copy of that client's inventory, and the one
// equip path that ever skipped the sync was a real shipped bug (the quickbar use-path).
bool Engine::autoEquipIfUpgrade(u8 lane, u8 bpIdx) {
    PlayerInventory& inv = m_inventories[lane];
    if (bpIdx >= MAX_INVENTORY_ITEMS) return false;
    const ItemInstance& cand = inv.backpack[bpIdx];
    if (cand.defId == 0xFFFF || cand.defId >= m_itemDefCount) return false;
    const ItemDef& def = m_itemDefs[cand.defId];
    if (def.petSummon) return false;                       // consumables are used, never worn

    const f32 candScore = BuildScore::score(cand, def, inv.buildCell);
    if (candScore <= 0.0f) return false;                   // wrong weapon family, or worthless
    const f32 worn = wornScore(inv, def.slot, m_itemDefs, m_itemDefCount, inv.buildCell);
    if (!BuildScore::isUpgrade(candScore, worn)) return false;

    Inventory::equip(inv, bpIdx, m_itemDefs);
    sendInventorySync(lane, activeNetSlot());              // no-op unless CLIENT
    // The game just dressed the player — say so. Silent gear changes read as items vanishing.
    addChatMessage("", def.name, Vec3{0.5f, 0.9f, 0.5f});
    LOG_INFO("AutoEquip[%u]: %s", lane, def.name);
    return true;
}

// Re-gear the whole lane under its current build — the "switch build in the grid" action. Walks
// every backpack slot repeatedly until nothing upgrades (an equip swaps the worn piece back into
// the bag, which can itself now be the best candidate for another slot).
void Engine::autoEquipBackpack(u8 lane) {
    if (lane >= MAX_LOCAL_PLAYERS) return;
    if (!m_inventories[lane].autoMode) return;
    bool changed = true;
    u32 guard = 0;
    while (changed && guard++ < 64) {                      // 64 >> slots; loops only on real swaps
        changed = false;
        for (u8 bi = 0; bi < MAX_INVENTORY_ITEMS; bi++)
            if (autoEquipIfUpgrade(lane, bi)) changed = true;
    }
}

// --- bag eviction -------------------------------------------------------------------------------
// Drop the lowest-scoring backpack item to make room (never pets, never quickbar-assigned gear —
// the same exemptions "drop all" honours). Returns true when a slot was freed.
bool Engine::autoEvictWorst(u8 lane) {
    PlayerInventory& inv = m_inventories[lane];
    s32 worst = -1;
    f32 worstScore = 1e30f;
    for (u8 bi = 0; bi < MAX_INVENTORY_ITEMS; bi++) {
        const ItemInstance& it = inv.backpack[bi];
        if (it.defId == 0xFFFF || it.defId >= m_itemDefCount) continue;
        if (m_itemDefs[it.defId].petSummon) continue;
        if (Quickbar::holdsBackpackItem(m_quickbars[lane], it.uid)) continue;
        const f32 s = BuildScore::score(it, m_itemDefs[it.defId], inv.buildCell);
        if (s < worstScore) { worstScore = s; worst = bi; }
    }
    if (worst < 0) return false;                           // nothing evictable — bag stays full

    ItemInstance dropped = Inventory::dropFromBackpack(inv, static_cast<u8>(worst));
    if (isItemEmpty(dropped)) return false;
    const Vec3 dropPos = m_localPlayer.position + m_localPlayer.forward * 1.2f + Vec3{0, 0.5f, 0};
    WorldItemSystem::spawn(m_worldItems, dropped, dropPos, &m_level.grid);
    if (m_netRole == NetRole::CLIENT)
        sendDropRequest(0, static_cast<u8>(worst), dropped, dropPos);   // R11: server mirrors the drop
    // Name what the bag threw away — the game deciding to discard gear must never be silent.
    if (dropped.defId < m_itemDefCount)
        addChatMessage("", m_itemDefs[dropped.defId].name, Vec3{0.8f, 0.7f, 0.4f});
    return true;
}

// --- the per-tick vacuum ------------------------------------------------------------------------
void Engine::updateAutoLoot(f32 /*dt*/) {
    const u8 lane = m_localPlayerIndex;
    PlayerInventory& inv = m_inventories[lane];
    if (!inv.autoMode) return;
    if (m_level.inArena) return;                           // no loot exists in the arena; stay inert

    // Find the nearest eligible drop in reach. Sentinels (globes/shrines/chests/stash/shards) keep
    // their own flows — this vacuums LOOT only.
    s32 best = -1;
    f32 bestD2 = AUTO_LOOT_RADIUS * AUTO_LOOT_RADIUS;
    for (u32 i = 0; i < MAX_WORLD_ITEMS; i++) {
        const WorldItem& wi = m_worldItems.items[i];
        if (!wi.active) continue;
        if (isSentinelItem(wi.item)) continue;
        if (wi.item.defId >= m_itemDefCount) continue;
        // Ownership: free-for-all, ours, or the exclusive window expired.
        if (wi.ownerSlot != 0xFF && wi.ownerSlot != activeNetSlot() && wi.exclusiveTimer > 0.0f)
            continue;
        const Vec3 d = wi.position - m_localPlayer.position;
        const f32 d2 = d.x * d.x + d.z * d.z;
        if (d2 >= bestD2) continue;
        if (std::fabs(d.y) > Interact::INTERACT_VERTICAL_REACH) continue;   // another storey's loot
        bestD2 = d2; best = static_cast<s32>(i);
    }
    if (best < 0) return;

    // CLIENT: request it through the server-validated path, exactly like a manual grab (the
    // request predicts the pickup and rolls back on reject). One request per tick keeps a burst of
    // drops from flooding the wire; the next tick grabs the next item.
    if (m_netRole == NetRole::CLIENT) {
        if (inv.backpackCount >= MAX_INVENTORY_ITEMS && !autoEvictWorst(lane)) return;
        sendPickupRequest(best);
        return;
    }

    // SP/HOST: full bag self-manages first (Aaron's call: evict the worst, never pause).
    if (inv.backpackCount >= MAX_INVENTORY_ITEMS && !autoEvictWorst(lane)) return;

    WorldItem& wi = m_worldItems.items[best];
    ItemInstance picked = wi.item;
    wi.active = false;
    if (m_worldItems.activeCount > 0) m_worldItems.activeCount--;

    const s8 bpSlot = Inventory::addToBackpack(inv, picked);
    if (bpSlot < 0) return;                                 // race with eviction — should not happen
    AudioSystem::play(SfxId::ITEM_PICKUP);
    Steam::unlockAchievement("ACH_FIRST_ITEM");             // same as a manual world pickup

    // The equip half: wear it on the spot if the build says it is an upgrade.
    autoEquipIfUpgrade(lane, static_cast<u8>(bpSlot));
}
