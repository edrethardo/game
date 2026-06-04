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


// ---------------------------------------------------------------------------
// renderInventoryHUD — the entire m_inventoryOpen branch:
// controller cursor, drawInventoryScreen, drag icon, button hints, equip tutorial.
// ---------------------------------------------------------------------------
void Engine::renderInventoryHUD(u32 sw, u32 sh) {
    // Inventory screen replaces normal HUD elements
    s32 invMX, invMY;
    Input::getMousePosition(invMX, invMY);
    invMY = static_cast<s32>(sh) - invMY; // flip to HUD coords

    // When using controller, override mouse position with D-pad cursor
    if (Input::isGamepadConnected(0)) {
        // Scale layout relative to 720p reference (matches hud.cpp / inventory_ui.cpp)
        f32 uiScale = static_cast<f32>(sh) / 720.0f;
        f32 bpCell = InventoryUI::BP_CELL * uiScale;
        f32 bpGap  = InventoryUI::BP_GAP * uiScale;
        f32 eqH    = InventoryUI::EQ_H * uiScale;
        f32 eqW    = InventoryUI::EQ_W * uiScale;
        f32 eqGap  = InventoryUI::EQ_GAP * uiScale;

        if (m_invCursorPanel == 0) {
            u32 col = m_invCursorIndex % InventoryUI::BP_COLS;
            u32 row = m_invCursorIndex / InventoryUI::BP_COLS;
            f32 bpX = static_cast<f32>(sw) * 0.42f;
            f32 bpStartY = static_cast<f32>(sh) * 0.5f + 180.0f * uiScale;
            invMX = static_cast<s32>(bpX + col * (bpCell + bpGap) + bpCell * 0.5f);
            invMY = static_cast<s32>(bpStartY - row * (bpCell + bpGap) + bpCell * 0.5f);
        } else {
            f32 eqX = static_cast<f32>(sw) * 0.12f;
            f32 eqStartY = static_cast<f32>(sh) * 0.5f + 220.0f * uiScale;
            invMX = static_cast<s32>(eqX + eqW * 0.5f);
            invMY = static_cast<s32>(eqStartY - m_invCursorIndex * (eqH + eqGap) + eqH * 0.5f);
        }
    }

    // Pass controller cursor selection for highlight rendering
    u8 selSlot = Input::isGamepadConnected(0) ? m_invCursorIndex : 0;
    bool selEquip = Input::isGamepadConnected(0) && m_invCursorPanel == 1;
    HUD::drawInventoryScreen(sw, sh, m_inventories[m_localPlayerIndex],
                              m_itemDefs, selSlot, selEquip, invMX, invMY);

    // Draw dragged item icon at cursor position
    if (isDragActive(m_dragState)) {
        s32 dmx, dmy;
        Input::getMousePosition(dmx, dmy);
        dmy = static_cast<s32>(sh) - dmy;
        if (m_dragState.itemDefId < m_itemDefCount) {
            const ItemDef& dragDef = m_itemDefs[m_dragState.itemDefId];
            // Find the rarity of the dragged item
            Rarity dragRarity = Rarity::COMMON;
            if (m_dragState.source == DragSource::BACKPACK &&
                m_dragState.sourceIndex < MAX_INVENTORY_ITEMS) {
                dragRarity = m_inventories[m_localPlayerIndex].backpack[m_dragState.sourceIndex].rarity;
            } else if (m_dragState.source == DragSource::EQUIPMENT &&
                       m_dragState.sourceIndex < static_cast<u8>(ItemSlot::COUNT)) {
                dragRarity = m_inventories[m_localPlayerIndex].equipped[m_dragState.sourceIndex].rarity;
            }
            ItemIconSystem::drawIcon(sw, sh,
                static_cast<f32>(dmx) - 16.0f,
                static_cast<f32>(dmy) - 16.0f,
                32.0f, dragDef, dragRarity);
        }
    }

    // Inventory button hints (always visible when inventory is open)
    if (Input::isGamepadConnected(0)) {
        f32 hintY = 10.0f;
        f32 hintX = 10.0f;
        HUD::drawKeySymbol(sw, sh, hintX, hintY, "A", true);
        FontSystem::drawText(sw, sh, hintX + 22.0f, hintY + 3.0f, "Equip", {0.6f, 0.6f, 0.6f}, 1);
        HUD::drawKeySymbol(sw, sh, hintX + 75.0f, hintY, "Y", true);
        FontSystem::drawText(sw, sh, hintX + 97.0f, hintY + 3.0f, "Drop", {0.6f, 0.6f, 0.6f}, 1);
        HUD::drawKeySymbol(sw, sh, hintX + 145.0f, hintY, "-", true);
        FontSystem::drawText(sw, sh, hintX + 167.0f, hintY + 3.0f, "Drop All", {0.8f, 0.4f, 0.4f}, 1);
        HUD::drawKeySymbol(sw, sh, hintX + 240.0f, hintY, "L", true);
        FontSystem::drawText(sw, sh, hintX + 262.0f, hintY + 3.0f, "/", {0.6f, 0.6f, 0.6f}, 1);
        HUD::drawKeySymbol(sw, sh, hintX + 272.0f, hintY, "R", true);
        FontSystem::drawText(sw, sh, hintX + 294.0f, hintY + 3.0f, "Panel", {0.6f, 0.6f, 0.6f}, 1);
    }

    // Equip tutorial — shown until the player equips an item (floor 1 only)
    if (m_equipTooltipShown && !m_itemEquippedOnce && m_level.currentFloor <= 1) {
        f32 alpha = 1.0f;
        bool mouseLit = (sinf(m_tutorialPulseTimer * 6.0f) > 0.0f);

        bool ep = Input::isGamepadConnected(0);
        const char* eqText = ep ? "Press A to equip" : "Double-click to equip";
        f32 textW = FontSystem::textWidth(eqText, 3);
        f32 totalW = 22.0f + 6.0f + textW;
        f32 cx = (static_cast<f32>(sw) - totalW) * 0.5f;
        f32 cy = static_cast<f32>(sh) * 0.3f;

        if (ep) HUD::drawKeySymbol(sw, sh, cx, cy, "A", mouseLit);
        else    HUD::drawMouseButton(sw, sh, cx, cy, 0, mouseLit);
        FontSystem::drawText(sw, sh, cx + 24.0f, cy + 4.0f, eqText,
                             {0.9f * alpha, 0.85f * alpha, 0.5f * alpha}, 3);
    }
}

// ---------------------------------------------------------------------------
// renderSkillsHUD — class skill bar + equip skill bar + active skill display.
// Called only in the non-inventory (normal) HUD branch.
// ---------------------------------------------------------------------------
void Engine::renderSkillsHUD(u32 sw, u32 sh) {
    // Class skill bar — 4 slots to the LEFT of the quickbar
    {
        f32 hs4 = static_cast<f32>(sh) / 720.0f;
        const ClassDef& cls = kClassDefs[static_cast<u32>(m_playerClass)];
        // Quickbar is 4 slots × 40px + 3 gaps × 4px = 172px, centered (scaled)
        f32 qbTotalW = QUICKBAR_SLOTS * 40.0f * hs4 + (QUICKBAR_SLOTS - 1) * 4.0f * hs4;
        f32 qbX = (static_cast<f32>(sw) - qbTotalW) * 0.5f;
        // Skill bar: 4×64px slots + 3×4px gaps = 268px (scaled)
        f32 skillBarW = 4 * 64.0f * hs4 + 3 * 4.0f * hs4;
        f32 skillBarX = qbX - skillBarW - 12.0f * hs4;
        f32 skillBarY = 14.0f * hs4; // align with quickbar bottom area

        f32 cooldowns[4];
        f32 maxCooldowns[4];
        for (u32 s = 0; s < 4; s++) {
            cooldowns[s] = m_classSkillStates[s].cooldownTimer;
            const SkillDef* sd = SkillSystem::findSkillDef(m_skillDefs, m_skillDefCount, cls.skills[s]);
            maxCooldowns[s] = sd ? sd->cooldown : 1.0f;
        }

        // Flash effect: briefly highlight slot border white when a skill comes off cooldown
        static f32 s_classSkillFlash[4] = {};
        static f32 s_prevCooldowns[4]   = {};
        for (u8 s = 0; s < 4; s++) {
            if (s_classSkillFlash[s] > 0.0f) s_classSkillFlash[s] -= 1.0f / 60.0f;
            // Transition from on-cooldown to ready triggers the flash
            if (cooldowns[s] <= 0.0f && s_prevCooldowns[s] > 0.0f) {
                s_classSkillFlash[s] = 0.15f;
            }
            s_prevCooldowns[s] = cooldowns[s];
        }

        // Pass skill IDs as u8 array for icon rendering
        u8 skillIdBytes[4];
        for (u8 si = 0; si < 4; si++) skillIdBytes[si] = static_cast<u8>(cls.skills[si]);
        // Effective floor accounts for difficulty so Nightmare/Hell show all skills unlocked
        u32 effectiveFloor = m_level.currentFloor + m_difficulty * 50;
        HUD::drawClassSkillBar(sw, sh, skillBarX, skillBarY,
                                m_activeClassSkill, effectiveFloor,
                                cls.skillUnlockFloor, cls.skillUpgradeFloor,
                                cooldowns, maxCooldowns, s_classSkillFlash, skillIdBytes);

        // Equipment skill bar — shows active legendary equipment skills above class bar
        {
            HUD::EquipSkillSlot equipSlots[4];
            u32 equipCount = 0;

            bool eqPad = Input::isGamepadConnected(0);
            // Boots (F key / L+A)
            if (m_bootSkillStates[m_localPlayerIndex].activeSkill != SkillId::NONE) {
                const SkillDef* sd = SkillSystem::findSkillDef(m_skillDefs, m_skillDefCount,
                                                                 m_bootSkillStates[m_localPlayerIndex].activeSkill);
                equipSlots[equipCount++] = {
                    static_cast<u8>(m_bootSkillStates[m_localPlayerIndex].activeSkill),
                    m_bootSkillStates[m_localPlayerIndex].cooldownTimer, sd ? sd->cooldown : 1.0f,
                    eqPad ? "L+A" : "F", sd ? sd->name : "???", false
                };
            }
            // Helmet (G key / L+B)
            if (m_helmetSkillStates[m_localPlayerIndex].activeSkill != SkillId::NONE) {
                const SkillDef* sd = SkillSystem::findSkillDef(m_skillDefs, m_skillDefCount,
                                                                 m_helmetSkillStates[m_localPlayerIndex].activeSkill);
                equipSlots[equipCount++] = {
                    static_cast<u8>(m_helmetSkillStates[m_localPlayerIndex].activeSkill),
                    m_helmetSkillStates[m_localPlayerIndex].cooldownTimer, sd ? sd->cooldown : 1.0f,
                    eqPad ? "L+B" : "G", sd ? sd->name : "???", false
                };
            }
            // Armor (passive aura)
            if (m_armorAura != SkillId::NONE) {
                const SkillDef* sd = SkillSystem::findSkillDef(m_skillDefs, m_skillDefCount, m_armorAura);
                equipSlots[equipCount++] = {
                    static_cast<u8>(m_armorAura), 0.0f, 0.0f,
                    "", sd ? sd->name : "???", true
                };
            }
            // Weapon (on-hit proc)
            if (m_weaponProc != SkillId::NONE) {
                const SkillDef* sd = SkillSystem::findSkillDef(m_skillDefs, m_skillDefCount, m_weaponProc);
                equipSlots[equipCount++] = {
                    static_cast<u8>(m_weaponProc), 0.0f, 0.0f,
                    "", sd ? sd->name : "???", true
                };
            }
            // Ring passive (proc on low HP — shows cooldown)
            if (m_ringPassive != SkillId::NONE) {
                const SkillDef* sd = SkillSystem::findSkillDef(m_skillDefs, m_skillDefCount, m_ringPassive);
                f32 maxCD = (m_ringPassive == SkillId::DIVINE_JUDGMENT) ? 45.0f : 60.0f;
                equipSlots[equipCount++] = {
                    static_cast<u8>(m_ringPassive),
                    m_localPlayer.secondWindCooldown, maxCD,
                    "", sd ? sd->name : "???", true
                };
            }

            if (equipCount > 0) {
                // Position above the class skill bar (scaled).
                // Equip bar: N×64px slots + (N-1)×4px gaps
                f32 equipBarW = equipCount * 64.0f * hs4 + (equipCount - 1) * 4.0f * hs4;
                f32 equipBarX = skillBarX + (skillBarW - equipBarW) * 0.5f;
                // Class bar is now 64px tall; place equip bar 8px above it
                f32 equipBarY = skillBarY + 64.0f * hs4 + 8.0f * hs4;
                HUD::drawEquipSkillBar(sw, sh, equipBarX, equipBarY,
                                        equipSlots, equipCount);
            }
        }
    }

    // Active skill display — right side of screen, shows current right-click skill name
    {
        f32 hs5 = static_cast<f32>(sh) / 720.0f;
        const ClassDef& cls = kClassDefs[static_cast<u32>(m_playerClass)];
        u8 slot = m_activeClassSkill;
        u32 effFloor = m_level.currentFloor + m_difficulty * 50;
        bool unlocked = (effFloor >= cls.skillUnlockFloor[slot]);
        const SkillDef* sd = SkillSystem::findSkillDef(m_skillDefs, m_skillDefCount, cls.skills[slot]);

        f32 rmbX = static_cast<f32>(sw) - 220.0f * hs5;
        f32 rmbY = 15.0f * hs5;

        // Skill activation button icon
        bool skillReady = (m_classSkillStates[slot].cooldownTimer <= 0.0f && unlocked);
        if (Input::isGamepadConnected(0))
            HUD::drawKeySymbol(sw, sh, rmbX, rmbY + 8.0f * hs5, "R", skillReady);
        else
            HUD::drawMouseButton(sw, sh, rmbX, rmbY + 8.0f * hs5, 1, skillReady);

        // Skill name
        const char* skillName = sd ? sd->name : "???";
        Vec3 nameCol = unlocked ? Vec3{0.9f, 0.9f, 1.0f} : Vec3{0.4f, 0.4f, 0.4f};
        if (m_classSkillStates[slot].cooldownTimer > 0.0f) nameCol = {0.6f, 0.4f, 0.3f};
        FontSystem::drawText(sw, sh, rmbX + 25.0f * hs5, rmbY + 22.0f * hs5, skillName, nameCol, 2);

        // Cooldown text
        if (m_classSkillStates[slot].cooldownTimer > 0.0f) {
            char cdTxt[8];
            std::snprintf(cdTxt, sizeof(cdTxt), "%.1fs", m_classSkillStates[slot].cooldownTimer);
            FontSystem::drawText(sw, sh, rmbX + 25.0f * hs5, rmbY + 6.0f * hs5, cdTxt, {1.0f, 0.5f, 0.2f}, 2);
        }
    }
}

// ---------------------------------------------------------------------------
// renderMinimapAndFloor — minimap + legendary-item dots + door blip +
// floor indicator text + potion cooldown.  All reads from m_level.
// Called only in the non-inventory (normal) HUD branch.
// ---------------------------------------------------------------------------
void Engine::renderMinimapAndFloor(u32 sw, u32 sh) {
    // Minimap (top-right corner)
    Minimap::draw(sw, sh, m_level.grid, m_localPlayer.position, m_localPlayer.yaw, m_entities);

    // Legendary item dots on minimap — gold "+" cross at each active legendary world item
    {
        f32 hudScale = static_cast<f32>(sh) / 720.0f;
        f32 mapSize = 150.0f * hudScale;
        f32 margin  = 10.0f * hudScale;
        f32 mapX = static_cast<f32>(sw) - mapSize - margin;
        f32 mapY = static_cast<f32>(sh) - mapSize - margin;

        for (u32 i = 0; i < MAX_WORLD_ITEMS; i++) {
            const WorldItem& wi = m_worldItems.items[i];
            if (!wi.active) continue;
            if (wi.item.rarity != Rarity::LEGENDARY) continue;

            u32 lgx, lgz;
            if (!LevelGridSystem::worldToGrid(m_level.grid, wi.position, lgx, lgz)) continue;

            f32 normX = (static_cast<f32>(lgx) + 0.5f) / static_cast<f32>(m_level.grid.width);
            f32 normZ = (static_cast<f32>(lgz) + 0.5f) / static_cast<f32>(m_level.grid.depth);
            f32 dotX  = mapX + normX * mapSize;
            f32 dotY  = mapY + (1.0f - normZ) * mapSize; // Z flipped to match minimap orientation

            // Pulsing gold cross — two short strokes drawn as "+" text stand-in characters
            f32 pulse    = 0.75f + 0.25f * sinf(m_statsTimer * 4.0f);
            Vec3 goldCol = {1.0f * pulse, 0.8f * pulse, 0.1f * pulse};
            // Draw a small cross using two one-pixel-wide line segments via FontSystem "+" glyph
            FontSystem::drawText(sw, sh, dotX - 3.0f, dotY - 4.0f, "+", goldCol, 1);
        }
    }

    // Door marker on minimap (pulsing green "V" symbol at door grid position)
    if (m_level.floorDoorActive) {
        u32 doorGx, doorGz;
        if (LevelGridSystem::worldToGrid(m_level.grid, m_level.floorDoorPos, doorGx, doorGz)) {
            // Convert grid coords to minimap screen position (scaled)
            f32 hudScale2 = static_cast<f32>(sh) / 720.0f;
            f32 mapSize = 150.0f * hudScale2;
            f32 margin = 10.0f * hudScale2;
            f32 mapX = static_cast<f32>(sw) - mapSize - margin;
            f32 mapY = static_cast<f32>(sh) - mapSize - margin;
            f32 normX = (static_cast<f32>(doorGx) + 0.5f) / static_cast<f32>(m_level.grid.width);
            f32 normZ = (static_cast<f32>(doorGz) + 0.5f) / static_cast<f32>(m_level.grid.depth);
            f32 dotX = mapX + normX * mapSize;
            f32 dotY = mapY + (1.0f - normZ) * mapSize; // Z flipped

            f32 doorPulse = 0.7f + 0.3f * sinf(m_statsTimer * 5.0f);
            // Red while the boss seals the exit, green once it's open (boss floors only).
            Vec3 doorCol = (m_level.floorHasBoss && floorBossAlive())
                ? Vec3{1.0f * doorPulse, 0.2f * doorPulse, 0.2f * doorPulse}
                : Vec3{0.2f * doorPulse, 1.0f * doorPulse, 0.3f * doorPulse};
            FontSystem::drawText(sw, sh, dotX - 3.0f, dotY - 4.0f, "V", doorCol, 1);
        }
    }

    // Floor indicator (top-left) — scaled with resolution
    {
        f32 hs = static_cast<f32>(sh) / 720.0f;
        char floorStr[32];
        std::snprintf(floorStr, sizeof(floorStr), "Floor %u", m_level.currentFloor);
        FontSystem::drawText(sw, sh, 20.0f * hs, static_cast<f32>(sh) - 22.0f * hs,
                             floorStr, {0.7f, 0.7f, 0.7f}, 2);
    }

    // Potion cooldown indicator (below floor text, Q key icon + label)
    {
        f32 hs = static_cast<f32>(sh) / 720.0f;
        // HUD y is y-up (origin bottom), so a larger subtraction sits lower on screen. Nudged
        // from 45 -> 60 px so the potion indicator drops a touch below the floor text.
        f32 potY = static_cast<f32>(sh) - 60.0f * hs;
        bool potReady = (m_potionCooldown <= 0.0f);
        HUD::drawKeySymbol(sw, sh, 20.0f * hs, potY, Input::isGamepadConnected(0) ? "B" : "Q", potReady);
        if (potReady) {
            FontSystem::drawText(sw, sh, 44.0f * hs, potY + 2.0f * hs,
                                 "Potion", {0.3f, 0.8f, 0.3f}, 2);
        } else {
            char potStr[32];
            std::snprintf(potStr, sizeof(potStr), "Potion: %.0fs", m_potionCooldown);
            FontSystem::drawText(sw, sh, 44.0f * hs, potY + 2.0f * hs,
                                 potStr, {0.8f, 0.3f, 0.3f}, 2);
        }
    }
}

// ---------------------------------------------------------------------------
// renderTutorials — all tutorial tooltip overlays (controls, shield, dodge,
// pickup, backpack-full).  Drawn over both inventory and normal HUD branches.
// ---------------------------------------------------------------------------
void Engine::renderTutorials(u32 sw, u32 sh) {
    // Backpack full notification — shown centered at 70% screen height, fades out
    if (m_fullBackpackNotifyTimer > 0.0f) {
        const char* fullText = "Backpack Full!";
        f32 fullW = FontSystem::textWidth(fullText, 2);
        f32 alpha = (m_fullBackpackNotifyTimer < 0.5f) ? m_fullBackpackNotifyTimer * 2.0f : 1.0f;
        Vec3 fullColor = {0.9f * alpha, 0.2f * alpha, 0.2f * alpha};
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - fullW) * 0.5f,
                             static_cast<f32>(sh) * 0.7f, fullText, fullColor, 2);
    }

    // Exit-sealed prompt — shown when the player tries to descend with the boss alive.
    if (m_bossLockNotifyTimer > 0.0f) {
        const char* lockText = "Defeat the boss to descend!";
        f32 lockW = FontSystem::textWidth(lockText, 2);
        f32 alpha = (m_bossLockNotifyTimer < 0.5f) ? m_bossLockNotifyTimer * 2.0f : 1.0f;
        Vec3 lockColor = {0.95f * alpha, 0.5f * alpha, 0.15f * alpha};
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - lockW) * 0.5f,
                             static_cast<f32>(sh) * 0.65f, lockText, lockColor, 2);
    }

    // Floor 1 controls tutorial — LMB Attack / RMB Skill
    if (m_controlsTooltipTimer > 0.0f) {
        f32 alpha = (m_controlsTooltipTimer < 1.0f)
                    ? m_controlsTooltipTimer : 1.0f;
        bool mouseLit = (sinf(m_controlsTooltipTimer * 5.0f) > 0.0f);
        f32 cx = static_cast<f32>(sw) * 0.5f;
        f32 cy = static_cast<f32>(sh) * 0.72f;

        bool cp = Input::isGamepadConnected(0);
        // Attack button
        if (cp) HUD::drawKeySymbol(sw, sh, cx - 120.0f, cy, "ZR", mouseLit);
        else    HUD::drawMouseButton(sw, sh, cx - 120.0f, cy, 0, mouseLit);
        FontSystem::drawText(sw, sh, cx - 98.0f, cy + 5.0f, "Attack",
                             {0.5f * alpha, 0.9f * alpha, 0.5f * alpha}, 3);
        // Skill button
        if (cp) HUD::drawKeySymbol(sw, sh, cx + 35.0f, cy, "R", mouseLit);
        else    HUD::drawMouseButton(sw, sh, cx + 35.0f, cy, 1, mouseLit);
        FontSystem::drawText(sw, sh, cx + 57.0f, cy + 5.0f, "Skill",
                             {0.5f * alpha, 0.6f * alpha, 0.9f * alpha}, 3);
    }

    // Shield tutorial — shown whenever a shield is equipped until the player blocks
    if (!m_shieldBlockedOnce) {
        const ItemInstance& offhand = m_inventories[m_localPlayerIndex].equipped[static_cast<u8>(ItemSlot::OFFHAND)];
        bool hasShield = !isItemEmpty(offhand) &&
                         m_itemDefs[offhand.defId].slot == ItemSlot::OFFHAND;
        if (hasShield) {
            f32 alpha = 1.0f;
            bool keyLit = (sinf(m_tutorialPulseTimer * 6.0f) > 0.0f);
            bool cp = Input::isGamepadConnected(0);
            const char* text = cp ? "Block" : "Block";
            f32 textW = FontSystem::textWidth(text, 3);
            f32 totalW = 28.0f + textW;
            f32 cx = (static_cast<f32>(sw) - totalW) * 0.5f;
            f32 cy = static_cast<f32>(sh) * 0.62f;

            HUD::drawKeySymbol(sw, sh, cx, cy, cp ? "ZL" : "Ctrl", keyLit);
            FontSystem::drawText(sw, sh, cx + 28.0f, cy + 2.0f, text,
                                 {0.5f * alpha, 0.7f * alpha, 0.9f * alpha}, 3);
        }
    }

    // Dodge roll tutorial — shown after shield tutorial is completed, until player dodges
    if (m_shieldBlockedOnce && !m_dodgeRolledOnce) {
        f32 alpha = 1.0f;
        bool keyLit = (sinf(m_tutorialPulseTimer * 6.0f) > 0.0f);
        bool cp = Input::isGamepadConnected(0);
        const char* text = "Dodge Roll";
        f32 textW = FontSystem::textWidth(text, 3);
        f32 totalW = 28.0f + textW;
        f32 cx = (static_cast<f32>(sw) - totalW) * 0.5f;
        f32 cy = static_cast<f32>(sh) * 0.62f;

        // Gamepad label comes from the live DODGE binding (currently R3 / right-stick click)
        // so this tooltip stays correct if the action is ever rebound. Pre-fix this was a
        // hardcoded "B" that drifted out of sync when DODGE moved to RIGHTSTICK.
        const char* dodgeLabel = cp
            ? Input::buttonName(Input::getBinding(GameAction::DODGE).button)
            : "Shift";
        HUD::drawKeySymbol(sw, sh, cx, cy, dodgeLabel, keyLit);
        FontSystem::drawText(sw, sh, cx + 28.0f, cy + 2.0f, text,
                             {0.9f * alpha, 0.7f * alpha, 0.3f * alpha}, 3);
    }

    // First pickup tutorial — shown until the player opens inventory (floor 1 only)
    if (m_firstPickupTooltipShown && !m_inventoryOpenedOnce && m_level.currentFloor <= 1) {
        f32 alpha = 1.0f;
        bool keyLit = (sinf(m_tutorialPulseTimer * 6.0f) > 0.0f);

        const char* text = "Open Inventory";
        f32 textW = FontSystem::textWidth(text, 3);
        f32 totalW = 28.0f + textW;
        f32 cx = (static_cast<f32>(sw) - totalW) * 0.5f;
        f32 cy = static_cast<f32>(sh) * 0.65f;

        HUD::drawKeySymbol(sw, sh, cx, cy, Input::isGamepadConnected(0) ? "+" : "Tab", keyLit);
        FontSystem::drawText(sw, sh, cx + 28.0f, cy + 2.0f, text,
                             {0.9f * alpha, 0.85f * alpha, 0.5f * alpha}, 3);
    }
}

// ---------------------------------------------------------------------------
// renderHUD — all 2D HUD elements: inventory screen or normal HUD
// (health bar, crosshair, quickbar, minimap, skill bars, net stats, profiler)
// ---------------------------------------------------------------------------
void Engine::renderHUD(u32 sw, u32 sh) {
    // Set font scale based on viewport height so text scales with resolution.
    // At 720p = 1.0, at 1080p = 1.5. HUD code passes raw font sizes (1, 2, 3).
    FontSystem::setUIScale(static_cast<f32>(sh) / 720.0f);

    // Pin the HUD batch resolution to native (sw/sh) for the whole pass. flushHUD()
    // projects through a file-static screen size that the SCALED 3D pass leaves set to
    // vpW/vpH (e.g. speech bubbles call HUD::flush(vpW,vpH)). The inventory branch below
    // skips drawCrosshair (which is what re-pins native in the normal HUD), so without
    // this its box backgrounds would project at the scaled size and tear. The batch is
    // empty here (prior passes flushed), so this only sets the size — it draws nothing.
    HUD::flush(sw, sh);

    if (m_menu.confirmQuit) {
        // Paused: hide the entire game HUD (and inventory). Only the pause overlay
        // below is drawn, so the screen looks unremarkable at a glance.
    } else if (m_inventoryOpen) {
        renderInventoryHUD(sw, sh);
    } else {
        Vec3 crossColor = (m_localPlayer.damageFlashTimer > 0.0f)
                        ? Vec3{1.0f, 0.3f, 0.3f}
                        : Vec3{1.0f, 1.0f, 1.0f};
        HUD::drawCrosshair(sw, sh, crossColor);

        if (m_hitMarkerTimer > 0.0f)
            HUD::drawHitMarker(sw, sh, m_hitMarkerTimer / 0.2f);

        // --- Dodge cooldown bar (all classes) ---
        {
            const DodgeState& ds = m_localPlayer.dodgeState;
            f32 hs = static_cast<f32>(sh) / 720.0f;
            f32 cx = static_cast<f32>(sw) * 0.5f;
            f32 cy = static_cast<f32>(sh) * 0.5f;

            if (ds.rolling || ds.cooldownTimer > 0.0f) {
                f32 pct = ds.rolling ? 0.0f : (1.0f - ds.cooldownTimer / 1.0f);
                Vec3 barBg = {0.15f, 0.15f, 0.15f};
                Vec3 barFg = ds.rolling ? Vec3{0.0f, 0.8f, 1.0f} : Vec3{0.5f, 0.5f, 0.5f};
                f32 barW = 30.0f * hs;
                f32 barH = 3.0f * hs;
                f32 bx = cx - barW * 0.5f;
                f32 by = cy - 18.0f * hs;
                HUD::drawFilledBar(sw, sh, bx, by, barW, barH, pct, barBg, barFg);
            }
        }

        // --- Wanderer-specific HUD (adrenaline, Death's Dance) ---
        if (m_playerClass == PlayerClass::WANDERER) {
            const DodgeState& ds = m_localPlayer.dodgeState;
            f32 hs = static_cast<f32>(sh) / 720.0f;
            f32 cx = static_cast<f32>(sw) * 0.5f;
            f32 cy = static_cast<f32>(sh) * 0.5f;

            // Adrenaline stacks: orange pips below the dodge bar
            if (m_localPlayer.adrenalineUnlocked && ds.counterStacks > 0) {
                u8 stacks = ds.counterStacks;
                f32 pipW = 8.0f * hs;
                f32 pipH = 4.0f * hs;
                f32 pipGap = 2.0f * hs;
                f32 totalPipW = stacks * pipW + (stacks - 1) * pipGap;
                f32 px = cx - totalPipW * 0.5f;
                f32 py = cy - 26.0f * hs; // below dodge bar
                Vec3 pipBg = {0.2f, 0.1f, 0.0f};
                Vec3 pipFg = {1.0f, 0.53f, 0.0f}; // orange
                for (u8 i = 0; i < stacks; i++) {
                    HUD::drawFilledBar(sw, sh, px + i * (pipW + pipGap), py,
                                       pipW, pipH, 1.0f, pipBg, pipFg);
                }
            }

            // Death's Dance timer bar: purple, below adrenaline pips
            if (m_localPlayer.deathsDanceTimer > 0.0f) {
                // 8s total duration (matches skill definition)
                f32 pct = m_localPlayer.deathsDanceTimer / 8.0f;
                if (pct > 1.0f) pct = 1.0f;
                f32 barW = 60.0f * hs;
                f32 barH = 4.0f * hs;
                f32 bx = cx - barW * 0.5f;
                f32 by = cy - 34.0f * hs; // below adrenaline pips
                HUD::drawFilledBar(sw, sh, bx, by, barW, barH, pct,
                                   {0.15f, 0.05f, 0.15f}, {0.8f, 0.27f, 1.0f});
            }
        }

    // CS-style directional damage arcs — show where hits came from
    for (u32 i = 0; i < Player::MAX_HIT_INDICATORS; i++) {
        const auto& hi = m_localPlayer.hitIndicators[i];
        if (hi.timer <= 0.0f) continue;
        f32 alpha = fminf(hi.timer * 2.0f, 1.0f) * 0.6f;
        HUD::drawDamageDirection(sw, sh, -hi.angle, alpha);
    }

        // M13: use renderedHealth for bar fill so the bar smoothly lerps on hits
        // rather than snapping. The numeric display (if added later) should read `health`.
        HUD::drawHealthBar(sw, sh, m_localPlayer.renderedHealth, m_localPlayer.maxHealth);

        // Summon portraits — top-left, stacked directly under the Potion tooltip.
        {
            f32 hs = static_cast<f32>(sh) / 720.0f;
            // The Potion tooltip sits at sh - 60*hs; its key symbol extends a little
            // above that. Anchor the portrait stack at sh - 95*hs so the first box
            // (which grows upward from its anchor) clears the potion row, and align
            // its left edge with the floor/potion text at x = 20*hs.
            f32 portX = 20.0f * hs;
            f32 portY = static_cast<f32>(sh) - 95.0f * hs;
            f32 portH = 26.0f, gap = 3.0f;

            // Scan friendly drone-class minions (npcClass NONE). Everything the
            // Tinkerer/Engineer deploys is a drone; split only the ground turret bot
            // out (GENERIC body, not flying) so the rest — ground spiders AND flying
            // bats from Swarm Deploy/Drones/Queen — aggregate into one swarm count.
            // The old scan keyed the swarm off ENT_UNTARGETABLE (never set on these
            // drones) and turrets off moveSpeed<=0 (turrets move), so neither counted.
            u32 swarmCount  = 0;
            u32 turretCount = 0;

            for (u32 a = 0; a < m_entities.activeCount; a++) {
                u32 idx = m_entities.activeList[a];
                Entity& ent = m_entities.entities[idx];
                if (!(ent.flags & ENT_FRIENDLY)) continue;
                if (ent.flags & ENT_DEAD) continue;
                if (ent.npcClass != NpcClass::NONE) continue;

                bool isTurret = (ent.enemyType == EnemyType::GENERIC) && !(ent.flags & ENT_FLYING);
                if (isTurret) turretCount++;
                else          swarmCount++;
            }

            u32 slot = 0;

            static const u8 matSwarm  = MaterialSystem::getIdByName("icon_swarm");
            static const u8 matTurret = MaterialSystem::getIdByName("icon_turret");

            if (swarmCount > 0) {
                HUD::drawSummonPortrait(sw, sh, portX, portY - slot * (portH + gap),
                                         "Swarm", {0.3f, 0.3f, 0.35f}, -1.0f, swarmCount, matSwarm);
                slot++;
            }
            if (turretCount > 0) {
                HUD::drawSummonPortrait(sw, sh, portX, portY - slot * (portH + gap),
                                         "Turret", {0.33f, 0.31f, 0.37f}, -1.0f, turretCount, matTurret);
                slot++;
            }
        }

        // Energy bar
        HUD::drawEnergyBar(sw, sh, m_skillStates[m_localPlayerIndex].energy, m_skillStates[m_localPlayerIndex].maxEnergy);

        // Status effect icons above the energy bar
        {
            // Adrenaline (Wanderer): longest remaining stack drives the blink, the
            // stack count is shown as the displayValue. counterStacks is only ever
            // >0 for Wanderer (the gain is gated on adrenalineUnlocked), so this row
            // entry is inactive for every other class.
            f32 adrTimer = 0.0f;
            const DodgeState& adrDs = m_localPlayer.dodgeState;
            for (u8 ai = 0; ai < adrDs.counterStacks; ai++)
                if (adrDs.counterTimers[ai] > adrTimer) adrTimer = adrDs.counterTimers[ai];

            HUD::StatusEffect statuses[] = {
                {"PSN", {0.2f, 0.8f, 0.2f}, m_localPlayer.poisonTimer, -1.0f},
                {"BRN", {1.0f, 0.5f, 0.1f}, m_localPlayer.burnTimer, -1.0f},
                {"FRZ", {0.4f, 0.7f, 1.0f}, m_localPlayer.freezeTimer, -1.0f},
                {"SLO", {0.6f, 0.3f, 0.9f}, m_localPlayer.slowTimer, -1.0f},
                {"INV", {1.0f, 0.85f, 0.3f}, m_localPlayer.invulnTimer, -1.0f},
                // Soul Harvest: timer drives blink, displayValue shows stack count
                {"SH",  {0.9f, 0.5f, 0.15f}, m_localPlayer.soulHarvestTimer,
                    m_localPlayer.soulHarvestTimer > 0.0f
                        ? static_cast<f32>(m_localPlayer.soulHarvestStacks) : -1.0f},
                // Adrenaline: lightning-bolt icon + stack count (electric yellow)
                {"ADR", {1.0f, 0.85f, 0.2f}, adrTimer,
                    adrDs.counterStacks > 0 ? static_cast<f32>(adrDs.counterStacks) : -1.0f},
            };
            // Energy bar top edge is at y=52, place icons above with gap (scaled)
            f32 hs2 = static_cast<f32>(sh) / 720.0f;
            HUD::drawStatusIcons(sw, sh, 20.0f * hs2, 58.0f * hs2, statuses, 7);
        }

        // Ammo display for hitscan weapons (right side of health bar area)
        {
            WeaponState& ws = m_players[activeNetSlot()].weaponState; // local player's net slot
            const ItemInstance& eqWpn = m_inventories[m_localPlayerIndex].equipped[static_cast<u32>(ItemSlot::WEAPON)];
            WeaponDef wpn;
            if (!isItemEmpty(eqWpn)) {
                wpn = Inventory::getWeaponFromItem(m_inventories[m_localPlayerIndex], m_itemDefs, eqWpn);
            } else {
                wpn = m_weaponDefs[ws.currentWeapon];
            }
            if (wpn.clipSize > 0) {
                f32 hs3 = static_cast<f32>(sh) / 720.0f;
                f32 ammoX = 230.0f * hs3;
                f32 ammoY = 20.0f * hs3;
                if (ws.reloading) {
                    f32 maxReload = (wpn.reloadTime > 0.0f) ? wpn.reloadTime : 1.0f;
                    f32 pct = 1.0f - ws.reloadTimer / maxReload; // 0→1

                    // "Reloading..." text that fills left-to-right with bright color
                    const char* reloadTxt = "Reloading...";
                    f32 fullW = FontSystem::textWidth(reloadTxt, 2);

                    // Dim base text (full word, dark)
                    FontSystem::drawText(sw, sh, ammoX, ammoY + 5.0f, reloadTxt,
                                         {0.3f, 0.2f, 0.1f}, 2);

                    // Bright fill — clip rendering to percentage of text width
                    // Draw character by character, coloring based on fill progress
                    f32 cx = ammoX;
                    for (const char* c = reloadTxt; *c; c++) {
                        char ch[2] = {*c, 0};
                        f32 cw = FontSystem::textWidth(ch, 2);
                        f32 charMid = (cx - ammoX + cw * 0.5f) / fullW;
                        if (charMid < pct) {
                            // Fully filled — bright orange-gold
                            FontSystem::drawText(sw, sh, cx, ammoY + 5.0f, ch,
                                                 {1.0f, 0.8f, 0.2f}, 2);
                        }
                        cx += cw;
                    }

                    // Progress bar below text
                    f32 barW = fullW;
                    for (f32 fy = 0; fy < 3.0f; fy += 1.0f) {
                        DebugDraw::line({ammoX, ammoY - 1.0f + fy, 0},
                                        {ammoX + barW * pct, ammoY - 1.0f + fy, 0},
                                        {1.0f, 0.7f, 0.2f});
                    }
                    // Bar background (dim)
                    for (f32 fy = 0; fy < 3.0f; fy += 1.0f) {
                        DebugDraw::line({ammoX + barW * pct, ammoY - 1.0f + fy, 0},
                                        {ammoX + barW, ammoY - 1.0f + fy, 0},
                                        {0.15f, 0.1f, 0.05f});
                    }
                } else {
                    char ammoStr[16];
                    std::snprintf(ammoStr, sizeof(ammoStr), "%u / %u", ws.currentClip, wpn.clipSize);
                    Vec3 ammoCol = (ws.currentClip <= 3 && wpn.clipSize > 3)
                        ? Vec3{0.9f, 0.3f, 0.2f} : Vec3{0.8f, 0.8f, 0.8f};
                    FontSystem::drawText(sw, sh, ammoX, ammoY + 5.0f, ammoStr, ammoCol, 2);
                }
            }
        }

        renderSkillsHUD(sw, sh);

        renderMinimapAndFloor(sw, sh);
    }

    // Pause menu overlay
    if (m_menu.confirmQuit) {
        f32 cx = static_cast<f32>(sw) * 0.5f;
        f32 cy = static_cast<f32>(sh) * 0.5f;

        const char* title = "PAUSED";
        f32 titleW = FontSystem::textWidth(title, 3);
        FontSystem::drawText(sw, sh, cx - titleW * 0.5f, cy + 50.0f, title, {0.9f, 0.85f, 0.7f}, 3);

        static const char* options[] = {"Continue Playing", "Save and Quit"};
        for (u32 i = 0; i < 2; i++) {
            f32 y = cy + 10.0f - i * 35.0f;
            bool sel = (i == m_menu.subSelection);
            Vec3 col = sel ? Vec3{0.3f, 1.0f, 0.4f} : Vec3{0.4f, 0.4f, 0.5f};
            HUD::drawMenuOption(sw, sh, y, 250, 28, col, sel);
            Vec3 tc = sel ? Vec3{1, 1, 1} : Vec3{0.6f, 0.6f, 0.6f};
            f32 tw = FontSystem::textWidth(options[i], 2);
            FontSystem::drawText(sw, sh, cx - tw * 0.5f, y + 7.0f, options[i], tc, 2);
        }

        const char* hint = Input::isGamepadConnected(0)
            ? "D-pad, A to select, B to resume"
            : "Up/Down, Enter to select, ESC to resume";
        f32 hintW = FontSystem::textWidth(hint, 1);
        FontSystem::drawText(sw, sh, cx - hintW * 0.5f, cy - 50.0f, hint, {0.4f, 0.4f, 0.5f}, 1);
    }

    renderTutorials(sw, sh);

    // Quickbar — always visible at bottom of screen
    {
        f32 cdPct = 0.0f;
        WeaponState& ws = m_players[activeNetSlot()].weaponState; // local player's net slot
        // Get cooldown percentage for active quickbar weapon
        const ItemInstance* activeItem = Quickbar::resolveSlot(
            m_quickbars[m_localPlayerIndex], m_inventories[m_localPlayerIndex],
            m_quickbars[m_localPlayerIndex].activeSlot);
        if (activeItem && !isItemEmpty(*activeItem)) {
            const ItemDef& def = m_itemDefs[activeItem->defId];
            if (def.baseCooldown > 0.0f && ws.cooldownTimer > 0.0f) {
                cdPct = ws.cooldownTimer / def.baseCooldown;
                if (cdPct > 1.0f) cdPct = 1.0f;
            }
        }
        HUD::drawQuickbar(sw, sh, m_quickbars[m_localPlayerIndex],
                           m_inventories[m_localPlayerIndex], m_itemDefs, cdPct);
    }

    // Profiler overlay (F3)
    HUD::drawProfiler(sw, sh);

    // Perf/net debug line — shown ONLY with the profiler overlay (F3 desktop / L+L3 chord on Switch).
    // Sits just above the profiler's scope bars (which start at y0 = sh-30). FPS / frame-ms (1000/FPS,
    // 1 Hz avg) / draw-calls (D, Switch budget ~300-500) / frustum-visible (V), plus the netplay
    // client's smoothed RTT. Gating it behind the profiler keeps the normal HUD clean now that the
    // Switch perf work is done — this used to be an always-on FPS line + a separate "Ping:" line.
    if (getProfiler().enabled) {
        char dbgBuf[96];
        f32 frameMs = (m_displayFps > 0) ? (1000.0f / static_cast<f32>(m_displayFps)) : 0.0f;
        if (m_netRole == NetRole::CLIENT) {
            u32 rtt = static_cast<u32>(Net::getStats(activeNetSlot()).rttMs);
            std::snprintf(dbgBuf, sizeof(dbgBuf), "%u FPS  %.1fms  D:%u V:%u  RTT:%ums",
                          m_displayFps, frameMs, Renderer::getDrawCallCount(),
                          Renderer::getVisibleCount(), rtt);
        } else {
            std::snprintf(dbgBuf, sizeof(dbgBuf), "%u FPS  %.1fms  D:%u V:%u",
                          m_displayFps, frameMs, Renderer::getDrawCallCount(),
                          Renderer::getVisibleCount());
        }
        FontSystem::drawText(sw, sh, 8.0f, static_cast<f32>(sh) - 16.0f,
                             dbgBuf, {0.4f, 1.0f, 0.5f}, 1);
    }

    // Net stats overlay in multiplayer
    if (m_netRole != NetRole::NONE) {
        u32 ping = 0;
        if (m_netRole == NetRole::CLIENT) {
            NetStats stats = Net::getStats(activeNetSlot()); // local player's net slot (arg ignored on client, kept correct)
            ping = static_cast<u32>(stats.rttMs);
        }
        HUD::drawNetStats(sw, sh, Net::getConnectedCount(), ping,
                          m_netRole == NetRole::SERVER ? "HOST" : "CLIENT");
    }

    // D6: detailed net-graph overlay — toggled by F9, CLIENT only.
    // Draws a single line of stats near the top of the screen:
    //   "NET: rtt=<ms> est=<tick> div=<count> loss=<pct>% lat=<ms>ms"
    // Fields:
    //   rtt   — estimated round-trip time (oneWayTripMs * 2) from the clock-sync subsystem
    //   est   — current server-tick estimate (ClockSyncOps::serverTickEst)
    //   div   — prediction reconcile mismatches accumulated since last 1 Hz reset
    //   loss  — fake-loss percentage (m_netFakeLossPct, 0 when off)
    //   lat   — fake one-way latency in ms (m_netFakeLatencyMs, 0 when off)
    if (m_netGraphVisible && m_netRole == NetRole::CLIENT) {
        // Detailed net-graph (D6 + net-diagnostics): real measured bandwidth / loss / snapshot
        // rate / baseline age so the M12 target (<=25 KB/s @ 60 Hz delta) can be verified with
        // the M14 fake-loss harness. The displayed values must stay in sync with what
        // Net::getMetrics() computes — see net_metrics.h. Four right-aligned lines, top-right.
        NetMetrics m = Net::getMetrics();
        f32 measLoss = Net::getStats(activeNetSlot()).packetLoss * 100.0f; // ENet-measured, not the injected %
        u32 bage = NetMetricsOps::baselineAgeTicks(
                       static_cast<u32>(m_clockSync.serverTickEst), m_lastAppliedSnap.serverTick);
        char lines[4][96];
        snprintf(lines[0], sizeof(lines[0]), "NET rtt=%.0fms est=%.0f loss=%.1f%%",
                 m_clockSync.oneWayTripMs * 2.0f, m_clockSync.serverTickEst, measLoss);
        snprintf(lines[1], sizeof(lines[1]), "BW in=%.1fKB/s (wire ~%.1f) snap=%.1f evt=%.1f",
                 m.kbInTotal, m.wireKbIn, m.kbInPerSec[1], m.kbInPerSec[0]); // ch1=snap, ch0=evt
        snprintf(lines[2], sizeof(lines[2]), "SNAP rx=%.1fHz bage=%ut", m.snapsInPerSec, bage);
        snprintf(lines[3], sizeof(lines[3]), "SIM div=%u fakeloss=%u%% fakelat=%ums",
                 m_divergenceCount, static_cast<u32>(m_netFakeLossPct), m_netFakeLatencyMs);
        f32 scale = 1.0f;
        for (u32 i = 0; i < 4; i++) {
            f32 tw = FontSystem::textWidth(lines[i], scale);
            f32 x  = static_cast<f32>(sw) - tw - 8.0f;
            f32 y  = static_cast<f32>(sh) - 14.0f - static_cast<f32>(i) * 12.0f; // stack downward
            FontSystem::drawText(sw, sh, x, y, lines[i], {0.2f, 1.0f, 0.4f}, scale);
        }
    }

    // Host info overlay — shown to the SERVER role so the host can read their
    // external IP (UPnP success) or the UPnP error / LAN-only fallback message
    // back to friends without leaving the game. Same F9 toggle as the CLIENT
    // net-graph so there's a single "show me network details" key.
    if (m_netGraphVisible && m_netRole == NetRole::SERVER) {
        char hostBuf[160];
        const char* extIp  = Net::getExternalIp();
        const char* upnpErr = Net::getUpnpError();
        if (extIp && extIp[0]) {
            // UPnP worked — show the WAN-side address friends should type into Join.
            snprintf(hostBuf, sizeof(hostBuf), "HOST: friends join at %s:%u (UPnP)",
                     extIp, static_cast<u32>(DEFAULT_PORT));
        } else if (upnpErr && upnpErr[0]) {
            // UPnP didn't take — host is reachable on LAN, but the internet path needs
            // either manual port-forwarding or a Tailscale-style overlay.
            snprintf(hostBuf, sizeof(hostBuf), "HOST: LAN only (UPnP: %s)", upnpErr);
        } else {
            // Should not happen post-hostServer, but guard against an empty pair so
            // a stray F9 doesn't render garbage.
            snprintf(hostBuf, sizeof(hostBuf), "HOST: port %u",
                     static_cast<u32>(DEFAULT_PORT));
        }
        f32 scale = 1.0f;
        f32 tw = FontSystem::textWidth(hostBuf, scale);
        f32 x  = static_cast<f32>(sw) - tw - 8.0f;
        f32 y  = static_cast<f32>(sh) - 14.0f;
        Vec3 col = (extIp && extIp[0]) ? Vec3{0.5f, 1.0f, 0.5f}  // green = online-reachable
                                       : Vec3{1.0f, 0.8f, 0.4f}; // yellow = LAN-only
        FontSystem::drawText(sw, sh, x, y, hostBuf, col, scale);

        // Per-client bandwidth — the M12 read-off surface. One right-aligned line per connected
        // remote slot, stacked below the host-IP line. Values mirror getMetricsForSlot()
        // (net_metrics.h): payload + estimated-wire out-KB/s, measured loss, delta ratio,
        // baseline age. Read these while the M14 fake-loss harness injects loss to verify M12.
        const NetPlayerSlot* slots = Net::getSlots();
        u32 rowsDrawn = 0;
        for (u32 slot = 0; slot < MAX_PLAYERS; slot++) {
            if (slot == static_cast<u32>(m_localPlayerIndex)) continue;       // host has no remote peer
            if (!slots || slots[slot].state != SlotState::ACTIVE) continue;
            NetMetrics cm = Net::getMetricsForSlot(static_cast<u8>(slot));
            f32 cliLoss   = Net::getStats(static_cast<u8>(slot)).packetLoss * 100.0f;
            u32 bage      = NetMetricsOps::baselineAgeTicks(m_serverTick, m_clientAckedSnap[slot]);
            char cliBuf[128];
            snprintf(cliBuf, sizeof(cliBuf),
                     "CLI s%u: out=%.1fKB/s (wire ~%.1f) loss=%.1f%% delta=%.0f%% bage=%ut",
                     slot, cm.kbOutTotal, cm.wireKbOut, cliLoss, cm.deltaFullRatio * 100.0f, bage);
            f32 ctw = FontSystem::textWidth(cliBuf, scale);
            f32 cx  = static_cast<f32>(sw) - ctw - 8.0f;
            f32 cy  = static_cast<f32>(sh) - 14.0f - static_cast<f32>(++rowsDrawn) * 12.0f;
            FontSystem::drawText(sw, sh, cx, cy, cliBuf, {0.5f, 1.0f, 0.5f}, scale);
        }
    }

    // Chat log — left side of screen, above the quickbar (scaled)
    {
        f32 cs = static_cast<f32>(sh) / 720.0f;
        f32 chatX = 15.0f * cs;
        f32 chatY = 100.0f * cs; // above status icons and quickbar
        f32 lineSpacing = 12.0f * cs;
        for (u32 i = 0; i < MAX_CHAT_LINES; i++) {
            if (m_chatLog[i].timer <= 0.0f || m_chatLog[i].text[0] == '\0') continue;
            f32 alpha = (m_chatLog[i].timer < 2.0f) ? m_chatLog[i].timer * 0.5f : 1.0f;
            Vec3 col = m_chatLog[i].color * alpha;
            f32 lineY = chatY + static_cast<f32>(i) * lineSpacing;
            FontSystem::drawText(sw, sh, chatX, lineY, m_chatLog[i].text, col, 1);
        }
    }
}

// renderMenu() and renderLobby() moved to engine_render_menus.cpp
