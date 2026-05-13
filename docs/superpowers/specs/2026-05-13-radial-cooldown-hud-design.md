# Radial Cooldown HUD Design

## Context

The current skill bars use 32px slots with a rectangular top-down cooldown overlay. These are too small to read on Switch handheld (720p) and the rectangular fill doesn't match the ARPG convention of radial pie-sweep timers (Diablo, WoW, Path of Exile). This spec upgrades both the class skill bar and equipment skill bar to use 64px slots with radial cooldown sweeps.

## Changes

### Slot Size

Scale all skill slots from 32px to **64px** at 720p reference height. Integer-friendly: 64px at 720p, 96px at 1080p (1.5x). Adjust gap spacing from 3px to 4px.

- Class skill bar: 4 slots × 64px + 3 gaps × 4px = 268px wide
- Equipment skill bar: up to 4 slots, same sizing, dynamic width

Position stays the same (class bar left of quickbar, equipment bar above class bar) but shifted to account for the larger size.

### Radial Cooldown Sweep

Replace the rectangular top-down fill with a clockwise **pie sweep** drawn from 12 o'clock:

- When cooldown starts: full circle is dark overlay
- As cooldown expires: the dark pie shrinks clockwise (revealing the ready portion)
- When ready: no overlay, full icon visible

**Drawing method:** ~24 line segments radiating from slot center to edge, forming filled triangles. Each segment is a `pushLine()` call from center to the arc point. The arc angle = `cooldownFraction * 2π`, starting from 12 o'clock (angle 0 = up).

```
for each angle step from 0 to (cooldownFraction * 2π):
    compute (x, y) on circle edge at that angle
    pushLine(centerX, centerY, edgeX, edgeY, darkColor)
```

Use ~24 steps for a smooth-looking circle at 64px. The overlay color is `{0.05, 0.05, 0.08}` at ~85% opacity (matches the dark sci-fantasy tone).

### Active Slot Highlight

The currently selected class skill slot gets a **bright border** (white or gold `{0.9, 0.8, 0.3}`) to indicate which skill right-click/ZL will activate. Other slots use a dim grey border `{0.3, 0.3, 0.3}`.

### Ready Flash

When a skill comes off cooldown, briefly pulse the border to bright white for 0.15 seconds. Track this with a `readyFlashTimer` per slot — set to 0.15 when `cooldownTimer` transitions from >0 to 0, decay each frame.

### Key Binding Labels

Keep the existing key symbols (1/2/3/4 for class skills, F/G for equipment) but render them slightly larger to match the 64px slot. Position at bottom-left corner of each slot.

## Files to Modify

| File | Change |
|------|--------|
| `src/renderer/hud.h` | Update slot size constants, add radial draw helper declaration |
| `src/renderer/hud.cpp` | Rewrite `drawClassSkillBar()` and `drawEquipSkillBar()` with 64px slots + radial sweep. Add `drawRadialCooldown()` helper. |
| `src/engine/engine_hud.cpp` | Update positioning math for larger slots. Add ready-flash timer tracking. |

## Verification

1. Build PC + Switch
2. In-game: activate a skill, verify the radial pie sweep fills then shrinks clockwise
3. Verify the active slot has a bright border, others are dim
4. Verify key labels render at correct positions
5. Test on Switch handheld (720p) — slots should be clearly readable
6. Test on Switch docked (1080p) — slots scale cleanly to 96px
7. Verify split-screen: each player gets their own skill bar with independent cooldowns
