// engine_persist.cpp — Save/load system for DungeonEngine.
// Supports 20 independent save slots (save_01.dat … save_20.dat).
// Each slot stores a compact header (readable without a full load) followed
// by per-player data for up to 2 local (split-screen) players.
// Version is reset to 1; all prior saves (version ≤ 6) are incompatible.

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "engine/engine.h"
#include "platform/window.h"
#include "platform/clock.h"
#include "platform/input.h"
#include "renderer/gl_context.h"
#include "renderer/renderer.h"
#include "renderer/debug_draw.h"
#include "renderer/hud.h"
#include "renderer/minimap.h"
#include "renderer/font.h"
#include "renderer/item_icons.h"
#include "renderer/material.h"
#include "renderer/obj_loader.h"
#include "world/level_gen.h"
#include "world/level_mesh.h"
#include "world/level_loader.h"
#include "world/collision.h"
#include "world/combat_query.h"
#include "game/player.h"
#include "game/combat.h"
#include "game/enemy_ai.h"
#include "game/squad.h"
#include "game/limb_system.h"
#include "game/projectile.h"
#include "game/item.h"
#include "game/skill.h"
#include "game/inventory_ui.h"
#include "game/game_constants.h"
#include "net/net.h"
#include "net/server.h"
#include "net/client.h"
#include "net/snapshot.h"
#include "net/packet.h"
#include "core/log.h"
#include "core/math.h"
#include "core/frame_allocator.h"
#include "core/allocation_tracker.h"
#include "core/profiler.h"

#include <glad/glad.h>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <ctime>

// Shared statics defined in engine.cpp
extern Engine* s_engine;
extern FrameAllocator s_frameAllocator;
extern bool s_firstKillDropGiven;
extern u16  s_sourceShards;   // secret superboss key — session-only set of collected shards
extern bool s_engineSlain;    // secret superboss — Engine defeated this session (victory variant)

// Version 3 = GLOVES equipment slot (PlayerInventory.equipped grows 6→7) + the
//             bonusAttackSpeedPct cache field. Header layout is UNCHANGED.
// Version 2 = adds m_difficulty byte to header. Still READABLE (see LegacyPlayerInventoryV2
//             below) — loaded v2 characters migrate to v3 on their next save.
// Version 1 saves are incompatible.
static constexpr u32 SAVE_VERSION           = 3;
static constexpr u32 SAVE_VERSION_LEGACY_V2 = 2;

// True for any version this build can read (the current one or a supported legacy one).
static bool saveVersionReadable(u32 ver) {
    return ver == SAVE_VERSION || ver == SAVE_VERSION_LEGACY_V2;
}

// --- Legacy v2 on-disk mirror of PlayerInventory -----------------------------------------
// v2 saves dumped the then-current PlayerInventory as one raw blob: 6 equipped slots (no
// GLOVES) and 14 bonus f32s (no bonusAttackSpeedPct). This mirror reproduces that exact
// layout (same member types ⇒ same compiler padding) so a v2 blob can be fread in one piece
// and mapped into the v3 struct. ItemInstance itself is unchanged between v2 and v3.
struct LegacyPlayerInventoryV2 {
    ItemInstance equipped[6] = {};                     // v2 slot order == v3 (GLOVES appended)
    ItemInstance backpack[MAX_INVENTORY_ITEMS] = {};
    u8           backpackCount = 0;
    f32 bonusDamageFlat, bonusDamagePct, bonusHealthFlat, bonusHealthPct, bonusMoveSpeed,
        bonusCooldownReduction, bonusLifeOnHit, bonusProjectileSpeedPct, bonusConeAngle,
        bonusRange, bonusDamageToFlying, bonusClipSizePct, bonusReloadSpeedPct, bonusEnergyFlat;
};

// Size guards: if any of these fire, the on-disk layout drifted — bump SAVE_VERSION, add a
// new Legacy*V<n> mirror for the previous layout, and extend the readers. NEVER ship a layout
// change under an unchanged version (that's what silently corrupted pre-v3 saves' class byte).
static_assert(sizeof(ItemInstance)            == 52,   "ItemInstance layout drifted — see comment above");
static_assert(sizeof(LegacyPlayerInventoryV2) == 1620, "v2 mirror must match the historical v2 blob size");
static_assert(sizeof(PlayerInventory)         == 1676, "PlayerInventory layout drifted — see comment above");
static_assert(sizeof(QuickbarState)           == 36,   "QuickbarState layout drifted — see comment above");

// Read one PlayerInventory blob at the given save version. v3 reads the struct directly;
// v2 reads the legacy mirror and maps it (GLOVES starts empty; bonus caches are rebuilt by
// applySavedCharToLane → recalculateStats, so only items/backpack need copying).
static bool readPlayerInventory(FILE* f, PlayerInventory& out, u32 ver) {
    if (ver == SAVE_VERSION)
        return std::fread(&out, sizeof(PlayerInventory), 1, f) == 1;
    LegacyPlayerInventoryV2 legacy;
    if (std::fread(&legacy, sizeof(LegacyPlayerInventoryV2), 1, f) != 1) return false;
    out = PlayerInventory{};
    for (u32 s = 0; s < 6; s++) out.equipped[s] = legacy.equipped[s];
    for (u32 b = 0; b < MAX_INVENTORY_ITEMS; b++) out.backpack[b] = legacy.backpack[b];
    out.backpackCount = legacy.backpackCount;
    return true;
}

// ---------------------------------------------------------------------------
// Path helper
// ---------------------------------------------------------------------------

const char* Engine::getSaveSlotPath(u8 slot, char* buf, u32 bufSize) {
    // Slots are 1-based externally; clamp to valid range just in case.
    if (slot < 1 || slot > MAX_SAVE_SLOTS) slot = 1;
    std::snprintf(buf, bufSize, "save_%02u.dat", static_cast<u32>(slot));
    return buf;
}

// ---------------------------------------------------------------------------
// Scan all slot headers to populate m_saveSlots[]
// ---------------------------------------------------------------------------

void Engine::scanSaveSlots() {
    char path[32];
    for (u32 i = 0; i < MAX_SAVE_SLOTS; i++) {
        SaveSlotInfo& info = m_saveSlots[i];
        info = {};  // default: does not exist

        getSaveSlotPath(static_cast<u8>(i + 1), path, sizeof(path));
        FILE* f = std::fopen(path, "rb");
        if (!f) continue;

        // Read just the header block — version + floor + playerCount + classes + timestamp + time
        u32 ver = 0;
        if (std::fread(&ver, sizeof(u32), 1, f) != 1 || !saveVersionReadable(ver)) {
            std::fclose(f);
            continue;
        }

        u8 floor = 0, playerCount = 0, classes[2] = {0, 0xFF}, difficulty = 0;
        u32 timestamp = 0;
        f32 totalTime = 0.0f;

        bool ok = true;
        ok = ok && std::fread(&floor,       sizeof(u8),  1, f) == 1;
        ok = ok && std::fread(&playerCount, sizeof(u8),  1, f) == 1;
        ok = ok && std::fread(classes,      sizeof(u8),  2, f) == 2;
        ok = ok && std::fread(&timestamp,   sizeof(u32), 1, f) == 1;
        ok = ok && std::fread(&totalTime,   sizeof(f32), 1, f) == 1;
        ok = ok && std::fread(&difficulty,  sizeof(u8),  1, f) == 1; // difficulty tier

        std::fclose(f);

        if (!ok) continue;

        info.exists           = true;
        info.floor            = floor;
        info.playerCount      = playerCount;
        info.playerClasses[0] = classes[0];
        info.playerClasses[1] = classes[1];
        info.timestamp        = timestamp;
        info.totalPlayTime    = totalTime;
        info.difficulty       = difficulty;  // R13 — paired with floor for the effective-floor compare in saveGame.
    }
}

// ---------------------------------------------------------------------------
// Save
// ---------------------------------------------------------------------------

// Lowest 1-based empty slot, or 0 if every slot is occupied. Used to migrate the
// Player-2 half of a legacy 2-player bundle onto its own per-character file.
u8 Engine::firstFreeSaveSlot() {
    for (u32 i = 0; i < MAX_SAVE_SLOTS; i++)
        if (!m_saveSlots[i].exists) return static_cast<u8>(i + 1);
    return 0;
}

// Write ONE local lane as a single-character (playerCount=1) save file. Each character now
// owns its own slot — a split-screen session calls this once per lane (see saveAllCharacters),
// so heroes are never bundled together and a Continue always loads exactly one of them.
//
// The header floor is the shared/P1 world floor (m_level.savedFloor). A *general* no-downgrade
// guard (generalized from the old R13 CLIENT-only guard) keeps a slot's higher effective floor
// (floor + difficulty*50): this protects a high-floor hero dropped into Player 1's lower world,
// and a joined CLIENT inside the host's level, while normal descent / difficulty-loop saves
// (which only raise the effective floor) pass through unchanged. The per-record byte layout is
// the version-3 layout (v2 files remain readable via LegacyPlayerInventoryV2 and migrate to
// v3 on their next save).
void Engine::saveCharacter(u8 lane, u8 slot) {
    if (slot < 1 || slot > MAX_SAVE_SLOTS || lane >= MAX_LOCAL_PLAYERS) return;

    char path[32];
    getSaveSlotPath(slot, path, sizeof(path));
    FILE* f = std::fopen(path, "wb");
    if (!f) { LOG_WARN("saveCharacter: failed to open %s for writing", path); return; }

    // --- Header (must match scanSaveSlots / loadGame readers) ---
    u32 ver = SAVE_VERSION;
    std::fwrite(&ver, sizeof(u32), 1, f);

    u8 floor      = static_cast<u8>(m_level.savedFloor);
    u8 difficulty = m_difficulty;

    // No-downgrade guard: never write a lower effective floor over a higher one already
    // in this slot (formula matches the skill-unlock gate at engine_update_skills.cpp:131-132).
    // ONLY applies to a character LOADED from a save (Continue / network join) — it protects a
    // high-floor hero dropped into Player 1's lower world. A fresh New Game character
    // (m_laneLoadedFromSave[lane] == false) intentionally overwrites its slot, so it must write its
    // real (low) floor and reset the slot — otherwise selecting New Game over an old high-floor save
    // appears to do nothing (the slot keeps the old floor). Continue is unaffected (its floor matches).
    if (m_laneLoadedFromSave[lane]) {
        const SaveSlotInfo& existing = m_saveSlots[slot - 1];
        if (existing.exists) {
            u32 oldEff = static_cast<u32>(existing.floor) + static_cast<u32>(existing.difficulty) * 50;
            u32 newEff = static_cast<u32>(floor)          + static_cast<u32>(difficulty)          * 50;
            if (oldEff > newEff) { floor = existing.floor; difficulty = existing.difficulty; }
        }
    }

    u8  playerCount = 1;                                              // one character per file
    u8  classes[2]  = { static_cast<u8>(m_playerClasses[lane]), 0xFF }; // slot 1 unused (single char)
    u32 timestamp   = static_cast<u32>(std::time(nullptr));
    f32 totalTime   = m_transition.totalPlayTime;

    std::fwrite(&floor,         sizeof(u8),  1, f);
    std::fwrite(&playerCount,   sizeof(u8),  1, f);
    std::fwrite(classes,        sizeof(u8),  2, f);
    std::fwrite(&timestamp,     sizeof(u32), 1, f);
    std::fwrite(&totalTime,     sizeof(f32), 1, f);
    std::fwrite(&difficulty,    sizeof(u8),  1, f);
    std::fwrite(&m_level.savedSeed, sizeof(u32), 1, f);

    // --- The single player block (from the per-lane arrays, not the swapped-in alias) ---
    // R17: SkillState's persisted form is only its four legacy fields (16 B, not 20) so the byte
    // layout matches pre-R17 saves; lastActivationTick is per-session and isn't written.
    auto writeSkillStateLegacy = [&](const SkillState& ss) {
        std::fwrite(&ss.activeSkill,   sizeof(SkillId), 1, f);
        std::fwrite(&ss.cooldownTimer, sizeof(f32),     1, f);
        std::fwrite(&ss.energy,        sizeof(f32),     1, f);
        std::fwrite(&ss.maxEnergy,     sizeof(f32),     1, f);
    };

    f32 hp    = m_localPlayers[lane].health;
    f32 maxHp = m_localPlayers[lane].maxHealth;
    std::fwrite(&hp,    sizeof(f32), 1, f);
    std::fwrite(&maxHp, sizeof(f32), 1, f);
    std::fwrite(&m_inventories[lane], sizeof(PlayerInventory), 1, f);
    std::fwrite(&m_quickbars[lane],   sizeof(QuickbarState),   1, f);
    writeSkillStateLegacy(m_skillStates[lane]);
    u8 cls         = static_cast<u8>(m_playerClasses[lane]);
    u8 activeSkill = m_activeClassSkills[lane];
    std::fwrite(&cls,         sizeof(u8), 1, f);
    std::fwrite(&activeSkill, sizeof(u8), 1, f);
    for (u32 s = 0; s < 4; s++) writeSkillStateLegacy(m_classSkillStatesPerPlayer[lane][s]);

    std::fclose(f);

    // Refresh the in-memory header so the menu + the next save's guard see the new state.
    SaveSlotInfo& info    = m_saveSlots[slot - 1];
    info.exists           = true;
    info.floor            = floor;
    info.playerCount      = 1;
    info.playerClasses[0] = classes[0];
    info.playerClasses[1] = 0xFF;
    info.timestamp        = timestamp;
    info.totalPlayTime    = totalTime;
    info.difficulty       = difficulty;

    LOG_INFO("Saved character (lane %u) to slot %u (%s) — floor %u, time %.0fs",
             lane, slot, path, floor, totalTime);
}

// Legacy single-character entry — persist Player 1 to `slot` and keep the active-slot aliases
// in sync. Retained so existing single-character callers don't need to change.
void Engine::saveGame(u8 slot) {
    m_activeSaveSlot    = slot;
    m_playerSaveSlot[0] = slot;
    saveCharacter(0, slot);
}

// Persist every active local lane to its OWN slot. A lane with no assigned slot (0) is skipped —
// e.g. the P2 half of a legacy bundle when all 20 slots were full at load time.
void Engine::saveAllCharacters() {
    for (u8 lane = 0; lane < m_splitPlayerCount && lane < MAX_LOCAL_PLAYERS; lane++) {
        u8 slot = m_playerSaveSlot[lane];
        if (slot >= 1 && slot <= MAX_SAVE_SLOTS) saveCharacter(lane, slot);
    }
}

// ---------------------------------------------------------------------------
// Load
// ---------------------------------------------------------------------------

// Read one persisted SkillState. R17: its persisted form is only the four legacy fields (16 B, not
// the in-memory 20); the per-session lastActivationTick is reset to 0 (= "never activated, gate
// clear"). Returns false on a short read. Shared by both loaders below.
// wideSkillId: early-v2 files were written while SkillId was still 4 bytes (it was later
// narrowed to u8 WITHOUT a version bump — the drift that misaligned the class byte). The two
// v2 flavors are told apart by per-player block size, derived from the file size (see
// v2HasWideSkillId below). SkillId values are tiny, so the u32→u8 cast is lossless.
static bool readSkillLegacy(FILE* f, SkillState& ss, bool wideSkillId) {
    bool r = true;
    if (wideSkillId) {
        u32 wide = 0;
        r = r && std::fread(&wide, sizeof(u32), 1, f) == 1;
        ss.activeSkill = static_cast<SkillId>(static_cast<u8>(wide));
    } else {
        r = r && std::fread(&ss.activeSkill, sizeof(SkillId), 1, f) == 1;
    }
    r = r && std::fread(&ss.cooldownTimer, sizeof(f32),     1, f) == 1;
    r = r && std::fread(&ss.energy,        sizeof(f32),     1, f) == 1;
    r = r && std::fread(&ss.maxEnergy,     sizeof(f32),     1, f) == 1;
    ss.lastActivationTick = 0;
    return r;
}

// Detect the early-v2 "wide SkillId" flavor from the bytes remaining after the header: each
// per-player block is hp(4)+maxHp(4)+inv(1620)+quickbar(36)+5 skill blocks+cls(1)+skill(1),
// where a skill block is 16 B (wide) or 13 B (narrow). Verified against real saves:
// wide = 1746 B/player (file 1767 for 1 player), narrow = 1731 B/player (file 1752).
static bool v2HasWideSkillId(FILE* f, u8 playerCount) {
    if (playerCount < 1) return false;
    long pos = std::ftell(f);                 // just past the header
    std::fseek(f, 0, SEEK_END);
    long fileSize = std::ftell(f);
    std::fseek(f, pos, SEEK_SET);
    const long blockWide = 8 + static_cast<long>(sizeof(LegacyPlayerInventoryV2))
                             + static_cast<long>(sizeof(QuickbarState)) + 5 * 16 + 2;
    return (fileSize - pos) == static_cast<long>(playerCount) * blockWide;
}

// Apply a deserialized character block to a local lane. World state (floor/seed/difficulty) is NOT
// touched here — only the per-lane character. Shared by loadGame (P1 + legacy 2-player files) and
// loadCharacterIntoLane (the Player-2 seat). Includes the old-save affix migration + stat rebuild.
void Engine::applySavedCharToLane(u8 lane, const SavedChar& ps) {
    if (lane >= MAX_LOCAL_PLAYERS) return;

    m_localPlayers[lane].health    = ps.hp;
    m_localPlayers[lane].maxHealth = ps.maxHp;

    m_inventories[lane] = ps.inv;
    // Reroll deprecated affixes from old saves (e.g. removed RANGE_BONUS)
    for (u32 s = 0; s < static_cast<u32>(ItemSlot::COUNT); s++) {
        ItemInstance& item = m_inventories[lane].equipped[s];
        for (u8 a = 0; a < item.affixCount; a++)
            if (item.affixes[a].type == AffixType::_REMOVED_RANGE_BONUS)
                item.affixes[a].type = AffixType::DAMAGE_FLAT;
    }
    for (u32 b = 0; b < m_inventories[lane].backpackCount; b++) {
        ItemInstance& item = m_inventories[lane].backpack[b];
        for (u8 a = 0; a < item.affixCount; a++)
            if (item.affixes[a].type == AffixType::_REMOVED_RANGE_BONUS)
                item.affixes[a].type = AffixType::DAMAGE_FLAT;
    }
    Inventory::recalculateStats(m_inventories[lane]);  // rebuild bonus cache from affixes

    m_quickbars[lane]   = ps.qb;
    m_skillStates[lane] = ps.skill;

    PlayerClass cls = (static_cast<u32>(ps.cls) < static_cast<u32>(PlayerClass::CLASS_COUNT))
                      ? static_cast<PlayerClass>(ps.cls)
                      : PlayerClass::WARRIOR;

    m_playerClasses[lane]     = cls;
    m_activeClassSkills[lane]  = ps.activeSkill;
    std::memcpy(m_classSkillStatesPerPlayer[lane], ps.classSkills, sizeof(ps.classSkills));

    // Re-apply class base stats so speed/reduction/energy are correct
    if (static_cast<u32>(cls) < static_cast<u32>(PlayerClass::CLASS_COUNT)) {
        const ClassDef& cdef = kClassDefs[static_cast<u32>(cls)];
        m_localPlayers[lane].moveSpeed       = cdef.baseMoveSpeed;
        m_localPlayers[lane].damageReduction = (cls == PlayerClass::WARRIOR) ? 0.3f : 0.0f;
        m_skillStates[lane].maxEnergy        = cdef.baseEnergy + m_inventories[lane].bonusEnergyFlat;
        if (m_skillStates[lane].energy > m_skillStates[lane].maxEnergy)
            m_skillStates[lane].energy = m_skillStates[lane].maxEnergy; // clamp; preserve saved energy
        // Re-wire class skill IDs (the active skill field is positional, not stored per-slot)
        for (u32 s = 0; s < 4; s++)
            m_classSkillStatesPerPlayer[lane][s].activeSkill = cdef.skills[s];
    }
}

// World-adopting load (the Player-1 / authoritative path): restores the world (floor/seed/
// difficulty) from the file and loads its character(s) into lane 0 (+lane 1 for a legacy
// playerCount=2 bundle). Sets m_playerSaveSlot[0]; for a legacy bundle it also hands P2 its own
// free slot so the next save migrates it to a per-character file.
bool Engine::loadGame(u8 slot) {
    char path[32];
    getSaveSlotPath(slot, path, sizeof(path));

    FILE* f = std::fopen(path, "rb");
    if (!f) return false;

    u32 ver = 0;
    if (std::fread(&ver, sizeof(u32), 1, f) != 1 || !saveVersionReadable(ver)) {
        LOG_WARN("loadGame slot %u: version mismatch (got %u, expected %u or %u) — ignoring",
                 slot, ver, SAVE_VERSION, SAVE_VERSION_LEGACY_V2);
        std::fclose(f);
        return false;
    }

    u8  floor = 0, playerCount = 0, classes[2] = {}, difficulty = 0;
    u32 timestamp = 0; f32 totalTime = 0.0f; u32 seed = 0;
    bool ok = true;
    ok = ok && std::fread(&floor,       sizeof(u8),  1, f) == 1;
    ok = ok && std::fread(&playerCount, sizeof(u8),  1, f) == 1;
    ok = ok && std::fread(classes,      sizeof(u8),  2, f) == 2;
    ok = ok && std::fread(&timestamp,   sizeof(u32), 1, f) == 1;
    ok = ok && std::fread(&totalTime,   sizeof(f32), 1, f) == 1;
    ok = ok && std::fread(&difficulty,  sizeof(u8),  1, f) == 1;
    ok = ok && std::fread(&seed,        sizeof(u32), 1, f) == 1;
    if (!ok) { std::fclose(f); return false; }

    // Clamp to sane bounds — don't trust file contents blindly
    if (playerCount < 1 || playerCount > MAX_LOCAL_PLAYERS) playerCount = 1;

    // Early-v2 files serialized SkillId as 4 bytes — detect via per-player block size.
    bool wideSkill = (ver == SAVE_VERSION_LEGACY_V2) && v2HasWideSkillId(f, playerCount);

    SavedChar pdata[MAX_LOCAL_PLAYERS] = {};
    for (u8 p = 0; p < playerCount; p++) {
        SavedChar& ps = pdata[p];
        bool pok = true;
        pok = pok && std::fread(&ps.hp,    sizeof(f32),             1, f) == 1;
        pok = pok && std::fread(&ps.maxHp, sizeof(f32),             1, f) == 1;
        pok = pok && readPlayerInventory(f, ps.inv, ver); // version-aware (v2 blob is smaller)
        pok = pok && std::fread(&ps.qb,    sizeof(QuickbarState),   1, f) == 1;
        pok = pok && readSkillLegacy(f, ps.skill, wideSkill);
        pok = pok && std::fread(&ps.cls,         sizeof(u8),        1, f) == 1;
        pok = pok && std::fread(&ps.activeSkill, sizeof(u8),        1, f) == 1;
        for (u32 s = 0; s < 4; s++) pok = pok && readSkillLegacy(f, ps.classSkills[s], wideSkill);
        if (!pok) { ok = false; break; }
    }
    std::fclose(f);
    if (!ok) return false;

    // Legacy 2-player bundle: re-activate split-screen so both heroes restore (back-compat).
    if (playerCount == 2 && m_splitPlayerCount < 2) {
        m_splitPlayerCount = 2;
        Input::setSplitScreen(true);
    }
    u8 restoreCount = (m_splitPlayerCount < playerCount) ? m_splitPlayerCount : playerCount;

    // --- Adopt the world from the file, then apply the character(s) ---
    m_level.savedFloor = floor;
    m_level.savedSeed  = seed;
    m_level.levelSeed  = seed; // restore the run seed so startGame regenerates the saved dungeon
    m_transition.totalPlayTime = totalTime;
    m_difficulty = (difficulty <= 2) ? difficulty : 0;

    // Secret superboss key is session-only and never serialized — a loaded run starts with no
    // shards collected (in-fiction: the curse resets you). See ~/.claude/plans (the-dungeon-engine).
    s_sourceShards = 0;
    s_engineSlain  = false;
    m_level.inSourceChamber    = false;
    m_level.sourcePortalActive = false;

    for (u8 p = 0; p < restoreCount; p++) {
        // The per-player `cls` byte sits AFTER the raw inventory/quickbar/skill blobs, whose on-disk
        // size has drifted across builds within SAVE_VERSION 2 (ItemInstance gained/lost a field; the
        // skill block shrank 20->16 B in R17), so on an OLD save it decodes as garbage and clamps to
        // WARRIOR. The header `classes[]` is read BEFORE any blob and is intact (the select screen
        // reads it correctly), so it is authoritative. New saves write the same value to both, so this
        // is a no-op for them; an invalid header (e.g. the 0xFF P2 sentinel) falls back to ps.cls.
        if (classes[p] < static_cast<u8>(PlayerClass::CLASS_COUNT)) pdata[p].cls = classes[p];
        applySavedCharToLane(p, pdata[p]);
    }

    // Sync active aliases for player 0 (swapInPlayer would overwrite these otherwise)
    m_localPlayer           = m_localPlayers[0];
    m_playerClass           = m_playerClasses[0];
    m_activeClassSkill      = m_activeClassSkills[0];
    std::memcpy(m_classSkillStates, m_classSkillStatesPerPlayer[0], sizeof(m_classSkillStates));

    m_activeSaveSlot    = slot;
    m_playerSaveSlot[0] = slot;
    m_laneLoadedFromSave[0] = true; // loaded character — keep the no-downgrade guard for it
    // Legacy 2-player bundle: give P2 its own free slot so the next save migrates it to a
    // per-character file. If all 20 are taken, P2 simply won't persist this session (logged).
    if (playerCount == 2) {
        m_laneLoadedFromSave[1] = true;
        u8 freeSlot = firstFreeSaveSlot();
        m_playerSaveSlot[1] = freeSlot;
        if (freeSlot == 0)
            LOG_WARN("loadGame: no free slot to migrate bundled Player 2 — P2 won't persist");
    }

    LOG_INFO("Game loaded from slot %u — floor %u, %u player(s) restored (save had %u)",
             slot, floor, restoreCount, playerCount);
    return true;
}

// Load the FIRST character from `slot` into local `lane` WITHOUT disturbing the world (floor/seed/
// difficulty stay Player 1's). Used to seat a Continue'd hero in the Player-2 lane for couch co-op.
bool Engine::loadCharacterIntoLane(u8 slot, u8 lane) {
    if (lane >= MAX_LOCAL_PLAYERS) return false;

    char path[32];
    getSaveSlotPath(slot, path, sizeof(path));
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;

    u32 ver = 0;
    if (std::fread(&ver, sizeof(u32), 1, f) != 1 || !saveVersionReadable(ver)) { std::fclose(f); return false; }

    // Skip the rest of the header — the world stays P1's, so floor/seed/difficulty are ignored.
    u8  floor = 0, playerCount = 0, classes[2] = {}, difficulty = 0;
    u32 timestamp = 0; f32 totalTime = 0.0f; u32 seed = 0;
    bool ok = true;
    ok = ok && std::fread(&floor,       sizeof(u8),  1, f) == 1;
    ok = ok && std::fread(&playerCount, sizeof(u8),  1, f) == 1;
    ok = ok && std::fread(classes,      sizeof(u8),  2, f) == 2;
    ok = ok && std::fread(&timestamp,   sizeof(u32), 1, f) == 1;
    ok = ok && std::fread(&totalTime,   sizeof(f32), 1, f) == 1;
    ok = ok && std::fread(&difficulty,  sizeof(u8),  1, f) == 1;
    ok = ok && std::fread(&seed,        sizeof(u32), 1, f) == 1;
    (void)floor; (void)playerCount; (void)difficulty;
    (void)timestamp; (void)totalTime; (void)seed;

    // Early-v2 files serialized SkillId as 4 bytes — detect via per-player block size.
    bool wideSkill = (ver == SAVE_VERSION_LEGACY_V2) &&
                     v2HasWideSkillId(f, (playerCount >= 1) ? playerCount : 1);

    SavedChar ps{};
    ok = ok && std::fread(&ps.hp,    sizeof(f32),             1, f) == 1;
    ok = ok && std::fread(&ps.maxHp, sizeof(f32),             1, f) == 1;
    ok = ok && readPlayerInventory(f, ps.inv, ver); // version-aware (v2 blob is smaller)
    ok = ok && std::fread(&ps.qb,    sizeof(QuickbarState),   1, f) == 1;
    ok = ok && readSkillLegacy(f, ps.skill, wideSkill);
    ok = ok && std::fread(&ps.cls,         sizeof(u8),        1, f) == 1;
    ok = ok && std::fread(&ps.activeSkill, sizeof(u8),        1, f) == 1;
    for (u32 s = 0; s < 4; s++) ok = ok && readSkillLegacy(f, ps.classSkills[s], wideSkill);
    std::fclose(f);
    if (!ok) return false;

    // Source the class from the intact header (see loadGame): the per-player `cls` byte can be
    // misaligned on old saves whose blob layout drifted under a frozen SAVE_VERSION. Per-character
    // files store the class in classes[0]; an invalid value falls back to the read ps.cls.
    if (classes[0] < static_cast<u8>(PlayerClass::CLASS_COUNT)) ps.cls = classes[0];
    applySavedCharToLane(lane, ps);
    if (lane == 0) {
        m_localPlayer      = m_localPlayers[0];
        m_playerClass      = m_playerClasses[0];
        m_activeClassSkill = m_activeClassSkills[0];
        std::memcpy(m_classSkillStates, m_classSkillStatesPerPlayer[0], sizeof(m_classSkillStates));
    }
    m_playerSaveSlot[lane] = slot;
    m_laneLoadedFromSave[lane] = true; // loaded character — keep the no-downgrade guard for it
    LOG_INFO("Loaded character from slot %u into lane %u (world unchanged)", slot, lane);
    return true;
}
