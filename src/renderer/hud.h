#pragma once

#include "core/types.h"
#include "core/math.h"

struct PlayerInventory;
struct ItemDef;
struct ItemInstance;
struct QuickbarState;

namespace HUD {
    void init();
    void shutdown();

    // Render crosshair at screen centre. Call after 3D rendering, before swap.
    void drawCrosshair(u32 screenWidth, u32 screenHeight, Vec3 color);

    // Render hit marker (X shape) at screen centre. alpha fades out.
    void drawHitMarker(u32 screenWidth, u32 screenHeight, f32 alpha);

    // Render a simple health bar at bottom-left.
    void drawHealthBar(u32 screenWidth, u32 screenHeight,
                       f32 health, f32 maxHealth);

    // Render weapon name text indicator (just a colored bar placeholder).
    void drawWeaponIndicator(u32 screenWidth, u32 screenHeight, u8 weaponSlot);

    // Menu elements
    void drawMenuOption(u32 screenWidth, u32 screenHeight,
                        f32 y, f32 width, f32 height,
                        Vec3 color, bool selected);

    // Network stats overlay
    void drawNetStats(u32 screenWidth, u32 screenHeight,
                      u32 playerCount, u32 ping, const char* role);

    // Profiler overlay (F3)
    void drawProfiler(u32 screenWidth, u32 screenHeight);

    // Inventory screen (replaces normal HUD when Tab is open)
    void drawInventoryScreen(u32 sw, u32 sh,
                              const PlayerInventory& inv,
                              const ItemDef* itemDefs,
                              u8 selectedSlot, bool selectedIsEquipped,
                              s32 mouseX = -1, s32 mouseY = -1);

    // Energy bar (blue bar below health bar)
    void drawEnergyBar(u32 sw, u32 sh, f32 energy, f32 maxEnergy);

    // Summon portrait — icon + name + optional health bar + optional count.
    // healthFrac < 0 means no health bar. count <= 1 means no count shown.
    // iconMatId: material ID for the portrait texture (0 = use colored square fallback).
    void drawSummonPortrait(u32 sw, u32 sh, f32 x, f32 y,
                             const char* name, Vec3 iconColor,
                             f32 healthFrac, u32 count, u8 iconMatId = 0);

    // Skill cooldown indicator (small square near weapon indicator)
    void drawSkillCooldown(u32 sw, u32 sh, f32 cooldownPct);

    // Loot notification bar (center-top, fades out)
    void drawLootNotification(u32 sw, u32 sh, Vec3 color, f32 alpha);

    // Item tooltip — drawn near the hovered item slot
    void drawItemTooltip(u32 sw, u32 sh, f32 tipX, f32 tipY,
                         const ItemInstance& item, const ItemDef& def);

    // Quickbar — 8 slots at bottom-center of screen
    void drawQuickbar(u32 sw, u32 sh,
                      const QuickbarState& qb,
                      const PlayerInventory& inv,
                      const ItemDef* itemDefs,
                      f32 cooldownPct);

    // Speech bubble — dark background with text, rendered at screen position (x,y).
    // textColor is pre-chosen (green for allies, red for enemies); alpha for fade-out.
    void drawSpeechBubble(u32 sw, u32 sh, f32 x, f32 y,
                          const char* text, Vec3 textColor, f32 alpha);
}
