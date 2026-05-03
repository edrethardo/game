#pragma once

#include "core/types.h"

// Centralised tuning constants. Replaces magic numbers scattered across
// engine.cpp, enemy_ai.cpp, item.cpp, projectile.cpp, and hud.cpp.
// Changing a value here takes effect everywhere it's consumed.

namespace GameConst {
    // Enemy base stats (before floor scaling)
    static constexpr f32 SKELETON_HEALTH     = 40.0f;
    static constexpr f32 SKELETON_SPEED      = 2.5f;
    static constexpr f32 SKELETON_DAMAGE     = 8.0f;
    static constexpr f32 SKELETON_DET_RANGE  = 12.0f;
    static constexpr f32 SKELETON_ATK_RANGE  = 2.5f;
    static constexpr f32 SKELETON_ATK_COOL   = 1.2f;

    static constexpr f32 BAT_HEALTH          = 25.0f;
    static constexpr f32 BAT_SPEED           = 4.5f;
    static constexpr f32 BAT_DAMAGE          = 6.0f;
    static constexpr f32 BAT_DET_RANGE       = 12.0f;
    static constexpr f32 BAT_ATK_RANGE       = 2.5f;
    static constexpr f32 BAT_ATK_COOL        = 1.0f;

    static constexpr f32 SPIDER_HEALTH       = 35.0f;
    static constexpr f32 SPIDER_SPEED        = 3.0f;
    static constexpr f32 SPIDER_DAMAGE       = 8.0f;
    static constexpr f32 SPIDER_DET_RANGE    = 10.0f;
    static constexpr f32 SPIDER_ATK_RANGE    = 2.0f;
    static constexpr f32 SPIDER_ATK_COOL     = 1.0f;

    // Floor scaling
    static constexpr f32 FLOOR_STAT_MULT     = 0.25f;

    // Combat
    static constexpr f32 LOOT_DROP_CHANCE    = 0.40f;
    static constexpr f32 HEALTH_GLOBE_CHANCE = 0.30f;
    static constexpr f32 ENERGY_GLOBE_CHANCE = 0.20f;
    static constexpr f32 HEALTH_GLOBE_AMOUNT = 20.0f;
    static constexpr f32 ENERGY_GLOBE_AMOUNT = 25.0f;
    static constexpr f32 POTION_HEAL_PCT     = 0.40f;
    static constexpr f32 POTION_COOLDOWN     = 15.0f;

    // World items
    static constexpr f32 ITEM_SCALE          = 1.4f;
    static constexpr f32 GLOBE_SCALE         = 0.4f;
    static constexpr f32 GLOBE_PICKUP_RADIUS = 2.5f;

    // NPC
    static constexpr f32 NPC_HEALTH          = 40.0f;
    static constexpr f32 NPC_FOLLOW_DIST     = 4.0f;
    static constexpr f32 MIMIC_TRIGGER_DIST  = 2.5f;
    static constexpr f32 MIMIC_HEALTH        = 60.0f;
    static constexpr f32 MIMIC_DAMAGE        = 20.0f;

    // Inventory UI
    static constexpr f32 DBLCLICK_TIME       = 0.3f;
    static constexpr s32 DRAG_DEADZONE_SQ    = 9;
}
