# Item Comparison & Legendary Improvements Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add side-by-side item comparison in inventory, and make legendary world items more visible (minimap dots, extra glow, pickup priority, skill name in hint).

**Architecture:** Five independent changes to existing files — no new files needed. Each task modifies a specific section of engine.cpp or hud.cpp and can be tested independently.

**Tech Stack:** C++17, OpenGL 3.3, existing HUD/font/minimap systems.

---

## File Structure

| Action | Path | Change |
|--------|------|--------|
| Modify | `src/renderer/hud.cpp` | Side-by-side tooltip in inventory backpack hover |
| Modify | `src/engine/engine.cpp:~6686` | Extra glow lines for legendary world items |
| Modify | `src/engine/engine.cpp:~3512` | Legendary pickup priority score bonus |
| Modify | `src/engine/engine.cpp:~6920` | Legendary pickup hint shows skill name |
| Modify | `src/engine/engine.cpp:~7276` | Legendary minimap dots |

---

### Task 1: Side-by-Side Inventory Comparison Tooltip

**Files:**
- Modify: `src/renderer/hud.cpp:1272-1285` (backpack tooltip section)

- [ ] **Step 1: Add equipped comparison tooltip when hovering backpack items**

In `hud.cpp`, find the backpack tooltip section (around line 1272-1285). Currently it draws one tooltip for the hovered backpack item. Add a second `drawItemTooltip` call for the equipped item in the matching slot.

```cpp
        // Check backpack slots
        for (u32 i = 0; i < MAX_INVENTORY_ITEMS; i++) {
            u32 col = i % 6;
            u32 row = i / 6;
            f32 x = bpX + static_cast<f32>(col) * (cellSize + cellGap);
            f32 y = bpStartY - static_cast<f32>(row) * (cellSize + cellGap);

            if (mx >= x && mx <= x + cellSize && my >= y && my <= y + cellSize) {
                if (!isItemEmpty(inv.backpack[i])) {
                    const ItemDef& bpDef = itemDefs[inv.backpack[i].defId];
                    // Backpack item tooltip (left side)
                    drawItemTooltip(sw, sh, x + cellSize + 8.0f, y,
                                    inv.backpack[i], bpDef);
                    // Equipped comparison tooltip (right side, offset by ~160px)
                    u32 slotIdx = static_cast<u32>(bpDef.slot);
                    if (slotIdx < static_cast<u32>(ItemSlot::COUNT)) {
                        const ItemInstance& eq = inv.equipped[slotIdx];
                        if (!isItemEmpty(eq)) {
                            // "EQUIPPED" label
                            f32 eqTipX = x + cellSize + 170.0f;
                            FontSystem::drawText(sw, sh, eqTipX, y + 6.0f,
                                "EQUIPPED", {0.6f, 0.6f, 0.7f}, 1);
                            drawItemTooltip(sw, sh, eqTipX, y - 10.0f,
                                            eq, itemDefs[eq.defId]);
                        } else {
                            f32 eqTipX = x + cellSize + 170.0f;
                            const char* slotNames[] = {"Weapon","Offhand","Helmet","Armor","Boots","Ring"};
                            char emptyText[32];
                            std::snprintf(emptyText, sizeof(emptyText), "Empty %s",
                                slotIdx < 6 ? slotNames[slotIdx] : "Slot");
                            FontSystem::drawText(sw, sh, eqTipX, y,
                                emptyText, {0.4f, 0.4f, 0.5f}, 1);
                        }
                    }
                }
                break;
            }
        }
```

- [ ] **Step 2: Build and verify**

Run: `cmake --build build`
Expected: Clean compilation. Open inventory, hover over backpack item — see two tooltips side by side.

- [ ] **Step 3: Commit**

```bash
git add src/renderer/hud.cpp
git commit -m "feat: add side-by-side item comparison tooltip in inventory"
```

---

### Task 2: Legendary Extra Glow Lines

**Files:**
- Modify: `src/engine/engine.cpp:~6686` (renderWorldItems glow section)

- [ ] **Step 1: Add 2 extra glow lines for legendary items**

After the existing 6 glow lines and loot beam (around line 6688), add extra lines for legendary rarity:

```cpp
            // Loot beam from floor upward
            DebugDraw::line({pos.x, floorY, pos.z}, {pos.x, floorY + 4.0f, pos.z}, color);

            // Legendary: extra glow for prominence
            if (wi.item.rarity == Rarity::LEGENDARY) {
                f32 ge = gr * 0.5f;
                Vec3 gold = {1.0f, 0.85f, 0.3f};
                DebugDraw::line(pos - Vec3{ge, ge, ge}, pos + Vec3{ge, ge, ge}, gold * glowPulse);
                DebugDraw::line(pos - Vec3{ge, -ge, ge}, pos + Vec3{ge, -ge, ge}, gold * glowPulse);
                // Taller loot beam for legendaries
                DebugDraw::line({pos.x, floorY, pos.z}, {pos.x, floorY + 6.0f, pos.z},
                    gold * (0.5f + glowPulse * 0.5f));
            }
```

- [ ] **Step 2: Build and verify**

Run: `cmake --build build`
Expected: Legendary items on ground have extra diagonal glow lines and a taller golden loot beam.

- [ ] **Step 3: Commit**

```bash
git add src/engine/engine.cpp
git commit -m "feat: add extra glow lines for legendary world items"
```

---

### Task 3: Legendary Pickup Priority

**Files:**
- Modify: `src/engine/engine.cpp:~3512` (pickup scoring)

- [ ] **Step 1: Add score bonus for legendary items**

Find the pickup scoring line:
```cpp
f32 score = dot - hDist * 0.1f;
```

Add a legendary bonus after it:
```cpp
            f32 score = dot - hDist * 0.1f;
            // Legendary items get pickup priority when multiple items are nearby
            if (w.item.rarity == Rarity::LEGENDARY) score += 0.5f;
```

- [ ] **Step 2: Build and verify**

Run: `cmake --build build`
Expected: When standing near a legendary and a common item, pressing pickup grabs the legendary first.

- [ ] **Step 3: Commit**

```bash
git add src/engine/engine.cpp
git commit -m "feat: add legendary pickup priority"
```

---

### Task 4: Legendary Pickup Hint Shows Skill Name

**Files:**
- Modify: `src/engine/engine.cpp:~6920` (pickup hint display)

- [ ] **Step 1: Append skill name to legendary pickup hint**

Find the pickup hint text rendering (around line 6927):
```cpp
FontSystem::drawText(sw, sh, cx + 22.0f, cy, bestDef->name, rColor, 1);
```

Replace with:
```cpp
            // Show skill name for legendary items
            if (bestItem->item.rarity == Rarity::LEGENDARY &&
                bestDef->legendarySkillId != SkillId::NONE) {
                // Find skill name from skill defs
                const char* skillName = nullptr;
                for (u32 si = 0; si < m_skillDefCount; si++) {
                    if (m_skillDefs[si].id == bestDef->legendarySkillId) {
                        skillName = m_skillDefs[si].name;
                        break;
                    }
                }
                if (skillName) {
                    char hintBuf[96];
                    std::snprintf(hintBuf, sizeof(hintBuf), "%s [%s]", bestDef->name, skillName);
                    f32 hintW = FontSystem::textWidth(hintBuf, 1);
                    f32 totalW2 = 22.0f + hintW;
                    f32 cx2 = (static_cast<f32>(sw) - totalW2) * 0.5f;
                    HUD::drawKeySymbol(sw, sh, cx2, cy - 2.0f,
                        Input::isGamepadConnected(0) ? "X" : "E", true);
                    FontSystem::drawText(sw, sh, cx2 + 22.0f, cy, hintBuf, rColor, 1);
                } else {
                    HUD::drawKeySymbol(sw, sh, cx, cy - 2.0f,
                        Input::isGamepadConnected(0) ? "X" : "E", true);
                    FontSystem::drawText(sw, sh, cx + 22.0f, cy, bestDef->name, rColor, 1);
                }
            } else {
                HUD::drawKeySymbol(sw, sh, cx, cy - 2.0f,
                    Input::isGamepadConnected(0) ? "X" : "E", true);
                FontSystem::drawText(sw, sh, cx + 22.0f, cy, bestDef->name, rColor, 1);
            }
```

- [ ] **Step 2: Build and verify**

Run: `cmake --build build`
Expected: Looking at a legendary item on ground shows `"Crown of Storms [Chain Lightning]"` in pickup hint.

- [ ] **Step 3: Commit**

```bash
git add src/engine/engine.cpp
git commit -m "feat: show legendary skill name in pickup hint"
```

---

### Task 5: Legendary Minimap Dots

**Files:**
- Modify: `src/engine/engine.cpp:~7276` (after Minimap::draw call)

- [ ] **Step 1: Draw gold dots for legendary world items on minimap**

After the `Minimap::draw(...)` call (around line 7276), add:

```cpp
        Minimap::draw(sw, sh, m_grid, m_localPlayer.position, m_localPlayer.yaw, m_entities);

        // Legendary world items as pulsing gold dots on minimap
        {
            // Minimap is drawn at top-right corner, 120x120 pixels
            // Player is always at center, map rotates with yaw
            f32 mmSize = 120.0f;
            f32 mmX = static_cast<f32>(sw) - mmSize - 8.0f;
            f32 mmY = static_cast<f32>(sh) - mmSize - 8.0f;
            f32 mmCx = mmX + mmSize * 0.5f;
            f32 mmCy = mmY + mmSize * 0.5f;
            f32 mmScale = mmSize / (MINIMAP_REVEAL_RADIUS * 2.0f * m_grid.cellSize);

            f32 pulse = 0.6f + 0.4f * sinf(m_statsTimer * 5.0f);
            Vec3 gold = {1.0f, 0.85f, 0.3f};

            for (u32 i = 0; i < MAX_WORLD_ITEMS; i++) {
                const WorldItem& wi = m_worldItems.items[i];
                if (!wi.active) continue;
                if (wi.item.rarity != Rarity::LEGENDARY) continue;

                // Relative position to player, rotated by yaw
                f32 dx = wi.position.x - m_localPlayer.position.x;
                f32 dz = wi.position.z - m_localPlayer.position.z;
                f32 cosY = cosf(-m_localPlayer.yaw);
                f32 sinY = sinf(-m_localPlayer.yaw);
                f32 rx = dx * cosY - dz * sinY;
                f32 rz = dx * sinY + dz * cosY;

                f32 sx = mmCx + rx * mmScale;
                f32 sy = mmCy - rz * mmScale;

                // Clamp to minimap bounds
                if (sx < mmX || sx > mmX + mmSize || sy < mmY || sy > mmY + mmSize) continue;

                // Draw pulsing gold dot (small cross)
                f32 dotR = 2.5f;
                DebugDraw::line({sx - dotR, sy, 0}, {sx + dotR, sy, 0}, gold * pulse);
                DebugDraw::line({sx, sy - dotR, 0}, {sx, sy + dotR, 0}, gold * pulse);
            }
        }
```

Note: The minimap uses DebugDraw in screen space. Check how the existing minimap draws entity dots — if it uses a different coordinate system, match that. The minimap position/scale may differ from the values above; check `Minimap::draw` implementation and match its viewport.

- [ ] **Step 2: Build and verify**

Run: `cmake --build build`
Expected: Gold pulsing dots appear on minimap for legendary world items.

- [ ] **Step 3: Commit**

```bash
git add src/engine/engine.cpp
git commit -m "feat: show legendary items as gold dots on minimap"
```

---

### Task 6: Build + Switch Deploy

- [ ] **Step 1: Full build and deploy**

```bash
cmake --build build
docker run --rm -u "$(id -u):$(id -g)" -v /home/aaron/game:/game -w /game devkitpro/devkita64 \
    bash -c "source /opt/devkitpro/switchvars.sh && rm -rf build-switch && \
    cmake -B build-switch -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake -DCMAKE_BUILD_TYPE=Release && \
    cmake --build build-switch"
docker run --rm --network host -v /home/aaron/game:/game devkitpro/devkita64 \
    nxlink -s /game/build-switch/DungeonEngine.nro -a 192.168.2.54
```

- [ ] **Step 2: Playtest checklist**

- [ ] Hover backpack weapon → see equipped weapon tooltip next to it
- [ ] Hover backpack ring with empty ring slot → see "Empty Ring" text
- [ ] Legendary on ground has extra glow + taller gold beam
- [ ] Press pickup near legendary + common → legendary grabbed first
- [ ] Look at legendary → hint shows "ItemName [SkillName]"
- [ ] Legendary drop visible as gold dot on minimap
