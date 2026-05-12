// Engine module — see engine.h for class definition.
// Split from engine.cpp for manageability. All methods are Engine:: members.

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

// Shared statics defined in engine.cpp
// Shared statics defined in engine.cpp
extern Engine* s_engine;
extern FrameAllocator s_frameAllocator;
extern bool s_firstKillDropGiven;


static constexpr u32 SAVE_VERSION = 6; // v6: added totalPlayTime

void Engine::saveGame() {
    FILE* f = std::fopen("save.dat", "wb");
    if (!f) { LOG_WARN("Failed to save game"); return; }

    // Version header
    u32 ver = SAVE_VERSION;
    std::fwrite(&ver, sizeof(u32), 1, f);

    // Header: floor + seed
    std::fwrite(&m_level.savedFloor, sizeof(u32), 1, f);
    std::fwrite(&m_level.savedSeed, sizeof(u32), 1, f);

    // Player health
    f32 hp = m_localPlayer.health;
    f32 maxHp = m_localPlayer.maxHealth;
    std::fwrite(&hp, sizeof(f32), 1, f);
    std::fwrite(&maxHp, sizeof(f32), 1, f);

    // Inventory (equipment + backpack)
    std::fwrite(&m_inventories[m_localPlayerIndex], sizeof(PlayerInventory), 1, f);

    // Quickbar
    std::fwrite(&m_quickbars[m_localPlayerIndex], sizeof(QuickbarState), 1, f);

    // Skill state
    std::fwrite(&m_skillStates[m_localPlayerIndex], sizeof(SkillState), 1, f);

    // Player class
    std::fwrite(&m_playerClass, sizeof(m_playerClass), 1, f);
    std::fwrite(&m_activeClassSkill, sizeof(m_activeClassSkill), 1, f);
    std::fwrite(m_classSkillStates, sizeof(m_classSkillStates), 1, f);

    // Total play time
    std::fwrite(&m_transition.totalPlayTime, sizeof(f32), 1, f);

    std::fclose(f);
    LOG_INFO("Game saved at floor %u (class %u, time %.0fs)", m_level.savedFloor, static_cast<u32>(m_playerClass), m_transition.totalPlayTime);
}

bool Engine::loadGame() {
    FILE* f = std::fopen("save.dat", "rb");
    if (!f) return false;

    // Check save version — reject incompatible saves
    u32 ver = 0;
    if (std::fread(&ver, sizeof(u32), 1, f) != 1 || ver != SAVE_VERSION) {
        LOG_WARN("Save file version mismatch (got %u, expected %u) — starting fresh", ver, SAVE_VERSION);
        std::fclose(f);
        return false;
    }

    bool ok = true;
    ok = ok && std::fread(&m_level.savedFloor, sizeof(u32), 1, f) == 1;
    ok = ok && std::fread(&m_level.savedSeed, sizeof(u32), 1, f) == 1;

    // Player health
    f32 hp = 100.0f, maxHp = 100.0f;
    ok = ok && std::fread(&hp, sizeof(f32), 1, f) == 1;
    ok = ok && std::fread(&maxHp, sizeof(f32), 1, f) == 1;

    // Inventory
    PlayerInventory loadedInv = {};
    ok = ok && std::fread(&loadedInv, sizeof(PlayerInventory), 1, f) == 1;

    // Quickbar
    QuickbarState loadedQb = {};
    ok = ok && std::fread(&loadedQb, sizeof(QuickbarState), 1, f) == 1;

    // Skill state
    SkillState loadedSkill = {};
    ok = ok && std::fread(&loadedSkill, sizeof(SkillState), 1, f) == 1;

    // Player class (may not exist in old saves — defaults to WARRIOR)
    PlayerClass loadedClass = PlayerClass::WARRIOR;
    u8 loadedActiveSkill = 0;
    SkillState loadedClassSkills[4] = {};
    if (std::fread(&loadedClass, sizeof(loadedClass), 1, f) == 1) {
        std::fread(&loadedActiveSkill, sizeof(loadedActiveSkill), 1, f);
        std::fread(loadedClassSkills, sizeof(loadedClassSkills), 1, f);
    }

    // Total play time (v6+)
    f32 loadedTotalTime = 0.0f;
    std::fread(&loadedTotalTime, sizeof(f32), 1, f); // silently fails on older saves

    std::fclose(f);

    if (ok) {
        m_transition.totalPlayTime = loadedTotalTime;
        m_localPlayer.health = hp;
        m_localPlayer.maxHealth = maxHp;
        m_inventories[m_localPlayerIndex] = loadedInv;
        Inventory::recalculateStats(m_inventories[m_localPlayerIndex]); // rebuild bonuses from affixes
        m_quickbars[m_localPlayerIndex] = loadedQb;
        m_skillStates[m_localPlayerIndex] = loadedSkill;

        // Restore player class and re-apply class-specific stats
        m_playerClass = loadedClass;
        m_activeClassSkill = loadedActiveSkill;
        for (u32 s = 0; s < 4; s++) m_classSkillStates[s] = loadedClassSkills[s];

        // Sync per-player arrays so swapInPlayer(0) doesn't overwrite with defaults
        m_playerClasses[0] = m_playerClass;
        m_activeClassSkills[0] = m_activeClassSkill;
        std::memcpy(m_classSkillStatesPerPlayer[0], m_classSkillStates, sizeof(m_classSkillStates));
        m_localPlayers[0] = m_localPlayer;

        if (static_cast<u32>(m_playerClass) < static_cast<u32>(PlayerClass::CLASS_COUNT)) {
            const ClassDef& cls = kClassDefs[static_cast<u32>(m_playerClass)];
            m_localPlayer.moveSpeed = cls.baseMoveSpeed;
            m_localPlayer.damageReduction = (m_playerClass == PlayerClass::WARRIOR) ? 0.3f : 0.0f;
            m_skillStates[m_localPlayerIndex].maxEnergy = cls.baseEnergy;
            // Re-sync class skill active IDs
            for (u32 s = 0; s < 4; s++) {
                m_classSkillStates[s].activeSkill = cls.skills[s];
            }
            // Update per-player arrays again after class stats applied
            m_playerClasses[0] = m_playerClass;
            m_localPlayers[0] = m_localPlayer;
            std::memcpy(m_classSkillStatesPerPlayer[0], m_classSkillStates, sizeof(m_classSkillStates));
        }

        LOG_INFO("Game loaded: floor %u, class %u, HP %.0f/%.0f",
                 m_level.savedFloor, static_cast<u32>(m_playerClass), hp, maxHp);
    }
    return ok;
}

