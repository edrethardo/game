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

// Version 2 = adds m_difficulty byte to header. Version 1 saves are incompatible.
static constexpr u32 SAVE_VERSION = 2;

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
        if (std::fread(&ver, sizeof(u32), 1, f) != 1 || ver != SAVE_VERSION) {
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

void Engine::saveGame(u8 slot) {
    char path[32];
    getSaveSlotPath(slot, path, sizeof(path));

    FILE* f = std::fopen(path, "wb");
    if (!f) { LOG_WARN("saveGame: failed to open %s for writing", path); return; }

    // --- Header (must match scanSaveSlots reader) ---
    u32 ver = SAVE_VERSION;
    std::fwrite(&ver, sizeof(u32), 1, f);

    u8 floor        = static_cast<u8>(m_level.savedFloor);
    u8 difficulty   = m_difficulty;

    // R13 — joined CLIENT: never downgrade on-disk floor + difficulty. The run is
    // happening inside the host's level, not the client's own. A floor-descend
    // auto-save (engine_update.cpp:1454) or difficulty-loop reset (line 1490)
    // otherwise writes the host's lower effective floor over a higher-progress SP
    // save. Preserve the larger effective floor (formula matches the runtime
    // skill-unlock gate at engine_update_skills.cpp:131-132) along with the
    // matching difficulty so the pair stays consistent. Inventory and per-player
    // state below still write from the live MP run — loot collected during the
    // session lands in the save as expected.
    if (m_netRole == NetRole::CLIENT && slot >= 1 && slot <= MAX_SAVE_SLOTS) {
        const SaveSlotInfo& existing = m_saveSlots[slot - 1];
        if (existing.exists) {
            u32 oldEff = static_cast<u32>(existing.floor) + static_cast<u32>(existing.difficulty) * 50;
            u32 newEff = static_cast<u32>(floor)          + static_cast<u32>(difficulty)          * 50;
            if (oldEff > newEff) {
                floor      = existing.floor;
                difficulty = existing.difficulty;
            }
        }
    }

    u8 playerCount  = m_splitPlayerCount;
    // Second class slot is 0xFF when only one player is saved
    u8 cls1 = static_cast<u8>(m_playerClasses[0]);
    u8 cls2 = (playerCount >= 2) ? static_cast<u8>(m_playerClasses[1]) : static_cast<u8>(0xFF);
    u8 classes[2] = { cls1, cls2 };
    u32 timestamp   = static_cast<u32>(std::time(nullptr));
    f32 totalTime   = m_transition.totalPlayTime;

    std::fwrite(&floor,         sizeof(u8),  1, f);
    std::fwrite(&playerCount,   sizeof(u8),  1, f);
    std::fwrite(classes,        sizeof(u8),  2, f);
    std::fwrite(&timestamp,     sizeof(u32), 1, f);
    std::fwrite(&totalTime,     sizeof(f32), 1, f);
    std::fwrite(&difficulty,    sizeof(u8),  1, f); // difficulty tier (R13: paired with `floor` above)

    // Level seed (after header, before per-player data)
    std::fwrite(&m_level.savedSeed, sizeof(u32), 1, f);

    // --- Per-player data ---
    // Always save from the per-player arrays, not the active aliases, so we
    // capture the correct state regardless of which player is currently swapped in.
    for (u8 p = 0; p < playerCount; p++) {
        f32 hp    = m_localPlayers[p].health;
        f32 maxHp = m_localPlayers[p].maxHealth;
        std::fwrite(&hp,    sizeof(f32), 1, f);
        std::fwrite(&maxHp, sizeof(f32), 1, f);

        std::fwrite(&m_inventories[p],  sizeof(PlayerInventory), 1, f);
        std::fwrite(&m_quickbars[p],    sizeof(QuickbarState),   1, f);
        // R17: SkillState gained lastActivationTick (u32), so `sizeof(SkillState)`
        // grew 16 → 20 bytes. To keep the save format byte-identical to pre-R17
        // saves, write only the four legacy fields (activeSkill / cooldownTimer /
        // energy / maxEnergy). lastActivationTick is intentionally NOT persisted —
        // it's in the per-session clientTick frame, so loading should start a
        // fresh session with all skills "never activated" (lastActivationTick = 0
        // = sentinel gate-clear), which is exactly what default-init gives us.
        auto writeSkillStateLegacy = [&](const SkillState& ss) {
            std::fwrite(&ss.activeSkill,   sizeof(SkillId), 1, f);
            std::fwrite(&ss.cooldownTimer, sizeof(f32),     1, f);
            std::fwrite(&ss.energy,        sizeof(f32),     1, f);
            std::fwrite(&ss.maxEnergy,     sizeof(f32),     1, f);
        };
        writeSkillStateLegacy(m_skillStates[p]);

        u8 cls        = static_cast<u8>(m_playerClasses[p]);
        u8 activeSkill = m_activeClassSkills[p];
        std::fwrite(&cls,         sizeof(u8), 1, f);
        std::fwrite(&activeSkill, sizeof(u8), 1, f);
        for (u32 s = 0; s < 4; s++) writeSkillStateLegacy(m_classSkillStatesPerPlayer[p][s]);
    }

    std::fclose(f);

    // Update in-memory slot info so the menu reflects the new state immediately
    m_activeSaveSlot = slot;
    SaveSlotInfo& info  = m_saveSlots[slot - 1];
    info.exists         = true;
    info.floor          = floor;
    info.playerCount    = playerCount;
    info.playerClasses[0] = classes[0];
    info.playerClasses[1] = classes[1];
    info.timestamp      = timestamp;
    info.totalPlayTime  = totalTime;

    LOG_INFO("Game saved to slot %u (%s) — floor %u, %u player(s), time %.0fs",
             slot, path, floor, playerCount, totalTime);
}

// ---------------------------------------------------------------------------
// Load
// ---------------------------------------------------------------------------

bool Engine::loadGame(u8 slot) {
    char path[32];
    getSaveSlotPath(slot, path, sizeof(path));

    FILE* f = std::fopen(path, "rb");
    if (!f) return false;

    // Verify version
    u32 ver = 0;
    if (std::fread(&ver, sizeof(u32), 1, f) != 1 || ver != SAVE_VERSION) {
        LOG_WARN("loadGame slot %u: version mismatch (got %u, expected %u) — ignoring",
                 slot, ver, SAVE_VERSION);
        std::fclose(f);
        return false;
    }

    // Read header
    u8  floor = 0, playerCount = 0, classes[2] = {}, difficulty = 0;
    u32 timestamp = 0;
    f32 totalTime = 0.0f;
    u32 seed = 0;

    bool ok = true;
    ok = ok && std::fread(&floor,       sizeof(u8),  1, f) == 1;
    ok = ok && std::fread(&playerCount, sizeof(u8),  1, f) == 1;
    ok = ok && std::fread(classes,      sizeof(u8),  2, f) == 2;
    ok = ok && std::fread(&timestamp,   sizeof(u32), 1, f) == 1;
    ok = ok && std::fread(&totalTime,   sizeof(f32), 1, f) == 1;
    ok = ok && std::fread(&difficulty,  sizeof(u8),  1, f) == 1; // difficulty tier
    ok = ok && std::fread(&seed,        sizeof(u32), 1, f) == 1;

    if (!ok) { std::fclose(f); return false; }

    // Clamp to sane bounds — don't trust file contents blindly
    if (playerCount < 1 || playerCount > MAX_LOCAL_PLAYERS) playerCount = 1;

    // Per-player scratch buffers
    struct PlayerSave {
        f32             hp, maxHp;
        PlayerInventory inv;
        QuickbarState   qb;
        SkillState      skill;
        u8              cls;
        u8              activeSkill;
        SkillState      classSkills[4];
    } pdata[MAX_LOCAL_PLAYERS] = {};

    // If the save has 2 players, activate split-screen so both get restored
    if (playerCount == 2 && m_splitPlayerCount < 2) {
        m_splitPlayerCount = 2;
        Input::setSplitScreen(true);
    }
    u8 restoreCount = (m_splitPlayerCount < playerCount) ? m_splitPlayerCount : playerCount;

    // R17 — match the writer in saveGame: SkillState's persisted form is only the
    // four legacy fields (activeSkill / cooldownTimer / energy / maxEnergy = 16 B).
    // The new lastActivationTick field is per-session-clientTick and is left at
    // its default-init 0 (= "never activated, gate clear") on load.
    auto readSkillStateLegacy = [&](SkillState& ss) -> bool {
        bool r = true;
        r = r && std::fread(&ss.activeSkill,   sizeof(SkillId), 1, f) == 1;
        r = r && std::fread(&ss.cooldownTimer, sizeof(f32),     1, f) == 1;
        r = r && std::fread(&ss.energy,        sizeof(f32),     1, f) == 1;
        r = r && std::fread(&ss.maxEnergy,     sizeof(f32),     1, f) == 1;
        ss.lastActivationTick = 0;
        return r;
    };

    for (u8 p = 0; p < playerCount; p++) {
        bool pok = true;
        pok = pok && std::fread(&pdata[p].hp,    sizeof(f32),            1, f) == 1;
        pok = pok && std::fread(&pdata[p].maxHp, sizeof(f32),            1, f) == 1;
        pok = pok && std::fread(&pdata[p].inv,   sizeof(PlayerInventory), 1, f) == 1;
        pok = pok && std::fread(&pdata[p].qb,    sizeof(QuickbarState),  1, f) == 1;
        pok = pok && readSkillStateLegacy(pdata[p].skill);
        pok = pok && std::fread(&pdata[p].cls,   sizeof(u8),             1, f) == 1;
        pok = pok && std::fread(&pdata[p].activeSkill, sizeof(u8),       1, f) == 1;
        for (u32 s = 0; s < 4; s++) pok = pok && readSkillStateLegacy(pdata[p].classSkills[s]);
        if (!pok) { ok = false; break; }
    }

    std::fclose(f);
    if (!ok) return false;

    // --- Apply to engine state ---
    m_level.savedFloor = floor;
    m_level.savedSeed  = seed;
    m_level.levelSeed  = seed; // restore the run seed so startGame regenerates the saved dungeon
    m_transition.totalPlayTime = totalTime;
    // Restore difficulty, clamped to valid range
    m_difficulty = (difficulty <= 2) ? difficulty : 0;

    for (u8 p = 0; p < restoreCount; p++) {
        const PlayerSave& ps = pdata[p];

        m_localPlayers[p].health    = ps.hp;
        m_localPlayers[p].maxHealth = ps.maxHp;

        m_inventories[p] = ps.inv;
        // Reroll deprecated affixes from old saves (e.g. removed RANGE_BONUS)
        for (u32 s = 0; s < static_cast<u32>(ItemSlot::COUNT); s++) {
            ItemInstance& item = m_inventories[p].equipped[s];
            for (u8 a = 0; a < item.affixCount; a++) {
                if (item.affixes[a].type == AffixType::_REMOVED_RANGE_BONUS) {
                    item.affixes[a].type = AffixType::DAMAGE_FLAT;
                }
            }
        }
        for (u32 b = 0; b < m_inventories[p].backpackCount; b++) {
            ItemInstance& item = m_inventories[p].backpack[b];
            for (u8 a = 0; a < item.affixCount; a++) {
                if (item.affixes[a].type == AffixType::_REMOVED_RANGE_BONUS) {
                    item.affixes[a].type = AffixType::DAMAGE_FLAT;
                }
            }
        }
        Inventory::recalculateStats(m_inventories[p]);  // rebuild bonus cache from affixes

        m_quickbars[p]   = ps.qb;
        m_skillStates[p] = ps.skill;

        PlayerClass cls = (static_cast<u32>(ps.cls) < static_cast<u32>(PlayerClass::CLASS_COUNT))
                          ? static_cast<PlayerClass>(ps.cls)
                          : PlayerClass::WARRIOR;

        m_playerClasses[p]    = cls;
        m_activeClassSkills[p] = ps.activeSkill;
        std::memcpy(m_classSkillStatesPerPlayer[p], ps.classSkills, sizeof(ps.classSkills));

        // Re-apply class base stats so speed/reduction/energy are correct
        if (static_cast<u32>(cls) < static_cast<u32>(PlayerClass::CLASS_COUNT)) {
            const ClassDef& cdef = kClassDefs[static_cast<u32>(cls)];
            m_localPlayers[p].moveSpeed      = cdef.baseMoveSpeed;
            m_localPlayers[p].damageReduction = (cls == PlayerClass::WARRIOR) ? 0.3f : 0.0f;
            m_skillStates[p].maxEnergy        = cdef.baseEnergy + m_inventories[p].bonusEnergyFlat;
            if (m_skillStates[p].energy > m_skillStates[p].maxEnergy)
                m_skillStates[p].energy = m_skillStates[p].maxEnergy; // clamp; preserve saved energy
            // Re-wire class skill IDs (the active skill field is positional, not stored per-slot)
            for (u32 s = 0; s < 4; s++)
                m_classSkillStatesPerPlayer[p][s].activeSkill = cdef.skills[s];
        }
    }

    // Sync active aliases for player 0 (swapInPlayer would overwrite these otherwise)
    m_localPlayer           = m_localPlayers[0];
    m_playerClass           = m_playerClasses[0];
    m_activeClassSkill      = m_activeClassSkills[0];
    std::memcpy(m_classSkillStates, m_classSkillStatesPerPlayer[0], sizeof(m_classSkillStates));

    m_activeSaveSlot = slot;

    LOG_INFO("Game loaded from slot %u — floor %u, %u player(s) restored (save had %u)",
             slot, floor, restoreCount, playerCount);
    return true;
}
