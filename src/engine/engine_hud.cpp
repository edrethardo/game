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
// renderHUD — all 2D HUD elements: inventory screen or normal HUD
// (health bar, crosshair, quickbar, minimap, skill bars, net stats, profiler)
// ---------------------------------------------------------------------------
void Engine::renderHUD(u32 sw, u32 sh) {
    // Set font scale based on viewport height so text scales with resolution.
    // At 720p = 1.0, at 1080p = 1.5. HUD code passes raw font sizes (1, 2, 3).
    FontSystem::setUIScale(static_cast<f32>(sh) / 720.0f);

    if (m_inventoryOpen) {
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

        // Equip tutorial — pulsing mouse left-click + "Double-click to equip"
        if (m_equipTooltipTimer > 0.0f) {
            f32 alpha = (m_equipTooltipTimer < 1.0f)
                        ? m_equipTooltipTimer : 1.0f;
            bool mouseLit = (sinf(m_equipTooltipTimer * 6.0f) > 0.0f);

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
    } else {
        Vec3 crossColor = (m_localPlayer.damageFlashTimer > 0.0f)
                        ? Vec3{1.0f, 0.3f, 0.3f}
                        : Vec3{1.0f, 1.0f, 1.0f};
        HUD::drawCrosshair(sw, sh, crossColor);

        if (m_hitMarkerTimer > 0.0f)
            HUD::drawHitMarker(sw, sh, m_hitMarkerTimer / 0.2f);

    // CS-style directional damage arcs — show where hits came from
    for (u32 i = 0; i < Player::MAX_HIT_INDICATORS; i++) {
        const auto& hi = m_localPlayer.hitIndicators[i];
        if (hi.timer <= 0.0f) continue;
        f32 alpha = fminf(hi.timer * 2.0f, 1.0f) * 0.6f;
        HUD::drawDamageDirection(sw, sh, -hi.angle, alpha);
    }

        HUD::drawHealthBar(sw, sh, m_localPlayer.health, m_localPlayer.maxHealth);

        // Summon portraits — top-left, well below floor/potion text
        {
            f32 portX = 10.0f;
            f32 portY = static_cast<f32>(sh) - 75.0f; // lower position, clear of other HUD
            f32 portH = 26.0f, gap = 3.0f;

            // Scan for summons
            Entity* combatDrone = nullptr;
            u32 swarmCount = 0;
            u32 turretCount = 0;

            for (u32 a = 0; a < m_entities.activeCount; a++) {
                u32 idx = m_entities.activeList[a];
                Entity& ent = m_entities.entities[idx];
                if (!(ent.flags & ENT_FRIENDLY)) continue;
                if (ent.flags & ENT_DEAD) continue;
                if (ent.npcClass != NpcClass::NONE) continue;

                if (ent.enemyType == EnemyType::SPIDER && ent.moveSpeed > 0.0f) {
                    combatDrone = &ent;
                } else if (ent.flags & ENT_UNTARGETABLE) {
                    swarmCount++;
                } else if (ent.moveSpeed <= 0.0f) {
                    turretCount++;
                }
            }

            u32 slot = 0;

            static const u8 matDrone  = MaterialSystem::getIdByName("icon_drone");
            static const u8 matSwarm  = MaterialSystem::getIdByName("icon_swarm");
            static const u8 matTurret = MaterialSystem::getIdByName("icon_turret");

            if (combatDrone) {
                f32 hpFrac = combatDrone->health / combatDrone->maxHealth;
                HUD::drawSummonPortrait(sw, sh, portX, portY - slot * (portH + gap),
                                         "Drone", {0.35f, 0.33f, 0.4f}, hpFrac, 1, matDrone);
                slot++;
            }
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
            };
            // Energy bar top edge is at y=52, place icons above with gap (scaled)
            f32 hs2 = static_cast<f32>(sh) / 720.0f;
            HUD::drawStatusIcons(sw, sh, 20.0f * hs2, 58.0f * hs2, statuses, 6);
        }

        // Ammo display for hitscan weapons (right side of health bar area)
        {
            WeaponState& ws = m_players[m_localPlayerIndex].weaponState;
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
                if (m_bootSkillStates[0].activeSkill != SkillId::NONE) {
                    const SkillDef* sd = SkillSystem::findSkillDef(m_skillDefs, m_skillDefCount,
                                                                     m_bootSkillStates[0].activeSkill);
                    equipSlots[equipCount++] = {
                        static_cast<u8>(m_bootSkillStates[0].activeSkill),
                        m_bootSkillStates[0].cooldownTimer, sd ? sd->cooldown : 1.0f,
                        eqPad ? "L+A" : "F", sd ? sd->name : "???", false
                    };
                }
                // Helmet (G key / L+B)
                if (m_helmetSkillStates[0].activeSkill != SkillId::NONE) {
                    const SkillDef* sd = SkillSystem::findSkillDef(m_skillDefs, m_skillDefCount,
                                                                     m_helmetSkillStates[0].activeSkill);
                    equipSlots[equipCount++] = {
                        static_cast<u8>(m_helmetSkillStates[0].activeSkill),
                        m_helmetSkillStates[0].cooldownTimer, sd ? sd->cooldown : 1.0f,
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
                Vec3 doorCol = {0.2f * doorPulse, 1.0f * doorPulse, 0.3f * doorPulse};
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
            f32 potY = static_cast<f32>(sh) - 45.0f * hs;
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

    // Backpack full notification — shown centered at 70% screen height, fades out
    if (m_fullBackpackNotifyTimer > 0.0f) {
        const char* fullText = "Backpack Full!";
        f32 fullW = FontSystem::textWidth(fullText, 2);
        f32 alpha = (m_fullBackpackNotifyTimer < 0.5f) ? m_fullBackpackNotifyTimer * 2.0f : 1.0f;
        Vec3 fullColor = {0.9f * alpha, 0.2f * alpha, 0.2f * alpha};
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - fullW) * 0.5f,
                             static_cast<f32>(sh) * 0.7f, fullText, fullColor, 2);
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

    // First pickup tutorial — pulsing Tab key + "Open Inventory" text
    if (m_firstPickupTooltipTimer > 0.0f) {
        f32 alpha = (m_firstPickupTooltipTimer < 1.0f)
                    ? m_firstPickupTooltipTimer : 1.0f;
        bool keyLit = (sinf(m_firstPickupTooltipTimer * 6.0f) > 0.0f);

        const char* text = "Open Inventory";
        f32 textW = FontSystem::textWidth(text, 3);
        f32 totalW = 28.0f + textW;
        f32 cx = (static_cast<f32>(sw) - totalW) * 0.5f;
        f32 cy = static_cast<f32>(sh) * 0.65f;

        HUD::drawKeySymbol(sw, sh, cx, cy, Input::isGamepadConnected(0) ? "+" : "Tab", keyLit);
        FontSystem::drawText(sw, sh, cx + 28.0f, cy + 2.0f, text,
                             {0.9f * alpha, 0.85f * alpha, 0.5f * alpha}, 3);
    }

    // Quickbar — always visible at bottom of screen
    {
        f32 cdPct = 0.0f;
        WeaponState& ws = m_players[m_localPlayerIndex].weaponState;
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

    // Net stats overlay in multiplayer
    if (m_netRole != NetRole::NONE) {
        u32 ping = 0;
        if (m_netRole == NetRole::CLIENT) {
            NetStats stats = Net::getStats(m_localPlayerIndex);
            ping = static_cast<u32>(stats.rttMs);
        }
        HUD::drawNetStats(sw, sh, Net::getConnectedCount(), ping,
                          m_netRole == NetRole::SERVER ? "HOST" : "CLIENT");
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

// ---------------------------------------------------------------------------
// Menu rendering (simple text-based using HUD lines)
// ---------------------------------------------------------------------------
void Engine::renderMenu() {
    u32 sw = Window::getWidth();
    u32 sh = Window::getHeight();
    f32 uiScale = static_cast<f32>(sh) / 720.0f;
    FontSystem::setUIScale(uiScale);

    // Title text
    {
        const char* title = "CURSE OF THE DUNGEON ENGINE";
        f32 titleW = FontSystem::textWidth(title, 3);
        f32 titleX = (static_cast<f32>(sw) - titleW) * 0.5f;
        f32 titleY = sh * 0.65f;
        FontSystem::drawText(sw, sh, titleX, titleY, title, {0.9f, 0.85f, 0.7f}, 3);
    }

    if (m_menu.subState == 1) {
        // Single player sub-menu — replaces main menu options
        const char* subTitle = "Single Player";
        f32 stW = FontSystem::textWidth(subTitle, 2);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - stW) * 0.5f, sh * 0.55f, subTitle, {0.2f, 0.9f, 0.2f}, 2);

        // Any populated slot means "Continue" is available
        bool hasSave = false;
        for (u32 si = 0; si < MAX_SAVE_SLOTS; si++) {
            if (m_saveSlots[si].exists) { hasSave = true; break; }
        }

        static const char* subLabels[] = {"New Game", "Continue"};
        for (u32 i = 0; i < 2; i++) {
            f32 y = sh * 0.38f + (1 - i) * 50.0f * uiScale;
            bool sel = (i == m_menu.subSelection);
            bool available = (i == 0) || hasSave;
            Vec3 col = sel ? Vec3{0.3f, 1.0f, 0.4f} : Vec3{0.15f, 0.4f, 0.2f};
            if (!available) col = {0.2f, 0.2f, 0.2f};
            HUD::drawMenuOption(sw, sh, y, 250.0f * uiScale, 35.0f * uiScale, col, sel && available);
            Vec3 tc = available ? (sel ? Vec3{1,1,1} : Vec3{0.6f,0.6f,0.6f}) : Vec3{0.35f,0.35f,0.35f};
            f32 tw = FontSystem::textWidth(subLabels[i], 2);
            FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - tw) * 0.5f, y + 10.0f * uiScale, subLabels[i], tc, 2);
        }

        const char* hint = Input::isGamepadConnected(0)
            ? "D-pad, A to confirm, B to go back"
            : "Up/Down, Enter to confirm, ESC to go back";
        f32 hintW = FontSystem::textWidth(hint, 1);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - hintW) * 0.5f, sh * 0.15f, hint, {0.4f, 0.4f, 0.5f}, 1);
    } else if (m_menu.subState == 2) {
        // Class selection screen
        const char* subTitle = "Choose Your Class";
        f32 stW = FontSystem::textWidth(subTitle, 3);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - stW) * 0.5f, sh * 0.62f,
                             subTitle, {0.9f, 0.8f, 0.3f}, 3);

        u8 classCount = static_cast<u8>(PlayerClass::CLASS_COUNT);
        f32 listTop = sh * 0.54f;
        f32 spacing = 38.0f * uiScale;

        for (u8 i = 0; i < classCount; i++) {
            const ClassDef& cls = kClassDefs[i];
            f32 y = listTop - i * spacing;
            bool sel = (i == m_menu.subSelection);

            Vec3 col = sel ? Vec3{0.3f, 1.0f, 0.4f} : Vec3{0.15f, 0.35f, 0.2f};
            HUD::drawMenuOption(sw, sh, y, 400.0f * uiScale, 32.0f * uiScale, col, sel);

            Vec3 tc = sel ? Vec3{1, 1, 1} : Vec3{0.55f, 0.55f, 0.55f};
            char label[64];
            std::snprintf(label, sizeof(label), "%s  (%.0f HP, %.0f EN)", cls.name, cls.baseHealth, cls.baseEnergy);
            f32 tw = FontSystem::textWidth(label, 2);
            FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - tw) * 0.5f, y + 9.0f * uiScale, label, tc, 2);
        }

        // Show selected class description and stats above the game title
        if (m_menu.subSelection < classCount) {
            const ClassDef& sel = kClassDefs[m_menu.subSelection];
            f32 descY = sh * 0.78f; // above title (at 0.65)

            // Description centered
            f32 descW = FontSystem::textWidth(sel.description, 2);
            FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - descW) * 0.5f, descY,
                                 sel.description, {0.7f, 0.7f, 0.8f}, 2);

            char statLine[80];
            std::snprintf(statLine, sizeof(statLine), "HP: %.0f  Speed: %.1f  Energy: %.0f  Weapon: %s",
                          sel.baseHealth, sel.baseMoveSpeed, sel.baseEnergy, sel.startingWeaponName);
            f32 statW = FontSystem::textWidth(statLine, 2);
            FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - statW) * 0.5f, descY - 24.0f * uiScale,
                                 statLine, {0.6f, 0.8f, 0.6f}, 2);
        }

        const char* hint2 = Input::isGamepadConnected(0)
            ? "D-pad to select, A to confirm, B to go back"
            : "Up/Down to select, Enter to confirm, ESC to go back";
        f32 hintW2 = FontSystem::textWidth(hint2, 2);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - hintW2) * 0.5f, sh * 0.06f, hint2, {0.4f, 0.4f, 0.5f}, 2);

        // Transient message (e.g. "save file incompatible")
        if (m_menu.msgTimer > 0.0f && m_menu.msg) {
            f32 alpha = fminf(m_menu.msgTimer, 1.0f); // fade out in last second
            f32 msgW = FontSystem::textWidth(m_menu.msg, 2);
            FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - msgW) * 0.5f, sh * 0.12f,
                                 m_menu.msg, {1.0f * alpha, 0.3f * alpha, 0.3f * alpha}, 2);
        }
    } else if (m_menu.subState == 4) {
        // Waiting for Player 2 join screen
        const char* p1Class = kClassDefs[static_cast<u32>(m_playerClasses[0])].name;
        char p1Str[64];
        std::snprintf(p1Str, sizeof(p1Str), "Player 1: %s", p1Class);
        f32 p1W = FontSystem::textWidth(p1Str, 2);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - p1W) * 0.5f, sh * 0.55f,
                             p1Str, {0.3f, 1.0f, 0.4f}, 2);

        const char* waitText = "Player 2: Press A to join co-op";
        f32 waitW = FontSystem::textWidth(waitText, 2);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - waitW) * 0.5f, sh * 0.42f,
                             waitText, {0.7f, 0.7f, 0.9f}, 2);

        bool pad = Input::isGamepadConnected(0);
        const char* soloText = pad ? "Press + to start solo" : "Press Enter to start solo";
        f32 soloW = FontSystem::textWidth(soloText, 2);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - soloW) * 0.5f, sh * 0.32f,
                             soloText, {0.5f, 0.5f, 0.6f}, 2);

        const char* backText = pad ? "B to go back" : "ESC to go back";
        f32 backW = FontSystem::textWidth(backText, 1);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - backW) * 0.5f, sh * 0.1f,
                             backText, {0.4f, 0.4f, 0.5f}, 1);
    } else if (m_menu.subState == 5) {
        // Player 2 class selection
        const char* subTitle = "Player 2: Choose Your Class";
        f32 stW = FontSystem::textWidth(subTitle, 3);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - stW) * 0.5f, sh * 0.62f,
                             subTitle, {0.3f, 0.7f, 1.0f}, 3);

        u8 classCount = static_cast<u8>(PlayerClass::CLASS_COUNT);
        f32 listTop = sh * 0.54f;
        f32 spacing = 38.0f * uiScale;
        for (u8 i = 0; i < classCount; i++) {
            const ClassDef& cls = kClassDefs[i];
            f32 y = listTop - i * spacing;
            bool sel = (i == m_menu.subSelection);
            Vec3 col = sel ? Vec3{0.3f, 0.6f, 1.0f} : Vec3{0.15f, 0.25f, 0.45f};
            HUD::drawMenuOption(sw, sh, y, 400.0f * uiScale, 32.0f * uiScale, col, sel);
            Vec3 tc = sel ? Vec3{1, 1, 1} : Vec3{0.55f, 0.55f, 0.55f};
            char label[64];
            std::snprintf(label, sizeof(label), "%s  (%.0f HP, %.0f EN)", cls.name, cls.baseHealth, cls.baseEnergy);
            f32 tw = FontSystem::textWidth(label, 2);
            FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - tw) * 0.5f, y + 9.0f * uiScale, label, tc, 2);
        }

        const char* hint = "D-pad to select, A to confirm";
        f32 hintW3 = FontSystem::textWidth(hint, 2);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - hintW3) * 0.5f, sh * 0.06f,
                             hint, {0.4f, 0.4f, 0.5f}, 2);
    // (subState 7 difficulty selection removed — difficulty is automatic per save)
    } else if (m_menu.subState == 3) {
        // Options / controls rebinding screen
        const char* subTitle = "Controls";
        f32 stW = FontSystem::textWidth(subTitle, 3);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - stW) * 0.5f, sh * 0.9f,
                             subTitle, {0.9f, 0.8f, 0.3f}, 3);

        // Column headers
        f32 colAction = sw * 0.1f;
        f32 colKey    = sw * 0.5f;
        f32 colBtn    = sw * 0.72f;
        f32 headerY   = sh * 0.82f;
        FontSystem::drawText(sw, sh, colAction, headerY, "Action",     {0.7f, 0.7f, 0.7f}, 1);
        FontSystem::drawText(sw, sh, colKey,    headerY, "Keyboard",   {0.7f, 0.7f, 0.7f}, 1);
        FontSystem::drawText(sw, sh, colBtn,    headerY, "Controller", {0.7f, 0.7f, 0.7f}, 1);

        static constexpr u32 REBIND_COUNT = static_cast<u32>(GameAction::INVENTORY) + 1;
        static constexpr u32 OPT_STICK_SENS   = REBIND_COUNT;
        static constexpr u32 OPT_GYRO_SENS    = REBIND_COUNT + 1;
        static constexpr u32 OPT_STICK_INVERT = REBIND_COUNT + 2;
        static constexpr u32 OPT_GYRO_INVERT  = REBIND_COUNT + 3;
        static constexpr u32 OPT_SPLIT_MODE   = REBIND_COUNT + 4;
        static constexpr u32 OPT_RESET        = REBIND_COUNT + 5;
        static constexpr u32 TOTAL_OPTIONS     = REBIND_COUNT + 6;

        f32 listTop = sh * 0.78f;
        f32 lineH = 22.0f * uiScale;

        u32 visibleRows = static_cast<u32>((listTop - sh * 0.1f) / lineH);
        u32 scrollOffset = 0;
        if (m_menu.subSelection >= visibleRows) scrollOffset = m_menu.subSelection - visibleRows + 1;

        for (u32 i = scrollOffset; i < TOTAL_OPTIONS && i - scrollOffset < visibleRows; i++) {
            f32 y = listTop - (i - scrollOffset) * lineH;
            bool sel = (i == m_menu.subSelection);

            if (i < REBIND_COUNT) {
                GameAction act = static_cast<GameAction>(i);
                const char* name = Input::actionName(act);
                const InputBinding& bind = Input::getBinding(act);

                // Action name
                FontSystem::drawText(sw, sh, colAction, y, name,
                    sel ? Vec3{1, 1, 0.6f} : Vec3{0.6f, 0.6f, 0.6f}, 1);

                // Keyboard binding
                const char* keyName = (bind.key >= 0) ? SDL_GetScancodeName(static_cast<SDL_Scancode>(bind.key)) : "";
                // Mouse button name
                char keyBuf[32] = "";
                if (bind.key >= 0) std::snprintf(keyBuf, sizeof(keyBuf), "%s", keyName);
                if (bind.mouseButton == MOUSE_LEFT)   std::snprintf(keyBuf, sizeof(keyBuf), "LMB");
                if (bind.mouseButton == MOUSE_RIGHT)  std::snprintf(keyBuf, sizeof(keyBuf), "RMB");
                if (bind.mouseButton == MOUSE_MIDDLE) std::snprintf(keyBuf, sizeof(keyBuf), "MMB");

                Vec3 keyCol = sel && m_menu.bindKeyboard ? Vec3{0.3f, 1.0f, 0.4f} : Vec3{0.5f, 0.5f, 0.5f};
                if (sel && m_menu.bindCapture && m_menu.bindKeyboard) keyCol = {1.0f, 0.5f, 0.2f};
                FontSystem::drawText(sw, sh, colKey, y, keyBuf[0] ? keyBuf : "-", keyCol, 1);

                // Controller binding
                char btnBuf[32] = "-";
                if (bind.button >= 0) {
                    if (bind.modifier >= 0) {
                        std::snprintf(btnBuf, sizeof(btnBuf), "%s+%s",
                            Input::buttonName(bind.modifier), Input::buttonName(bind.button));
                    } else {
                        std::snprintf(btnBuf, sizeof(btnBuf), "%s", Input::buttonName(bind.button));
                    }
                } else if (bind.axis >= 0) {
                    std::snprintf(btnBuf, sizeof(btnBuf), "%s",
                        bind.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT ? "ZR" :
                        bind.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT  ? "ZL" : "Axis");
                }
                Vec3 btnCol = sel && !m_menu.bindKeyboard ? Vec3{0.3f, 1.0f, 0.4f} : Vec3{0.5f, 0.5f, 0.5f};
                if (sel && m_menu.bindCapture && !m_menu.bindKeyboard) btnCol = {1.0f, 0.5f, 0.2f};
                FontSystem::drawText(sw, sh, colBtn, y, btnBuf, btnCol, 1);
            } else if (i == OPT_STICK_SENS) {
                char buf[48];
                std::snprintf(buf, sizeof(buf), "Stick Sensitivity: %.2f", Input::getStickSensitivity());
                FontSystem::drawText(sw, sh, colAction, y, buf,
                    sel ? Vec3{1, 1, 0.6f} : Vec3{0.6f, 0.6f, 0.6f}, 1);
                if (sel) FontSystem::drawText(sw, sh, colBtn, y, "<  Left/Right  >", {0.4f, 0.8f, 0.4f}, 1);
            } else if (i == OPT_GYRO_SENS) {
                char buf[48];
                std::snprintf(buf, sizeof(buf), "Gyro Sensitivity: %.1f", Input::getGyroSensitivity());
                FontSystem::drawText(sw, sh, colAction, y, buf,
                    sel ? Vec3{1, 1, 0.6f} : Vec3{0.6f, 0.6f, 0.6f}, 1);
                if (sel) FontSystem::drawText(sw, sh, colBtn, y, "<  Left/Right  >", {0.4f, 0.8f, 0.4f}, 1);
            } else if (i == OPT_STICK_INVERT) {
                char buf[48];
                std::snprintf(buf, sizeof(buf), "Stick Invert Y: %s", Input::getStickInvertY() ? "ON" : "OFF");
                FontSystem::drawText(sw, sh, colAction, y, buf,
                    sel ? Vec3{1, 1, 0.6f} : Vec3{0.6f, 0.6f, 0.6f}, 1);
                if (sel) FontSystem::drawText(sw, sh, colBtn, y, "Enter to toggle", {0.4f, 0.8f, 0.4f}, 1);
            } else if (i == OPT_SPLIT_MODE) {
                char buf[48];
                std::snprintf(buf, sizeof(buf), "Split Screen: %s", m_splitMode == 0 ? "Horizontal" : "Vertical");
                FontSystem::drawText(sw, sh, colAction, y, buf,
                    sel ? Vec3{1, 1, 0.6f} : Vec3{0.6f, 0.6f, 0.6f}, 1);
                if (sel) FontSystem::drawText(sw, sh, colBtn, y, "Enter to toggle", {0.4f, 0.8f, 0.4f}, 1);
            } else if (i == OPT_GYRO_INVERT) {
                char buf[48];
                std::snprintf(buf, sizeof(buf), "Gyro Invert Y: %s", Input::getGyroInvertY() ? "ON" : "OFF");
                FontSystem::drawText(sw, sh, colAction, y, buf,
                    sel ? Vec3{1, 1, 0.6f} : Vec3{0.6f, 0.6f, 0.6f}, 1);
                if (sel) FontSystem::drawText(sw, sh, colBtn, y, "Enter to toggle", {0.4f, 0.8f, 0.4f}, 1);
            } else if (i == OPT_RESET) {
                FontSystem::drawText(sw, sh, colAction, y, "Reset to Defaults",
                    sel ? Vec3{1.0f, 0.4f, 0.4f} : Vec3{0.5f, 0.3f, 0.3f}, 1);
            }
        }

        // Hint text
        const char* hint = m_menu.bindCapture
            ? "Press a key/button to rebind, ESC to cancel"
            : "Up/Down select, Left/Right adjust, Enter rebind/toggle, ESC back";
        f32 hintW = FontSystem::textWidth(hint, 1);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - hintW) * 0.5f, sh * 0.04f,
                             hint, {0.5f, 0.5f, 0.6f}, 1);
    } else if (m_menu.subState == 6) {
        // Save-slot selection screen — shown for both New Game and Continue.
        bool isContinue = (m_menu.msg && m_menu.msg[0] == 'c');
        const char* screenTitle = isContinue ? "Select Save Slot — Continue" : "Select Save Slot — New Game";
        f32 stW = FontSystem::textWidth(screenTitle, 2);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - stW) * 0.5f, sh * 0.88f,
                             screenTitle, {0.9f, 0.8f, 0.3f}, 2);

        // Scroll window: show up to ~14 slots at once
        static constexpr u32 VISIBLE = 14;
        u32 scrollOffset = 0;
        if (m_menu.subSelection >= VISIBLE)
            scrollOffset = m_menu.subSelection - VISIBLE + 1;

        f32 listTop = sh * 0.82f;
        f32 lineH   = 28.0f * uiScale;

        for (u32 i = scrollOffset; i < MAX_SAVE_SLOTS && (i - scrollOffset) < VISIBLE; i++) {
            const SaveSlotInfo& info = m_saveSlots[i];
            f32 y   = listTop - static_cast<f32>(i - scrollOffset) * lineH;
            bool sel = (i == m_menu.subSelection);

            // Background highlight for the selected row
            Vec3 bgCol = sel ? Vec3{0.3f, 1.0f, 0.4f} : Vec3{0.12f, 0.3f, 0.15f};
            HUD::drawMenuOption(sw, sh, y - 2.0f * uiScale, sw * 0.7f, lineH - 4.0f * uiScale, bgCol, sel);

            char label[80];
            if (!info.exists) {
                std::snprintf(label, sizeof(label), "Slot %2u:  Empty", i + 1);
            } else {
                // Format play time as MM:SS
                u32 totalSec = static_cast<u32>(info.totalPlayTime);
                u32 mins = totalSec / 60;
                u32 secs = totalSec % 60;

                // Build class string (one or two players)
                const char* cls1 = (info.playerClasses[0] < static_cast<u8>(PlayerClass::CLASS_COUNT))
                    ? kClassDefs[info.playerClasses[0]].name : "?";

                if (info.playerCount >= 2 && info.playerClasses[1] != 0xFF &&
                    info.playerClasses[1] < static_cast<u8>(PlayerClass::CLASS_COUNT)) {
                    const char* cls2 = kClassDefs[info.playerClasses[1]].name;
                    std::snprintf(label, sizeof(label), "Slot %2u:  Floor %2u — %s + %s — %02u:%02u",
                                  i + 1, info.floor, cls1, cls2, mins, secs);
                } else {
                    std::snprintf(label, sizeof(label), "Slot %2u:  Floor %2u — %s — %02u:%02u",
                                  i + 1, info.floor, cls1, mins, secs);
                }
            }

            Vec3 textCol;
            if (!info.exists) {
                // Empty slot: dimmer, but still selectable for new game
                textCol = isContinue ? Vec3{0.3f, 0.3f, 0.3f} : (sel ? Vec3{0.8f, 0.8f, 0.8f} : Vec3{0.45f, 0.45f, 0.45f});
            } else {
                textCol = sel ? Vec3{1, 1, 1} : Vec3{0.6f, 0.7f, 0.6f};
            }

            f32 lw = FontSystem::textWidth(label, 1);
            FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - lw) * 0.5f, y + 7.0f * uiScale, label, textCol, 1);
        }

        // Scroll indicator when list overflows
        if (MAX_SAVE_SLOTS > VISIBLE) {
            char scrollBuf[16];
            std::snprintf(scrollBuf, sizeof(scrollBuf), "%u / %u", m_menu.subSelection + 1, MAX_SAVE_SLOTS);
            FontSystem::drawText(sw, sh, sw * 0.88f, sh * 0.88f, scrollBuf, {0.5f, 0.5f, 0.6f}, 1);
        }

        const char* slotHint = Input::isGamepadConnected(0)
            ? "D-pad to select, A to confirm, B to go back"
            : "Up/Down to select, Enter to confirm, ESC to go back";
        f32 hintW2 = FontSystem::textWidth(slotHint, 1);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - hintW2) * 0.5f, sh * 0.04f,
                             slotHint, {0.4f, 0.4f, 0.5f}, 1);
    } else if (m_menu.subState == 7) {
        // Credits screen — scrolling text
        f32 cx = static_cast<f32>(sw) * 0.5f;
        f32 baseY = static_cast<f32>(sh) * 1.2f - m_menu.creditsScroll;
        f32 lineH = 20.0f * uiScale;
        f32 sectionGap = 40.0f * uiScale;

        static const struct { const char* text; f32 scale; Vec3 color; bool gap; } credits[] = {
            {"CURSE OF THE DUNGEON ENGINE", 3, {1.0f, 0.9f, 0.3f}, false},
            {"", 1, {0,0,0}, true},
            {"Developed by", 2, {0.7f, 0.7f, 0.8f}, false},
            {"Aaron (edrethardo)", 2, {1.0f, 1.0f, 1.0f}, false},
            {"", 1, {0,0,0}, true},
            {"--- Libraries ---", 2, {1.0f, 0.6f, 0.1f}, false},
            {"ENet - Lee Salzman (MIT)", 1, {0.7f, 0.7f, 0.7f}, false},
            {"SDL2 - Sam Lantinga (Zlib)", 1, {0.7f, 0.7f, 0.7f}, false},
            {"SDL_mixer - Sam Lantinga (Zlib)", 1, {0.7f, 0.7f, 0.7f}, false},
            {"nlohmann/json - Niels Lohmann (MIT)", 1, {0.7f, 0.7f, 0.7f}, false},
            {"stb_image - Sean Barrett (MIT/PD)", 1, {0.7f, 0.7f, 0.7f}, false},
            {"glad - OpenGL Loader Generator", 1, {0.7f, 0.7f, 0.7f}, false},
            {"", 1, {0,0,0}, true},
            {"--- Audio ---", 2, {1.0f, 0.6f, 0.1f}, false},
            {"Kenney.nl - RPG Audio, Impacts, UI (CC0)", 1, {0.7f, 0.7f, 0.7f}, false},
            {"OpenGameArt.org - CC0 RPG SFX,", 1, {0.7f, 0.7f, 0.7f}, false},
            {"  Retro Synth, Swishes, Thwack,", 1, {0.7f, 0.7f, 0.7f}, false},
            {"  RPG Sound Pack, Magic Spell SFX", 1, {0.7f, 0.7f, 0.7f}, false},
            {"", 1, {0,0,0}, true},
            {"Built with love, C++17, and AI", 2, {0.5f, 0.8f, 1.0f}, false},
            {"", 1, {0,0,0}, true},
            {"Press ESC to return", 1, {0.4f, 0.4f, 0.5f}, false},
        };

        f32 y = baseY;
        for (const auto& line : credits) {
            if (line.gap) { y -= sectionGap; continue; }
            if (y > -30.0f && y < static_cast<f32>(sh) + 30.0f) {
                f32 tw = FontSystem::textWidth(line.text, line.scale);
                FontSystem::drawText(sw, sh, cx - tw * 0.5f, y, line.text, line.color, line.scale);
            }
            y -= lineH * line.scale;
        }
    } else {
        // Main menu options
        static const char* labels[] = {"Single Player", "Host Game", "Join Game", "Options", "Credits", "Exit Game"};
        Vec3 colors[] = {
            {0.2f, 0.9f, 0.2f},
            {0.2f, 0.5f, 1.0f},
            {1.0f, 0.7f, 0.2f},
            {0.6f, 0.6f, 0.8f},
            {1.0f, 0.6f, 0.1f},   // orange for Credits
            {0.7f, 0.2f, 0.2f},
        };

        for (u32 i = 0; i < 6; i++) {
            f32 y = sh * 0.2f + (5 - i) * 50.0f * uiScale;
            Vec3 color = colors[i];
            bool selected = (i == m_menu.selection);
            if (!selected) color = color * 0.4f;
            HUD::drawMenuOption(sw, sh, y, 250.0f * uiScale, 35.0f * uiScale, color, selected);

            f32 textW = FontSystem::textWidth(labels[i], 2);
            f32 textX = (static_cast<f32>(sw) - textW) * 0.5f;
            FontSystem::drawText(sw, sh, textX, y + 10.0f * uiScale, labels[i],
                selected ? Vec3{1,1,1} : Vec3{0.6f,0.6f,0.6f}, 2);
        }

        const char* hint = Input::isGamepadConnected(0)
            ? "D-pad to select, A to confirm"
            : "Up/Down to select, Enter to confirm";
        f32 hintW = FontSystem::textWidth(hint, 1);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - hintW) * 0.5f, sh * 0.1f, hint, {0.4f, 0.4f, 0.5f}, 1);
    }
}

void Engine::renderLobby() {
    u32 sw = Window::getWidth();
    u32 sh = Window::getHeight();
    HUD::drawCrosshair(sw, sh, {0.3f, 0.3f, 1.0f});
}

