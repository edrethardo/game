#pragma once

#include "core/types.h"
#include "core/math.h"

#include "game/item.h"   // SkillId / ItemSlot / SkillDef — the skill tooltip is typed on them

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

    // Enemy health bar across the TOP of the screen (Diablo 2 style): the name of what you are
    // aiming at (or last hit), and how much of it is left.
    //   name      — "Grimfang the Molten", "Skeleton", a boss's name…
    //   subtitle  — affix list for a champion, or nullptr
    //   hpFrac    — 0..1
    //   accent    — name colour (champion tint / boss red / plain white)
    void drawTargetBar(u32 screenWidth, u32 screenHeight,
                       const char* name, const char* subtitle,
                       f32 hpFrac, Vec3 accent, f32 fade);

    // Render weapon name text indicator (just a colored bar placeholder).
    void drawWeaponIndicator(u32 screenWidth, u32 screenHeight, u8 weaponSlot);

    // Menu elements
    void drawMenuOption(u32 screenWidth, u32 screenHeight,
                        f32 y, f32 width, f32 height,
                        Vec3 color, bool selected);
    // Filled rect at an arbitrary x (drawMenuOption is centre-anchored, so it can't lay out a grid).
    // Used by the lobby-code screen for its glyph slots and on-screen keyboard cells.
    void drawRectAt(u32 screenWidth, u32 screenHeight,
                    f32 x, f32 y, f32 width, f32 height, Vec3 color);

    // Network stats overlay
    void drawNetStats(u32 screenWidth, u32 screenHeight,
                      u32 playerCount, u32 ping, const char* role);

    // Profiler overlay (F3)
    void drawProfiler(u32 screenWidth, u32 screenHeight);

    // Inventory screen (replaces normal HUD when Tab is open).
    // `skillDefs` feeds the legendary block's description resolver (see resolveSkillDescription):
    // without it the tooltip would fall back to the legendary-only C++ table and could disagree
    // with the skill-bar tooltip on the same screen.
    // selectedSlot == 0xFF means "no item selected" — used when the controller cursor is parked on a
    // skill bar, so neither item panel paints a phantom highlight.
    void drawInventoryScreen(u32 sw, u32 sh,
                              const PlayerInventory& inv,
                              const ItemDef* itemDefs,
                              const SkillDef* skillDefs, u32 skillDefCount,
                              u8 selectedSlot, bool selectedIsEquipped,
                              s32 mouseX = -1, s32 mouseY = -1);

    // Skill name / description resolution — the SINGLE entry point, used by both the item tooltip's
    // legendary block and the skill-bar tooltip, so one skill can't describe itself two ways.
    // Resolution: slot-specific override -> SkillDef (skills.json) -> legacy table (for the
    // legendary passives that have no SkillDef at all). Pass ItemSlot::COUNT for a class skill.
    const char* resolveSkillName(SkillId id, const SkillDef* skillDefs, u32 skillDefCount);
    const char* resolveSkillDescription(SkillId id, ItemSlot slot,
                                        const SkillDef* skillDefs, u32 skillDefCount);

    // Tooltip for a skill-bar slot (hover with mouse / select with gamepad on the inventory screen).
    struct SkillTooltipInfo {
        const char*    name        = "";
        const char*    subtitle    = "";      // "Class Skill" / "Legendary - Armor" ...
        const char*    description = "";      // from resolveSkillDescription
        const SkillDef* def        = nullptr; // nullptr for a def-less passive -> stats block omitted
        u8   unlockFloor  = 0;                // class skills only (0 = not applicable)
        u8   upgradeFloor = 0;
        bool unlocked     = true;
        bool upgraded     = false;
    };
    void drawSkillTooltip(u32 sw, u32 sh, f32 tipX, f32 tipY, const SkillTooltipInfo& info);

    // Energy bar (blue bar below health bar)
    void drawEnergyBar(u32 sw, u32 sh, f32 energy, f32 maxEnergy);

    // Potion belt flask — drawn beside the health/energy bars. Shows cooldown state
    // (same language as the skill slots) plus a low-HP red "drink now" pulse. The
    // potion is an infinite 5s-cooldown heal, so there is no count.
    struct PotionHudState {
        f32 cooldownRemaining; // seconds; >0 = cooling
        f32 maxCooldown;       // denominator for the radial sweep
        f32 healthFrac;        // 0..1 current HP fraction
        f32 readyFlash;        // 0..POP_DURATION; >0 = just came ready (green pop)
        bool urgent;           // low HP + ready -> red breathing pulse (shares the vignette's red + low-HP trigger)
        f32 pulsePhase;        // shared HUD pulse clock (m_statsTimer); drawn at a 2*pi-multiple Hz so it's continuous across its 1 s wrap
        const char* keyLabel;  // "Q" / "B"
    };
    void drawPotionFlask(u32 sw, u32 sh, f32 x, f32 y, const PotionHudState& st);

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
    // edgeColor draws a bright line along the sweep boundary so progress is legible
    // against the dark cover.
    void drawRadialCooldown(f32 cx, f32 cy, f32 radius, f32 fraction,
                            Vec3 color, Vec3 edgeColor);

    // Shared "ability is back" pop — an expanding, fading square ring drawn around a
    // slot/flask. t01 MUST be pre-clamped to [0,1] (pass HudCooldown::readyPopT(flashTimer)):
    // 1 = the instant of readiness, 0 = pop finished. scale = caller uiScale. Fades toward
    // black, so it reads as a dissolve only over the dark HUD backdrop.
    void drawReadyPop(f32 cx, f32 cy, f32 baseHalf, f32 t01, f32 scale, Vec3 color);

    // Class skill bar — 4 skill slots with key icons, selection, cooldown
    // flashTimers: per-slot pop timer (0..POP_DURATION) — drives the green "ready" pop when > 0
    void drawClassSkillBar(u32 sw, u32 sh, f32 x, f32 y,
                            u8 activeSlot, u32 currentFloor,
                            const u8* unlockFloors, const u8* upgradeFloors,
                            const f32* cooldownTimers, const f32* maxCooldowns,
                            const f32* flashTimers = nullptr,
                            const u8* skillIds = nullptr);

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
        f32  readyFlash = 0.0f; // 0..POP_DURATION; drives the green "ready" pop (set by caller)
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

    // Loot notification bar (center-top, fades out)
    void drawLootNotification(u32 sw, u32 sh, Vec3 color, f32 alpha);

    // Item tooltip — drawn near the hovered item slot. `skillDefs` feeds the legendary block through
    // resolveSkillDescription (see drawInventoryScreen).
    void drawItemTooltip(u32 sw, u32 sh, f32 tipX, f32 tipY,
                         const ItemInstance& item, const ItemDef& def,
                         const SkillDef* skillDefs = nullptr, u32 skillDefCount = 0);

    // Quickbar — slots at bottom-center of screen. Geometry comes from
    // InventoryUI::quickbarLayout(sw, sh), which also bakes in the rightward nudge that clears the
    // bottom-left potion flask. There is deliberately NO xShift parameter: a caller-supplied shift
    // is how the drawn bar and the hit-test rects drifted apart in the first place.
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

    // Filled horizontal bar with optional background.
    // (x,y) is the bottom-left corner in HUD pixel coords (origin bottom-left).
    // bgColor: background bar; fgColor: filled portion up to pct [0,1].
    void drawFilledBar(u32 sw, u32 sh, f32 x, f32 y, f32 w, f32 h,
                       f32 pct, Vec3 bgColor, Vec3 fgColor);
}
