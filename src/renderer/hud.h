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

    // Flush all accumulated HUD vertices in a single draw call. Call once at end of frame.
    void flush(u32 screenWidth, u32 screenHeight);

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

    // Keyboard key symbol — small key-shaped box with label centered inside.
    // Auto-detects controller button names (A/B/X/Y/ZR etc.) when gamepad connected
    // and draws colored Nintendo-style button shapes instead.
    void drawKeySymbol(u32 sw, u32 sh, f32 x, f32 y,
                        const char* label, bool highlighted);

    // Controller button symbol — colored circle/pill for Nintendo-style buttons
    void drawControllerButton(u32 sw, u32 sh, f32 x, f32 y,
                               const char* label, bool highlighted);

    // Radial pie-sweep cooldown overlay — draws clockwise from 12 o'clock.
    // fraction=1.0 fills entire circle, fraction=0.0 draws nothing.
    void drawRadialCooldown(f32 cx, f32 cy, f32 radius, f32 fraction, Vec3 color);

    // Class skill bar — 4 skill slots with key icons, selection, cooldown
    // flashTimers: per-slot countdown (seconds) — slot border flashes white when > 0
    void drawClassSkillBar(u32 sw, u32 sh, f32 x, f32 y,
                            u8 activeSlot, u32 currentFloor,
                            const u8* unlockFloors, const u8* upgradeFloors,
                            const f32* cooldownTimers, const f32* maxCooldowns,
                            const f32* flashTimers = nullptr);

    // Equipment skill bar — shows active legendary equipment skills (boots F, helmet G,
    // armor passive, weapon proc) with 8x8 pixel-art skill icons and cooldown overlays.
    // Up to 4 slots; empty slots (NONE) are hidden.
    struct EquipSkillSlot {
        u8  skillId;       // SkillId cast to u8
        f32 cooldownTimer; // 0 = ready
        f32 maxCooldown;   // total cooldown for percentage calculation
        const char* keyLabel;   // "F", "G", etc.
        const char* skillName;  // display name
        bool isPassive;    // armor aura / weapon proc (no key activation)
    };
    void drawEquipSkillBar(u32 sw, u32 sh, f32 x, f32 y,
                            const EquipSkillSlot* slots, u32 slotCount);

    // Mouse button symbol — mouse outline with specified button highlighted
    // button: 0=left, 1=right, 2=middle
    void drawMouseButton(u32 sw, u32 sh, f32 x, f32 y,
                          u8 button, bool highlighted);

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

    // Status effect icons above health bar — shows active debuffs/buffs with timers
    struct StatusEffect {
        const char* label; // short text (e.g. "PSN", "BRN")
        Vec3 color;        // icon tint color
        f32  timer;        // remaining duration (0 = inactive), drives blink
        f32  displayValue; // if >= 0, shown as text instead of timer (e.g. stack count)
    };
    void drawStatusIcons(u32 sw, u32 sh, f32 x, f32 y,
                          const StatusEffect* effects, u32 count);

    // Speech bubble — dark background with text, rendered at screen position (x,y).
    // textColor is pre-chosen (green for allies, red for enemies); alpha for fade-out.
    void drawSpeechBubble(u32 sw, u32 sh, f32 x, f32 y,
                          const char* text, Vec3 textColor, f32 alpha);

    // Red vignette overlay when taking damage — draws red gradient edges
    void drawDamageVignette(u32 sw, u32 sh, f32 intensity);

    // CS-style directional damage arc — red arc around crosshair pointing toward attacker
    void drawDamageDirection(u32 sw, u32 sh, f32 angle, f32 alpha);
}
