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
#include "renderer/hud_cooldown_util.h"
#include "renderer/minimap.h"
#include "renderer/font.h"
#include "renderer/item_icons.h"
#include "renderer/material.h"
#include "renderer/obj_loader.h"
#include "world/level_gen.h"
#include "world/level_mesh.h"
#include "world/level_loader.h"
#include "world/collision.h"
#include "world/combat_query.h"  // rayVsAABB — target-under-crosshair
#include "world/raycast.h"       // grid raycast — line-of-sight gate for the target bar
#include "game/player.h"
#include "game/combat.h"
#include "game/enemy_ai.h"
#include "game/squad.h"
#include "game/limb_system.h"
#include "game/projectile.h"
#include "game/item.h"
#include "game/shrine.h"
#include "game/champion.h"  // champion name + tint for the target bar
#include "game/skill.h"
#include "game/inventory_ui.h"
#include "game/game_constants.h"
#include "net/net.h"
#include "net/server.h"
#include "net/client.h"
#include "platform/steam.h"   // Steam::currentLobbyId — show the host's "Close Lobby" pause row when applicable
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
// inventoryComparisonActive — true while the inventory's item-comparison view is on screen: the
// inventory branch is the one being rendered AND the mouse (or the pad cursor, mapped through the
// same inventoryCursorToMouse the interaction path uses) is parked on a non-empty backpack cell,
// which is exactly the condition under which drawInventoryScreen lays its two side-by-side
// tooltips across the bottom action cluster. The skill bars and the quickbar hide for that frame
// (see renderInventoryHUD and renderHUD's quickbar block) instead of half-showing through the gap
// between the tooltips.
// ---------------------------------------------------------------------------
bool Engine::inventoryComparisonActive(u32 sw, u32 sh) const {
    // Same priority chain as renderHUD's branch selection: if the pause-quit overlay or the
    // character-inspect screen is covering the inventory, no comparison is visible.
    if (m_menu.confirmQuit || m_characterScreenOpen || !m_inventoryOpen) return false;

    s32 mx, my;
    Input::getMousePosition(mx, my);
    my = static_cast<s32>(sh) - my;   // flip to HUD coords (Y up)
    if (Input::isGamepadConnected(0)) {
        inventoryCursorToMouse(sw, sh, mx, my);
    }
    const InventoryUI::SlotHit hit = InventoryUI::hitTest(sw, sh, mx, my);
    return hit.panel == InventoryUI::SlotHit::BACKPACK &&
           !isItemEmpty(m_inventories[m_localPlayerIndex].backpack[hit.index]);
}

// ---------------------------------------------------------------------------
// renderInventoryHUD — the entire m_inventoryOpen branch:
// controller cursor, drawInventoryScreen, drag icon, button hints, equip tutorial.
// ---------------------------------------------------------------------------
void Engine::renderInventoryHUD(u32 sw, u32 sh) {
    // Inventory screen replaces normal HUD elements
    s32 invMX, invMY;
    Input::getMousePosition(invMX, invMY);
    invMY = static_cast<s32>(sh) - invMY; // flip to HUD coords

    // On a controller, drive the cursor from the D-pad selection instead of the physical mouse — the
    // hover tooltip (items AND skills) then follows the selection with no second code path.
    // Shared with updateInventoryInteraction; this used to be a second copy of the same math.
    if (Input::isGamepadConnected(0)) {
        inventoryCursorToMouse(sw, sh, invMX, invMY);
    }

    // --- Skill bars ---------------------------------------------------------------------------
    // Drawn BEFORE drawInventoryScreen on purpose. HUD primitives are batched in submission order,
    // so anything drawn later paints on top — which is how the single equip-slot tooltip (drawn
    // last, inside drawInventoryScreen) paints over these bars when they graze each other.
    //
    // While the item COMPARISON is up (backpack hover) the bars are hidden outright instead: the
    // two side-by-side tooltips land exactly on the bars' screen area, and bars peeking through
    // the gap between the frames read as clutter while the player is reading items, not skills.
    //
    // Position is the in-game anchor (shared layout), so the bars don't move when you open the Tab
    // screen. The quickbar is still drawn during inventory (renderHUD's common tail — hidden there
    // under the same comparison predicate) and sits to the RIGHT of this, so nothing collides.
    if (!inventoryComparisonActive(sw, sh)) {
        HUD::EquipSkillSlot equipSlots[MAX_EQUIP_SKILL_SLOTS];
        ItemSlot            equipSources[MAX_EQUIP_SKILL_SLOTS];   // which slot each skill came from
        const u32 equipCount = buildEquipSkillSlots(equipSlots, equipSources);
        const auto sb = InventoryUI::skillBarLayout(sw, sh, equipCount);
        renderInventorySkillBars(sw, sh, sb, equipSlots, equipCount, equipSources, invMX, invMY);
    }

    // Pass controller cursor selection for highlight rendering.
    // 0xFF = "no item selected": when the controller cursor is parked on a skill bar, neither item
    // panel may paint a highlight. (Note the pre-existing quirk this also sidesteps — selEquip=false
    // with selSlot=0 always lit backpack cell 0.)
    const bool onSkillPanel = Input::isGamepadConnected(0) && m_invCursorPanel >= 2;
    u8 selSlot = (Input::isGamepadConnected(0) && !onSkillPanel) ? m_invCursorIndex : 0xFF;
    bool selEquip = Input::isGamepadConnected(0) && m_invCursorPanel == 1;
    HUD::drawInventoryScreen(sw, sh, m_inventories[m_localPlayerIndex],
                              m_itemDefs, m_skillDefs, m_skillDefCount,
                              selSlot, selEquip, invMX, invMY);

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
    if (Input::activeDeviceIsGamepad()) {
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
        // L/R now cycles four panels (backpack / equipment / class skills / equip skills), not two.
        FontSystem::drawText(sw, sh, hintX + 294.0f, hintY + 3.0f, "Panels", {0.6f, 0.6f, 0.6f}, 1);
    }

    // Equip tutorial — shown until the player equips an item (floor 1 only)
    if (m_equipTooltipShown && !m_itemEquippedOnce && m_level.currentFloor <= 1) {
        f32 alpha = 1.0f;
        bool mouseLit = (sinf(m_tutorialPulseTimer * 6.0f) > 0.0f);

        bool ep = Input::activeDeviceIsGamepad();
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
// Rightward nudge (720p px; callers scale by hs) applied to the bottom action cluster
// — class/equip skill bars + quickbar — and to the ammo readout, so they clear the potion
// flask that now occupies the old bottom-left gutter (flask spans x≈228..272 at baseline).
// This reopens an ammo gutter to the flask's right wide enough for the full "N / M"
// readout (~88px for the widest SMG clips) before the class skill bar begins.
// Single-sourced from InventoryUI so the skill-bar layout helper and the quickbar/ammo
// callers here can never disagree about where the bottom cluster sits.
static constexpr f32 kFlaskClusterShift = InventoryUI::FLASK_CLUSTER_SHIFT;

// Draw the class + equipment skill bars on the INVENTORY screen, plus the hover/selection tooltip.
//
// The bars themselves are the same widgets the in-game HUD draws, at the same anchor — the point is
// that they don't move when you open Tab. What's new here is that a slot can be interrogated:
// hovering it with the mouse (or selecting it with the D-pad, which synthesizes the same cursor
// position) pops a tooltip explaining what the skill actually does.
//
// Called BEFORE HUD::drawInventoryScreen so the item tooltips paint over these — see the call site.
void Engine::renderInventorySkillBars(u32 sw, u32 sh,
                                      const InventoryUI::SkillBarRects& sb,
                                      const HUD::EquipSkillSlot* equipSlots, u32 equipCount,
                                      const ItemSlot* equipSources,
                                      s32 mx, s32 my) {
    const ClassDef& cls = kClassDefs[static_cast<u32>(m_playerClass)];
    // Effective floor folds in difficulty, so Nightmare/Hell show everything unlocked — same rule the
    // in-game bar uses, so a skill can't look locked here and usable there.
    const u32 effectiveFloor = m_level.currentFloor + m_difficulty * 50;

    f32 cooldowns[4], maxCooldowns[4];
    u8  skillIdBytes[4];
    for (u32 s = 0; s < 4; s++) {
        cooldowns[s] = m_classSkillStates[s].cooldownTimer;
        const SkillDef* sd = SkillSystem::findSkillDef(m_skillDefs, m_skillDefCount, cls.skills[s]);
        maxCooldowns[s] = sd ? sd->cooldown : 1.0f;
        skillIdBytes[s] = static_cast<u8>(cls.skills[s]);
    }

    // flashTimers = nullptr: the green "ready" pop is combat feedback and means nothing on a paused
    // inventory screen (and its timers are private statics inside renderSkillsHUD anyway).
    HUD::drawClassSkillBar(sw, sh, sb.classX, sb.classY,
                           m_activeClassSkill, effectiveFloor,
                           cls.skillUnlockFloor, cls.skillUpgradeFloor,
                           cooldowns, maxCooldowns, nullptr, skillIdBytes);
    if (equipCount > 0) {
        HUD::drawEquipSkillBar(sw, sh, sb.equipX, sb.equipY, equipSlots, equipCount);
    }

    // --- Hover / selection -------------------------------------------------------------------
    bool isClassBar = false;
    u8   idx = 0;
    if (!InventoryUI::skillSlotAt(sb, mx, my, isClassBar, idx)) return;

    // Cursor ring on the hovered slot. Drawn here rather than by passing the index as
    // drawClassSkillBar's `activeSlot`, which means "the skill bound to right-click" — a different
    // concept that we must not clobber just to show a hover.
    const f32 hx = (isClassBar ? sb.classX : sb.equipX) + idx * (sb.slot + sb.gap);
    const f32 hy = (isClassBar ? sb.classY : sb.equipY);
    const Vec3 hoverCol = {1.0f, 0.9f, 0.4f};
    HUD::drawRectAt(sw, sh, hx - 2.0f, hy - 2.0f, sb.slot + 4.0f, 2.0f, hoverCol);              // bottom
    HUD::drawRectAt(sw, sh, hx - 2.0f, hy + sb.slot, sb.slot + 4.0f, 2.0f, hoverCol);           // top
    HUD::drawRectAt(sw, sh, hx - 2.0f, hy - 2.0f, 2.0f, sb.slot + 4.0f, hoverCol);              // left
    HUD::drawRectAt(sw, sh, hx + sb.slot, hy - 2.0f, 2.0f, sb.slot + 4.0f, hoverCol);           // right

    HUD::SkillTooltipInfo info;
    SkillId id = SkillId::NONE;
    // ItemSlot::COUNT = "not riding in an item", which is what suppresses the per-slot overrides for
    // a class skill (a Paladin CASTING Divine Judgment must not get the Phoenix Band's ring text).
    ItemSlot slot = ItemSlot::COUNT;
    char subtitle[48];

    if (isClassBar) {
        id = cls.skills[idx];
        info.unlockFloor  = cls.skillUnlockFloor[idx];
        info.upgradeFloor = cls.skillUpgradeFloor[idx];
        info.unlocked     = effectiveFloor >= info.unlockFloor;
        info.upgraded     = effectiveFloor >= info.upgradeFloor;
        std::snprintf(subtitle, sizeof(subtitle), "Class Skill - %s", cls.name);
    } else {
        id   = static_cast<SkillId>(equipSlots[idx].skillId);
        slot = equipSources[idx];
        info.unlocked = true;   // if it's on the bar, it's equipped and live
        std::snprintf(subtitle, sizeof(subtitle), "Legendary - %s",
                      slot == ItemSlot::WEAPON  ? "Weapon"  :
                      slot == ItemSlot::ARMOR   ? "Armor"   :
                      slot == ItemSlot::BOOTS   ? "Boots"   :
                      slot == ItemSlot::HELMET  ? "Helmet"  :
                      slot == ItemSlot::RING    ? "Ring"    :
                      slot == ItemSlot::GLOVES  ? "Gloves"  : "Offhand");
    }
    if (id == SkillId::NONE) return;   // an empty class slot (class has fewer than 4) — nothing to say

    info.name        = HUD::resolveSkillName(id, m_skillDefs, m_skillDefCount);
    info.description = HUD::resolveSkillDescription(id, slot, m_skillDefs, m_skillDefCount);
    info.def         = SkillSystem::findSkillDef(m_skillDefs, m_skillDefCount, id);
    info.subtitle    = subtitle;

    // Anchor above the hovered slot; drawSkillTooltip clamps itself to the screen, so a slot near the
    // left edge is handled without special-casing.
    HUD::drawSkillTooltip(sw, sh, hx, hy + sb.slot + 8.0f, info);
}

// Gather the legendary equipment skills into the equip-bar slot list, in a FIXED order (boots,
// helmet, armor aura, weapon proc, ring passive, gloves passive). Shared by the in-game HUD and the
// inventory screen so both render the same bar and a hovered slot index means the same thing in
// both — if the inventory screen rebuilt this list itself, the two could silently drift and a
// tooltip would describe the wrong skill.
//
// `out` must hold MAX_EQUIP_SKILL_SLOTS. Returns the number filled. readyFlash is left at 0; only
// the in-game HUD tracks the pop (it is combat feedback, meaningless on a paused inventory screen).
u32 Engine::buildEquipSkillSlots(HUD::EquipSkillSlot* out, ItemSlot* outSlots) const {
    u32 n = 0;
    const bool pad = Input::activeDeviceIsGamepad();

    // outSlots records WHICH equipment slot each entry came from. The tooltip needs it: a skill can
    // read differently per slot (Blood Nova is a free proc on a weapon but a health-sacrificing
    // retaliation on armor), and without the slot the skill-bar tooltip would show the generic text
    // while the item's own tooltip showed the specific one — the exact disagreement this feature is
    // built to prevent.
    auto push = [&](SkillId id, ItemSlot from, f32 cd, f32 maxCd, const char* key, bool passive) {
        if (id == SkillId::NONE || n >= MAX_EQUIP_SKILL_SLOTS) return;
        const SkillDef* sd = SkillSystem::findSkillDef(m_skillDefs, m_skillDefCount, id);
        if (outSlots) outSlots[n] = from;
        out[n++] = { static_cast<u8>(id), cd, maxCd, key, sd ? sd->name : "???", passive, 0.0f };
    };

    const SkillState& boots = m_bootSkillStates[m_localPlayerIndex];
    const SkillState& helm  = m_helmetSkillStates[m_localPlayerIndex];
    const SkillDef* bootDef = SkillSystem::findSkillDef(m_skillDefs, m_skillDefCount, boots.activeSkill);
    const SkillDef* helmDef = SkillSystem::findSkillDef(m_skillDefs, m_skillDefCount, helm.activeSkill);

    push(boots.activeSkill, ItemSlot::BOOTS,  boots.cooldownTimer, bootDef ? bootDef->cooldown : 1.0f, pad ? "L+A" : "F", false);
    push(helm.activeSkill,  ItemSlot::HELMET, helm.cooldownTimer,  helmDef ? helmDef->cooldown : 1.0f, pad ? "L+B" : "G", false);
    push(m_armorAura,  ItemSlot::ARMOR,  0.0f, 0.0f, "", true);
    push(m_weaponProc, ItemSlot::WEAPON, 0.0f, 0.0f, "", true);
    // Ring passive shares the Second Wind cooldown slot; Divine Judgment's is 45 s, the rest 60 s.
    push(m_ringPassive, ItemSlot::RING, m_localPlayer.secondWindCooldown,
         (m_ringPassive == SkillId::DIVINE_JUDGMENT) ? 45.0f : 60.0f, "", true);
    // Gloves (Frenzy): the bar shows the "you have this passive" icon; the live stack count is in the
    // status bar ("FRN"), since passive slots render "auto" and ignore the key label.
    push(m_glovesPassive, ItemSlot::GLOVES, 0.0f, 0.0f, "", true);
    return n;
}

// renderSkillsHUD — class skill bar + equip skill bar + active skill display.
// Called only in the non-inventory (normal) HUD branch.
// ---------------------------------------------------------------------------
void Engine::renderSkillsHUD(u32 sw, u32 sh) {
    // Class skill bar — 4 slots to the LEFT of the quickbar
    {
        const ClassDef& cls = kClassDefs[static_cast<u32>(m_playerClass)];
        // Anchors come from the shared, unit-tested layout (game/inventory_ui.h) instead of being
        // computed here: the inventory screen re-draws these same bars and hit-tests their slots, so
        // the geometry has to have exactly ONE definition or the tooltip lands on the wrong skill.
        const auto sb0 = InventoryUI::skillBarLayout(sw, sh, 0);
        f32 skillBarX = sb0.classX;
        f32 skillBarY = sb0.classY;

        f32 cooldowns[4];
        f32 maxCooldowns[4];
        for (u32 s = 0; s < 4; s++) {
            cooldowns[s] = m_classSkillStates[s].cooldownTimer;
            const SkillDef* sd = SkillSystem::findSkillDef(m_skillDefs, m_skillDefCount, cls.skills[s]);
            maxCooldowns[s] = sd ? sd->cooldown : 1.0f;
        }

        // Flash effect: green "ready" pop when a skill comes off cooldown. Per-player
        // (split-screen renders each local player's HUD in turn) and POP_DURATION long
        // so the pop is felt, not a one-frame blink.
        static f32 s_classSkillFlash[MAX_LOCAL_PLAYERS][4] = {};
        static f32 s_prevCooldowns[MAX_LOCAL_PLAYERS][4]   = {};
        u32 clp = m_localPlayerIndex;
        for (u8 s = 0; s < 4; s++) {
            if (s_classSkillFlash[clp][s] > 0.0f) s_classSkillFlash[clp][s] -= 1.0f / 60.0f;
            if (cooldowns[s] <= 0.0f && s_prevCooldowns[clp][s] > 0.0f) {
                s_classSkillFlash[clp][s] = HudCooldown::POP_DURATION;
            }
            s_prevCooldowns[clp][s] = cooldowns[s];
        }

        // Pass skill IDs as u8 array for icon rendering
        u8 skillIdBytes[4];
        for (u8 si = 0; si < 4; si++) skillIdBytes[si] = static_cast<u8>(cls.skills[si]);
        // Effective floor accounts for difficulty so Nightmare/Hell show all skills unlocked
        u32 effectiveFloor = m_level.currentFloor + m_difficulty * 50;
        HUD::drawClassSkillBar(sw, sh, skillBarX, skillBarY,
                                m_activeClassSkill, effectiveFloor,
                                cls.skillUnlockFloor, cls.skillUpgradeFloor,
                                cooldowns, maxCooldowns, s_classSkillFlash[clp], skillIdBytes);

        // Equipment skill bar — shows active legendary equipment skills above class bar
        {
            // Gathered by buildEquipSkillSlots so the inventory screen re-draws the IDENTICAL bar
            // (same slots, same order) instead of assembling its own and quietly drifting.
            HUD::EquipSkillSlot equipSlots[MAX_EQUIP_SKILL_SLOTS];
            u32 equipCount = buildEquipSkillSlots(equipSlots);

            // Green "ready" pop tracking for equip skills — parallels the class bar.
            // Index-keyed (equipped set is stable during combat); passives stay at 0.
            static f32 s_equipFlash[MAX_LOCAL_PLAYERS][6]  = {};
            static f32 s_prevEquipCd[MAX_LOCAL_PLAYERS][6] = {};
            u32 eqp = m_localPlayerIndex;
            for (u32 i = 0; i < equipCount; i++) {
                if (s_equipFlash[eqp][i] > 0.0f) s_equipFlash[eqp][i] -= 1.0f / 60.0f;
                if (equipSlots[i].cooldownTimer <= 0.0f && s_prevEquipCd[eqp][i] > 0.0f) {
                    s_equipFlash[eqp][i] = HudCooldown::POP_DURATION;
                }
                s_prevEquipCd[eqp][i] = equipSlots[i].cooldownTimer;
                equipSlots[i].readyFlash = s_equipFlash[eqp][i];
            }

            if (equipCount > 0) {
                // Anchor from the shared layout (InventoryUI::skillBarLayout) rather than repeating
                // the math — the inventory screen must land the bar in exactly the same place.
                const auto sb = InventoryUI::skillBarLayout(sw, sh, equipCount);
                HUD::drawEquipSkillBar(sw, sh, sb.equipX, sb.equipY, equipSlots, equipCount);
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
        if (Input::activeDeviceIsGamepad())
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
    // Minimap (top-right corner). On CLIENT the authoritative entities — including
    // server-summoned minions the local ghost never spawned — live in the interp pool, not
    // m_entities. Mirror the fog-of-war source switch (engine_update.cpp:1254) so minion
    // dots actually appear for clients instead of reading the frozen/empty ghost pool.
    const EntityPool& minimapEntities = (m_netRole == NetRole::CLIENT)
                                        ? m_renderInterp.entities : m_entities;

    // Remote co-op players → cyan dots. Source differs by role: a CLIENT reads the
    // interpolated remote-player pool (its own slot(s) are already inactive there, set by
    // interpolateRemotePlayers); the SERVER reads authoritative NetPlayers, skipping the
    // host's own local lane(s). Singleplayer leaves every slot inactive → no extra dots.
    Vec3 otherPos[MAX_PLAYERS];
    bool otherActive[MAX_PLAYERS] = {};
    if (m_netRole == NetRole::CLIENT) {
        for (u32 i = 0; i < MAX_PLAYERS; i++) {
            otherActive[i] = m_renderInterp.playerActive[i];
            otherPos[i]    = m_renderInterp.playerPositions[i];
        }
    } else if (m_netRole == NetRole::SERVER) {
        for (u32 i = 0; i < MAX_PLAYERS; i++) {
            if (i < m_splitPlayerCount) continue;      // host's own local lane(s)
            const NetPlayer& np = m_players[i];
            if (!np.active || np.isDead) continue;
            otherActive[i] = true;
            otherPos[i]    = np.position;
        }
    }

    // m_worldItems carries the shrines. On a CLIENT it is mirrored from the server's snapshot every
    // frame, so a guest's minimap shows exactly the shrines the host's does (and loses one the
    // moment anybody activates it).
    Minimap::draw(sw, sh, m_level.grid, m_localPlayer.position, m_localPlayer.yaw,
                  minimapEntities, otherPos, otherActive, MAX_PLAYERS, &m_worldItems);

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

}

// ---------------------------------------------------------------------------
// renderTutorials — all tutorial tooltip overlays (controls, shield, dodge,
// pickup, backpack-full).  Drawn over both inventory and normal HUD branches.
// ---------------------------------------------------------------------------
void Engine::renderTutorials(u32 sw, u32 sh) {
    // Shared "look here" treatment: tutorial labels render one scale step LARGER than the rest of
    // the HUD (4 vs the standard 3) and breathe between 75% and 100% brightness on the shared
    // tutorial clock. The breathe is a 0.5 Hz sine — same cadence family as the existing key-glyph
    // flash and far below any photosensitivity threshold (the hit-feedback rule bans oscillating
    // FULL-SCREEN effects; a small breathing label is the standard attention cue elsewhere in the
    // HUD, e.g. the potion "drink now" pulse).
    const f32 TUT_SCALE = 4.0f;
    const f32 breathe = 0.75f + 0.25f * sinf(m_tutorialPulseTimer * 3.0f);

    // Backpack full notification — shown centered at 70% screen height, fades out
    if (m_fullBackpackNotifyTimer > 0.0f) {
        const char* fullText = "Backpack Full!";
        f32 fullW = FontSystem::textWidth(fullText, 3);
        f32 alpha = (m_fullBackpackNotifyTimer < 0.5f) ? m_fullBackpackNotifyTimer * 2.0f : 1.0f;
        Vec3 fullColor = {1.0f * alpha, 0.25f * alpha, 0.25f * alpha};
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - fullW) * 0.5f,
                             static_cast<f32>(sh) * 0.7f, fullText, fullColor, 3);
    }

    // Exit-sealed prompt — shown when the player tries to descend with the boss alive.
    if (m_bossLockNotifyTimer > 0.0f) {
        const char* lockText = "Defeat the boss to descend!";
        f32 lockW = FontSystem::textWidth(lockText, 3);
        f32 alpha = (m_bossLockNotifyTimer < 0.5f) ? m_bossLockNotifyTimer * 2.0f : 1.0f;
        Vec3 lockColor = {1.0f * alpha, 0.55f * alpha, 0.2f * alpha};
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - lockW) * 0.5f,
                             static_cast<f32>(sh) * 0.65f, lockText, lockColor, 3);
    }

    // Floor 1 controls tutorial — LMB Attack / RMB Skill.
    // Layout is MEASURED left-to-right (glyph, label, gap, glyph, label centered as one strip) —
    // the old fixed offsets (cx-120/cx-98/cx+35) only fit scale-3 labels; scale-4 "Attack" would
    // run under the Skill glyph.
    if (m_controlsTooltipTimer > 0.0f) {
        f32 alpha = (m_controlsTooltipTimer < 1.0f)
                    ? m_controlsTooltipTimer : 1.0f;
        bool mouseLit = (sinf(m_controlsTooltipTimer * 5.0f) > 0.0f);
        f32 cx = static_cast<f32>(sw) * 0.5f;
        f32 cy = static_cast<f32>(sh) * 0.72f;

        bool cp = Input::activeDeviceIsGamepad();
        const f32 glyphW  = 26.0f;   // key/mouse glyph footprint incl. spacing to its label
        const f32 pairGap = 48.0f;   // between the Attack pair and the Skill pair
        f32 attackW = FontSystem::textWidth("Attack", TUT_SCALE);
        f32 skillW  = FontSystem::textWidth("Skill", TUT_SCALE);
        f32 x = cx - (glyphW + attackW + pairGap + glyphW + skillW) * 0.5f;

        f32 b = alpha * breathe;
        // Attack button
        if (cp) HUD::drawKeySymbol(sw, sh, x, cy, "ZR", mouseLit);
        else    HUD::drawMouseButton(sw, sh, x, cy, 0, mouseLit);
        FontSystem::drawText(sw, sh, x + glyphW, cy, "Attack",
                             {0.6f * b, 1.0f * b, 0.6f * b}, TUT_SCALE);
        x += glyphW + attackW + pairGap;
        // Skill button
        if (cp) HUD::drawKeySymbol(sw, sh, x, cy, "R", mouseLit);
        else    HUD::drawMouseButton(sw, sh, x, cy, 1, mouseLit);
        FontSystem::drawText(sw, sh, x + glyphW, cy, "Skill",
                             {0.6f * b, 0.75f * b, 1.0f * b}, TUT_SCALE);
    }

    // Shield tutorial — shown whenever a shield is equipped until the player blocks
    if (!m_shieldBlockedOnce) {
        const ItemInstance& offhand = m_inventories[m_localPlayerIndex].equipped[static_cast<u8>(ItemSlot::OFFHAND)];
        bool hasShield = !isItemEmpty(offhand) &&
                         m_itemDefs[offhand.defId].slot == ItemSlot::OFFHAND;
        if (hasShield) {
            bool keyLit = (sinf(m_tutorialPulseTimer * 6.0f) > 0.0f);
            bool cp = Input::activeDeviceIsGamepad();
            const char* text = "Block";
            f32 textW = FontSystem::textWidth(text, TUT_SCALE);
            f32 totalW = 28.0f + textW;
            f32 cx = (static_cast<f32>(sw) - totalW) * 0.5f;
            f32 cy = static_cast<f32>(sh) * 0.62f;

            HUD::drawKeySymbol(sw, sh, cx, cy, cp ? "ZL" : "Ctrl", keyLit);
            FontSystem::drawText(sw, sh, cx + 28.0f, cy + 2.0f, text,
                                 {0.65f * breathe, 0.85f * breathe, 1.0f * breathe}, TUT_SCALE);
        }
    }

    // Dodge roll tutorial — shown after shield tutorial is completed, until player dodges
    if (m_shieldBlockedOnce && !m_dodgeRolledOnce) {
        bool keyLit = (sinf(m_tutorialPulseTimer * 6.0f) > 0.0f);
        bool cp = Input::activeDeviceIsGamepad();
        const char* text = "Dodge Roll";
        f32 textW = FontSystem::textWidth(text, TUT_SCALE);
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
                             {1.0f * breathe, 0.8f * breathe, 0.35f * breathe}, TUT_SCALE);
    }

    // First pickup tutorial — shown until the player opens inventory (floor 1 only)
    if (m_firstPickupTooltipShown && !m_inventoryOpenedOnce && m_level.currentFloor <= 1) {
        bool keyLit = (sinf(m_tutorialPulseTimer * 6.0f) > 0.0f);

        const char* text = "Open Inventory";
        f32 textW = FontSystem::textWidth(text, TUT_SCALE);
        f32 totalW = 28.0f + textW;
        f32 cx = (static_cast<f32>(sw) - totalW) * 0.5f;
        f32 cy = static_cast<f32>(sh) * 0.65f;

        HUD::drawKeySymbol(sw, sh, cx, cy, Input::activeDeviceIsGamepad() ? "+" : "Tab", keyLit);
        FontSystem::drawText(sw, sh, cx + 28.0f, cy + 2.0f, text,
                             {1.0f * breathe, 0.95f * breathe, 0.55f * breathe}, TUT_SCALE);
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
    } else if (m_characterScreenOpen) {
        // Character-inspect overlay: live rotatable armored model (left) + grouped stats sheet
        // (right). The 3D model was rendered into m_inspectColorTex earlier in render(); this
        // composites it and draws the stat text. Takes priority over the inventory branch.
        renderCharacterInspect(sw, sh);
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

        // Summon portraits — top-left, stacked under the floor-text row.
        {
            f32 hs = static_cast<f32>(sh) / 720.0f;
            // Anchor the portrait stack at sh - 95*hs (below the floor text at the top-left)
            // so the first box, which grows upward from its anchor, clears that row; align
            // its left edge with the floor text at x = 20*hs.
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

        // Potion belt flask — welded to the right of the health/energy bars, where the
        // eye already sits when hurt. Per-player ready-pop tracking mirrors the skill bars.
        {
            f32 phs = static_cast<f32>(sh) / 720.0f;
            static f32 s_potionFlash[MAX_LOCAL_PLAYERS]  = {};
            static f32 s_prevPotionCd[MAX_LOCAL_PLAYERS] = {};
            u32 pp = m_localPlayerIndex;
            if (s_potionFlash[pp] > 0.0f) s_potionFlash[pp] -= 1.0f / 60.0f;
            if (m_potionCooldown <= 0.0f && s_prevPotionCd[pp] > 0.0f) {
                s_potionFlash[pp] = HudCooldown::POP_DURATION;
            }
            s_prevPotionCd[pp] = m_potionCooldown;

            f32 hpFrac = (m_localPlayer.maxHealth > 0.0f)
                       ? m_localPlayer.health / m_localPlayer.maxHealth : 1.0f;
            HUD::PotionHudState ps;
            ps.cooldownRemaining = m_potionCooldown;
            ps.maxCooldown       = GameConst::POTION_COOLDOWN;
            ps.healthFrac        = hpFrac;
            ps.readyFlash        = s_potionFlash[pp];
            ps.urgent            = HudCooldown::potionUrgent(hpFrac, m_potionCooldown,
                                                             GameConst::LOW_HP_FRACTION);
            ps.pulsePhase        = m_statsTimer;
            ps.keyLabel          = Input::activeDeviceIsGamepad() ? "B" : "Q";
            // x = health bar x0(20) + barW(200) + 8px gap; y = 10px centers the 46px cell on
            // the health+energy stack and keeps its top clear of the status-icon row above.
            HUD::drawPotionFlask(sw, sh, 228.0f * phs, 10.0f * phs, ps);
        }

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
                // Frenzy (gloves): timer drives visibility/blink, displayValue shows the stack count
                {"FRN", {1.0f, 0.7f, 0.2f}, m_localPlayer.frenzyTimer,
                    m_localPlayer.frenzyTimer > 0.0f
                        ? static_cast<f32>(m_localPlayer.frenzyStacks) : -1.0f},
                // --- Shrine buffs (rows 8/9/10 — MUST stay in this order: getStatusIcon() in
                // hud_status.cpp keys the glyph off the ROW INDEX, so reordering these silently
                // shows the wrong icon for the effect). Only one can be live at a time (there is a
                // single shrine slot), so at most one of the three ever has a timer > 0.
                // Colours are Shrine::colorOf — same red/cyan/green as the crystal and the minimap.
                {"POW", Shrine::colorOf(ShrineBuff::POWER),
                    m_localPlayer.shrineBuff == ShrineBuff::POWER    ? m_localPlayer.shrineBuffTimer : 0.0f, -1.0f},
                {"SPD", Shrine::colorOf(ShrineBuff::SPEED),
                    m_localPlayer.shrineBuff == ShrineBuff::SPEED    ? m_localPlayer.shrineBuffTimer : 0.0f, -1.0f},
                {"VIT", Shrine::colorOf(ShrineBuff::VITALITY),
                    m_localPlayer.shrineBuff == ShrineBuff::VITALITY ? m_localPlayer.shrineBuffTimer : 0.0f, -1.0f},
            };
            // Energy bar top edge is at y=52, place icons above with gap (scaled)
            f32 hs2 = static_cast<f32>(sh) / 720.0f;
            HUD::drawStatusIcons(sw, sh, 20.0f * hs2, 58.0f * hs2, statuses,
                                 sizeof(statuses) / sizeof(statuses[0]));
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
                // Sits in the gutter reopened to the right of the potion flask (which now
                // fills the flask's old x≈230 spot); the skill bar was nudged right to match.
                f32 ammoX = 280.0f * hs3;
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

    // Pause menu overlay. Every vertical offset scales by the same sh/720 factor FontSystem bakes
    // into the text (via pauseMenuLayout for the rows the hit-test shares, inline `ms` for the
    // render-only title/lobby/hint offsets) — the layout was raw pixels while the labels scaled,
    // so above 720p the text outgrew the boxes and the spacing bunched up.
    if (m_menu.confirmQuit) {
        f32 cx = static_cast<f32>(sw) * 0.5f;
        f32 cy = static_cast<f32>(sh) * 0.5f;
        const f32 ms = static_cast<f32>(sh) / 720.0f;
        const PauseMenuLayout L = pauseMenuLayout(sh);

        const char* title = "PAUSED";
        f32 titleW = FontSystem::textWidth(title, 3);
        FontSystem::drawText(sw, sh, cx - titleW * 0.5f, cy + 50.0f * ms, title, {0.9f, 0.85f, 0.7f}, 3);

        // The host's SHARE CODE. This is the only place the host can actually read it, and for a
        // PRIVATE lobby it's the only way anyone who isn't a Steam friend can get in — so it has to
        // be somewhere they can pull up mid-game to read out. Shown only while we own a live lobby.
        if (m_netRole == NetRole::SERVER && Steam::currentLobbyId() != 0 && m_lobbyCode[0]) {
            char line[64];
            std::snprintf(line, sizeof(line), "Lobby Code:  %s", m_lobbyCode);
            f32 lw = FontSystem::textWidth(line, 2);
            FontSystem::drawText(sw, sh, cx - lw * 0.5f, cy + 105.0f * ms, line, {0.4f, 1.0f, 0.6f}, 2);
            const char* share = m_menu.hostPrivate ? "Private game - share this code to let friends in"
                                                   : "Friends can join with this code, or from the browser";
            f32 sw2 = FontSystem::textWidth(share, 1);
            FontSystem::drawText(sw, sh, cx - sw2 * 0.5f, cy + 88.0f * ms, share, {0.45f, 0.5f, 0.55f}, 1);
        }

        // Option list is dynamic: the host of an open Steam lobby gets a middle "Close Lobby" row.
        // currentLobbyId()==0 for SP / ENet host / client / non-Steam builds, so it stays 2 rows there.
        // Ordering MUST match the input handler in engine_update.cpp:
        // [Continue, (Close Lobby), (Menagerie), Options, Save/Quit].
        const bool canCloseLobby = (m_netRole == NetRole::SERVER && Steam::currentLobbyId() != 0);
        const char* options[5];
        u32 optCount = 0;
        options[optCount++] = "Continue Playing";
        if (canCloseLobby) options[optCount++] = "Close Lobby";
        if (menagerieUnlocked()) options[optCount++] = "Menagerie";   // hidden until the first pet summon
        options[optCount++] = "Options";          // opens the real options screens mid-run
        options[optCount++] = "Save and Quit";
        for (u32 i = 0; i < optCount; i++) {
            f32 y = cy + L.firstRowOffset - i * L.rowStep;
            bool sel = (i == m_menu.subSelection);
            Vec3 col = sel ? Vec3{0.3f, 1.0f, 0.4f} : Vec3{0.4f, 0.4f, 0.5f};
            HUD::drawMenuOption(sw, sh, y, L.rowW, L.rowH, col, sel);
            Vec3 tc = sel ? Vec3{1, 1, 1} : Vec3{0.6f, 0.6f, 0.6f};
            f32 tw = FontSystem::textWidth(options[i], 2);
            FontSystem::drawText(sw, sh, cx - tw * 0.5f, y + 7.0f * ms, options[i], tc, 2);
        }

        const char* hint = Input::activeDeviceIsGamepad()
            ? "D-pad, A to select, B to resume"
            : "Up/Down, Enter to select, ESC to resume";
        f32 hintW = FontSystem::textWidth(hint, 1);
        // Below the LAST option row, wherever that is. The hint was authored at a fixed cy-50
        // when this menu had two rows; the Options row (and the host's Close Lobby row) grew the
        // list downward past it, so the fixed offset drew the hint straight across the bottom
        // row's box.
        f32 hintY = cy + L.firstRowOffset - static_cast<f32>(optCount - 1) * L.rowStep - 20.0f * ms;
        FontSystem::drawText(sw, sh, cx - hintW * 0.5f, hintY, hint, {0.4f, 0.4f, 0.5f}, 1);
    }

    // Enemy health bar at the top of the screen (Diablo 2 style). Also suppressed while paused —
    // see the note below on renderTutorials; the same gap let this bleed through too.
    if (!m_characterScreenOpen && !m_menu.confirmQuit)
        renderTargetBar(sw, sh);

    // Tutorial tooltips are suppressed on the character-inspect screen so they don't draw over the
    // stats sheet (the inspect overlay owns the whole screen while open), AND while paused: they
    // sit at fixed y ~= 0.62-0.72*sh, horizontally centered — the same screen region as the PAUSED
    // title and option rows (also centered). The pause branch above claims "only the pause overlay
    // is drawn, so the screen looks unremarkable at a glance", but these two calls were gated only
    // on !m_characterScreenOpen, an unrelated flag, so a live controls/shield/dodge/pickup tooltip
    // kept rendering through the pause overlay whenever one happened to be showing when ESC was hit.
    if (!m_characterScreenOpen && !m_menu.confirmQuit)
        renderTutorials(sw, sh);

    // Quickbar — always visible at bottom of screen, EXCEPT while the inventory's item comparison
    // is up: the side-by-side tooltips land on the bottom action cluster, so the bar hides for
    // that frame just like the skill bars do (see inventoryComparisonActive).
    if (!inventoryComparisonActive(sw, sh)) {
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
        // The flask-clearing nudge now lives inside InventoryUI::quickbarLayout, which drawQuickbar
        // and the inventory hit-test both read — so there is nothing to pass here.
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

    // Menagerie overlay — LAST in renderHUD so its opaque page sits above everything the
    // per-frame HUD drew (target bar, tutorials, chat); while it is open confirmQuit is false,
    // so the pause suppressions above don't cover us and draw order has to.
    if (m_menagerieOpen) renderMenagerie(sw, sh);
}

// ---------------------------------------------------------------------------
// renderMenagerie — the pet collection page (pause menu → Menagerie, hidden until the
// profile's first summon). View-only: the goblin jackpot in gold up top, then one row per
// enemies.json mini in enemy-table order (which is tier order, so the page fills roughly
// top-to-bottom as the player descends). Uncollected entries show a dark "?" and "???" —
// present-but-unknown, the collection's pull — never the pet's name (that IS the reward).
// ---------------------------------------------------------------------------
void Engine::renderMenagerie(u32 sw, u32 sh) {
    const f32 ms = static_cast<f32>(sh) / 720.0f;
    const f32 cx = static_cast<f32>(sw) * 0.5f;

    // Opaque page backdrop (full takeover, like the character inspect screen).
    HUD::drawRectAt(sw, sh, 0.0f, 0.0f, static_cast<f32>(sw), static_cast<f32>(sh),
                    {0.06f, 0.06f, 0.08f});

    const char* title = "MENAGERIE";
    f32 titleW = FontSystem::textWidth(title, 3);
    FontSystem::drawText(sw, sh, cx - titleW * 0.5f, static_cast<f32>(sh) - 46.0f * ms,
                         title, {1.0f, 0.84f, 0.25f}, 3);

    // Collect the entries: the goblin first (find its def: the one petSummon item with no
    // petEnemy binding), then every enemy that actually HAS a resolved pet item.
    u16 goblinDef = 0xFFFF;
    for (u32 i = 0; i < m_itemDefCount; i++)
        if (m_itemDefs[i].petSummon && m_itemDefs[i].petEnemyIdx == 0xFF) { goblinDef = static_cast<u16>(i); break; }

    u32 total = 0, owned = 0;
    struct Row { u16 defId; bool have; bool gold; };
    Row rows[1 + MAX_ENEMY_DEFS];
    u32 rowCount = 0;
    if (goblinDef != 0xFFFF) {
        rows[rowCount++] = { goblinDef, m_menagerieGoblin, true };
        total++; if (m_menagerieGoblin) owned++;
    }
    for (u32 e = 0; e < m_enemyDefs.count && e < 64; e++) {
        if (m_petItemForEnemy[e] == 0xFFFF) continue;
        const bool have = (m_menagerieEnemyMask & (1ull << e)) != 0;
        rows[rowCount++] = { m_petItemForEnemy[e], have, false };
        total++; if (have) owned++;
    }

    char counter[48];
    std::snprintf(counter, sizeof(counter), "Collected %u / %u", owned, total);
    f32 cw = FontSystem::textWidth(counter, 2);
    FontSystem::drawText(sw, sh, cx - cw * 0.5f, static_cast<f32>(sh) - 78.0f * ms,
                         counter, {0.7f, 0.7f, 0.75f}, 2);

    // Two-column list (39 entries → 20 rows), names in full — a name you can read is the
    // collectable. Icons come from the item-icon atlas (goblin face / paw, rarity-colored).
    const u32 perCol   = (rowCount + 1) / 2;
    const f32 colW     = static_cast<f32>(sw) * 0.42f;
    const f32 leftX    = cx - colW;
    const f32 topY     = static_cast<f32>(sh) - 108.0f * ms;
    const f32 rowStep  = (topY - 40.0f * ms) / static_cast<f32>(perCol > 0 ? perCol : 1);
    const f32 iconSize = 16.0f * ms;
    for (u32 i = 0; i < rowCount; i++) {
        const f32 x = leftX + static_cast<f32>(i / perCol) * colW;
        const f32 y = topY - static_cast<f32>(i % perCol + 1) * rowStep;
        const Row& r = rows[i];
        if (r.have) {
            ItemIconSystem::drawIcon(sw, sh, x, y, iconSize, m_itemDefs[r.defId],
                                     r.gold ? Rarity::LEGENDARY : Rarity::COMMON);
            const Vec3 nameCol = r.gold ? Vec3{1.0f, 0.84f, 0.25f} : Vec3{0.85f, 0.85f, 0.9f};
            FontSystem::drawText(sw, sh, x + iconSize + 8.0f * ms, y + 4.0f * ms,
                                 m_itemDefs[r.defId].name, nameCol, 1);
        } else {
            FontSystem::drawText(sw, sh, x + 4.0f * ms, y + 4.0f * ms, "?", {0.3f, 0.3f, 0.36f}, 2);
            FontSystem::drawText(sw, sh, x + iconSize + 8.0f * ms, y + 4.0f * ms,
                                 "???", {0.35f, 0.35f, 0.4f}, 1);
        }
    }

    const char* hint = Input::activeDeviceIsGamepad() ? "B to return" : "ESC to return";
    f32 hintW = FontSystem::textWidth(hint, 1);
    FontSystem::drawText(sw, sh, cx - hintW * 0.5f, 18.0f * ms, hint, {0.4f, 0.4f, 0.5f}, 1);
}

// renderMenu() and renderLobby() moved to engine_render_menus.cpp

// Can the local player actually SEE this point? A grid raycast from the eye; a wall closer than the
// target means no.
//
// Tests the target's CENTRE and a point near its head, and accepts if EITHER is visible. Testing the
// centre alone makes the bar flicker on and off whenever an enemy is half-behind cover or coming up
// a step — which reads as a bug rather than as occlusion. Two samples is enough to be stable without
// pretending to be a real visibility volume.
bool Engine::hasLineOfSightTo(Vec3 target) const {
    const Vec3 eye = m_localPlayer.position + Vec3{0.0f, m_localPlayer.eyeHeight, 0.0f};
    const Vec3 samples[2] = { target, target + Vec3{0.0f, 0.5f, 0.0f} };

    for (const Vec3& s : samples) {
        Vec3 d = s - eye;
        const f32 dist = length(d);
        if (dist < 0.01f) return true;              // standing inside it — trivially visible
        d = d * (1.0f / dist);
        const RayHit wall = Raycast::cast(m_level.grid, eye, d, dist);
        // A hit slightly SHORT of the target is the wall the target is standing against, not a wall
        // between us — the epsilon stops an enemy pressed against a wall from occluding itself.
        if (!wall.hit || wall.distance >= dist - 0.25f) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// renderTargetBar — the Diablo 2 enemy health bar at the top of the screen.
//
// Target preference:
//   1. the enemy you are AIMING at (a ray through its collider, with a little forgiveness);
//   2. otherwise the last enemy you HIT, held for a short linger.
// The linger is what stops the bar strobing every time the crosshair slips off a moving target
// mid-fight — which is exactly when you most want to see how much of it is left.
//
// Render-only: nothing here feeds the simulation, so it reads whichever pool the RENDERER uses
// (m_renderInterp on a client, m_entities otherwise) and never writes to it.
// ---------------------------------------------------------------------------
void Engine::renderTargetBar(u32 sw, u32 sh) {
    const EntityPool& pool = (m_netRole == NetRole::CLIENT) ? m_renderInterp.entities : m_entities;

    const Vec3 eye = m_localPlayer.position + Vec3{0.0f, m_localPlayer.eyeHeight, 0.0f};
    const Vec3 fwd = m_localPlayer.forward;

    // 1. What are we aiming at? The collider is inflated slightly so the bar is easier to summon
    //    than a shot is to land — this is an information display, not a hit test, and demanding
    //    pixel-perfect aim just to READ an enemy's health would be miserable.
    constexpr f32 AIM_RANGE   = 40.0f;
    constexpr f32 AIM_INFLATE = 0.35f;

    // LINE OF SIGHT. Without this the bar is a wallhack: sweep the crosshair across a wall and it
    // names and health-bars every enemy in the room behind it. Clip the aim ray at the first wall
    // and only accept entities in FRONT of it.
    const RayHit aimWall = Raycast::cast(m_level.grid, eye, fwd, AIM_RANGE);
    const f32    losRange = aimWall.hit ? aimWall.distance : AIM_RANGE;

    EntityHandle aimed;
    bool found = false;
    f32  bestT = losRange;
    for (u32 a = 0; a < pool.activeCount; a++) {
        const u32 i = pool.activeList[a];
        const Entity& e = pool.entities[i];
        if (e.flags & ENT_DEAD)     continue;
        if (e.flags & ENT_FRIENDLY) continue;
        if (e.enemyType == EnemyType::PROP) continue;

        AABB box = entityAABB(e);
        box.min = box.min - Vec3{AIM_INFLATE, AIM_INFLATE, AIM_INFLATE};
        box.max = box.max + Vec3{AIM_INFLATE, AIM_INFLATE, AIM_INFLATE};
        f32 t; Vec3 n;
        if (!CombatQuery::rayVsAABB(eye, fwd, box, t, n)) continue;
        if (t > bestT) continue;   // bestT starts at the WALL distance, so anything behind it is out
        bestT = t;
        aimed = EntityHandle{ static_cast<u16>(i), e.generation };
        found = true;
    }

    const f32 dt = static_cast<f32>(Clock::getDeltaSeconds());
    if (found) {
        m_targetEnt    = aimed;
        m_targetLinger = TARGET_LINGER_SEC;      // refreshed for as long as you keep it in your sights
    } else {
        // 2. Fall back to whatever we last hit. Combat records it at the single point every
        //    player-sourced hit passes through, so no damage source can forget to report itself.
        //    LOS applies here too: without it, hitting an enemy once and then stepping behind a wall
        //    would let you watch its live health through the wall — the same exploit, one step later.
        const EntityHandle lastHit = Combat::getLastHitEntity(activeNetSlot());
        if (handleValid(pool, lastHit) && hasLineOfSightTo(pool.entities[lastHit.index].position)) {
            // Only ADOPT the last-hit target if we have nothing; never let it steal focus from a
            // target we are actively aiming at (handled above by the early assignment).
            if (!handleValid(pool, m_targetEnt) || m_targetLinger <= 0.0f) {
                m_targetEnt    = lastHit;
                m_targetLinger = TARGET_LINGER_SEC;
            }
        }
        if (m_targetLinger > 0.0f) m_targetLinger -= dt;
    }

    // Whatever we ended up showing, stop showing it the moment a wall comes between us. The linger
    // keeps DRAINING rather than being cut to zero, so an enemy ducking behind a pillar fades the
    // bar out over ~0.6 s instead of blinking it off — occlusion should feel like losing sight of
    // something, not like the HUD glitching.
    if (handleValid(pool, m_targetEnt) &&
        !hasLineOfSightTo(pool.entities[m_targetEnt.index].position)) {
        if (m_targetLinger > TARGET_FADE_SEC) m_targetLinger = TARGET_FADE_SEC;
    }

    if (m_targetLinger <= 0.0f) return;
    if (!handleValid(pool, m_targetEnt)) return;
    // Indexed directly rather than via handleGet, which wants a non-const pool — this is a
    // render-only read and must not be able to mutate the world. handleValid already checked the
    // generation, so a recycled slot can't be mistaken for the old target.
    const Entity* e = &pool.entities[m_targetEnt.index];
    if ((e->flags & ENT_DEAD) || e->maxHealth <= 0.0f) return;

    // --- Name + accent ---
    char  nameBuf[64];
    char  subBuf[96];
    subBuf[0] = '\0';
    const char* name   = nullptr;
    Vec3        accent = {1.0f, 1.0f, 1.0f};

    if ((e->flags & ENT_CHAMPION) && e->champAffixes != 0) {
        // Rebuilt from the REPLICATED name index + affix mask, both pure — so the guest reads the
        // same name the host does. The accent is the same colour its body is tinted, so the name
        // and the monster reinforce each other rather than being two unrelated cues.
        Champion::formatName(nameBuf, sizeof(nameBuf), e->champNameIdx, e->champAffixes);
        name   = nameBuf;
        accent = Champion::tintFor(e->champAffixes);

        // The affix list is the ACTIONABLE half — the name is flavour, "Molten · Vampiric" is what
        // tells you not to stand next to it and not to trade hits with it.
        u32 w = 0;
        for (u8 b = 0; b < ChampAffix::COUNT; b++) {
            const u8 bit = static_cast<u8>(1u << b);
            if (!(e->champAffixes & bit)) continue;
            const char* an = Champion::affixName(bit);
            if (w > 0 && w + 3 < sizeof(subBuf)) { subBuf[w++] = ' '; subBuf[w++] = '-'; subBuf[w++] = ' '; }
            for (const char* p = an; *p && w + 1 < sizeof(subBuf); ++p) subBuf[w++] = *p;
        }
        subBuf[w] = '\0';
    } else if (e->flags & ENT_LOOT_GOBLIN) {
        name   = "Loot Goblin";
        accent = {0.45f, 0.95f, 0.45f};
    } else if (e->isBoss) {
        name   = e->nameTag ? e->nameTag : "Boss";
        accent = {1.0f, 0.35f, 0.35f};
    } else if (e->enemyDefIdx < m_enemyDefs.count) {
        // The monster's REAL authored name — "Bone Archer", "Crypt Herald", "Broodmother".
        // Looked up through the replicated def index, so the guest names it identically.
        //
        // EnemyType is deliberately NOT used: it is only the RIG. The 38 authored monsters share
        // about 16 rigs between them, so naming from it called a Bone Archer, a Bone Mage, a
        // Necromancer and a Demon Caster all "Skeleton" — which is why this index is on the wire at
        // all. Material+mesh could not disambiguate either: Crypt Spider/Broodmother and Hellforged
        // Reaver/Demon Caster are identical in both.
        name = m_enemyDefs.defs[e->enemyDefIdx].name;
    } else {
        // Not from an enemy def: a drone, a summoned add, or the kTier fallback path (which only
        // runs if enemies.json failed to load). EnemyType is all we have, and it is at least honest.
        switch (e->enemyType) {
            case EnemyType::SKELETON:        name = "Skeleton";   break;
            case EnemyType::BAT:             name = "Bat";        break;
            case EnemyType::SPIDER:          name = "Spider";     break;
            case EnemyType::MIMIC:           name = "Mimic";      break;
            case EnemyType::HELLHOUND:       name = "Hellhound";  break;
            case EnemyType::SENTINEL:        name = "Sentinel";   break;
            case EnemyType::SUCCUBUS:        name = "Succubus";   break;
            case EnemyType::PIT_FIEND:       name = "Pit Fiend";  break;
            case EnemyType::HELLFORGE_SMITH: name = "Smith";      break;
            default:                         name = "Enemy";      break;
        }
    }

    // Fade out over the last of the linger so the bar leaves gently instead of blinking off.
    f32 fade = 1.0f;
    if (m_targetLinger < TARGET_FADE_SEC) fade = m_targetLinger / TARGET_FADE_SEC;

    HUD::drawTargetBar(sw, sh, name, subBuf[0] ? subBuf : nullptr,
                       e->health / e->maxHealth, accent, fade);
}
