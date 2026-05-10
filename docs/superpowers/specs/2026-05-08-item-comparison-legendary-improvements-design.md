# Item Comparison Tooltips + Legendary World Item Improvements

## Problem

1. In the inventory, you can't easily compare a backpack item to what you have equipped — you have to remember stats while switching views.
2. Legendary world items look the same as rare items on the ground and aren't visible on the minimap. They can be missed or accidentally ignored when multiple items drop together.

## Design

### 1. Side-by-Side Inventory Comparison

When hovering over a backpack item in the inventory screen, draw TWO tooltips:
- **Left:** The hovered backpack item (existing `drawItemTooltip`)
- **Right:** The currently equipped item in the matching slot (same tooltip format)

If the matching equipment slot is empty, show "Empty [Slot]" placeholder on the right side.

**Implementation:**
- In `updateInventoryInteraction` (engine.cpp ~line 3505), track which backpack slot the cursor is on
- In the inventory render section, when a backpack tooltip is drawn, also look up `m_inventories[idx].equipped[def.slot]` and draw a second tooltip beside it
- Reuse the existing `HUD::drawItemTooltip` function for both sides
- Add a small "EQUIPPED" label above the right tooltip

**Files:** `src/engine/engine.cpp` (inventory render section), `src/renderer/hud.h` / `hud.cpp` (minor: may need to expose tooltip width for layout)

### 2. Legendary Minimap Dots

Draw legendary world items as gold pulsing dots on the minimap.

**Implementation:**
- In the minimap render section (engine.cpp), loop through `m_worldItems` and for each active legendary item, project its world position to minimap coordinates and draw a gold dot.
- Use a pulsing alpha (`0.6 + 0.4 * sin(timer)`) so they catch the eye.

**Files:** `src/engine/engine.cpp` (minimap render section)

### 3. Legendary Glow Enhancement

Add 2 extra diagonal glow lines to the existing rarity cross effect for legendary items, making them more prominent on the ground.

**Implementation:**
- In `renderWorldItems` (engine.cpp ~line 6676), after the existing 6 glow lines, add 2 more at different diagonal angles for legendary rarity only.

**Files:** `src/engine/engine.cpp` (renderWorldItems)

### 4. Legendary Pickup Priority

When pressing pickup with multiple items nearby, legendaries get a score bonus so they're grabbed first.

**Implementation:**
- In the pickup scoring code (engine.cpp ~line 3510), add `+0.5f` to the score for legendary items.

**Files:** `src/engine/engine.cpp` (pickup section)

### 5. Legendary Pickup Hint Shows Skill

The pickup hint already shows the item name in rarity color. For legendaries with a skill, append the skill name: `"Crown of Storms [Chain Lightning]"`.

**Implementation:**
- In the pickup hint display (engine.cpp ~line 6920), if the item is legendary and has a `legendarySkillId != NONE`, format the text as `"ItemName [SkillName]"`.
- Use the existing `SkillDef` name lookup.

**Files:** `src/engine/engine.cpp` (pickup hint render)

## Verification

- Open inventory with items in backpack → hover over a weapon → see your equipped weapon tooltip next to it
- Hover over a ring → see your equipped ring (or "Empty Ring") beside it
- Drop a legendary → gold dot appears on minimap, pulsing
- Legendary on ground has more glow lines than rare items
- Press pickup near a legendary + a common → legendary is picked up
- Look at a legendary on the ground → hint shows "ItemName [SkillName]"
