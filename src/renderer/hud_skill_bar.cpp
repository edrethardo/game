// hud_skill_bar.cpp — HUD skill bar drawing: class skill slots, equipment skill
// slots, radial cooldown overlay, and skill cooldown indicator. Part of the HUD
// namespace split from hud.cpp. Calls pushLine/pushQuad/flushHUD via hud_internal.h.

#include "renderer/hud.h"
#include "renderer/hud_internal.h"
#include "renderer/font.h"
#include "renderer/skill_icons_data.h"
#include "platform/input.h"
#include "game/item.h"
#include <cmath>
#include <cstdio>

// ---------------------------------------------------------------------------
// drawRadialCooldown — pie sweep from 12 o'clock, clockwise.
// Uses line-fan triangulation: each of STEPS angular wedges is filled by
// 5 lines from center to the arc edge, giving a dense fill at any slot size.
// ---------------------------------------------------------------------------
void HUD::drawRadialCooldown(f32 cx, f32 cy, f32 radius, f32 fraction, Vec3 color) {
    if (fraction <= 0.0f) return;
    if (fraction > 1.0f) fraction = 1.0f;
    constexpr u32 STEPS = 24;
    f32 endAngle = fraction * 6.28318f;
    for (u32 i = 0; i < STEPS; i++) {
        f32 a0 = (static_cast<f32>(i) / STEPS) * 6.28318f;
        if (a0 >= endAngle) break;
        f32 a1 = (static_cast<f32>(i + 1) / STEPS) * 6.28318f;
        if (a1 > endAngle) a1 = endAngle;
        // 0 = 12 o'clock (up), clockwise. Offset by -PI/2 so 0=up
        f32 r0 = a0 - 1.5708f, r1 = a1 - 1.5708f;
        f32 ex0 = cx + cosf(r0) * radius, ey0 = cy + sinf(r0) * radius;
        f32 ex1 = cx + cosf(r1) * radius, ey1 = cy + sinf(r1) * radius;
        // Fill the wedge triangle (center → arc edge0 → arc edge1) with 5 lines
        for (u32 f = 0; f <= 4; f++) {
            f32 t  = static_cast<f32>(f) / 4.0f;
            f32 px = ex0 + (ex1 - ex0) * t;
            f32 py = ey0 + (ey1 - ey0) * t;
            pushLine(cx, cy, px, py, color);
        }
    }
}

// 8x8 pixel-art icons for legendary equipment skills.
// 0=transparent, 1=primary, 2=secondary, 3=detail, 4=highlight
static const u8 kIconFrozenOrb[8][8] = {
    {0,0,0,2,2,0,0,0},
    {0,0,2,4,4,2,0,0},
    {0,2,4,1,1,4,2,0},
    {2,4,1,1,1,1,4,2},
    {2,4,1,1,1,1,4,2},
    {0,2,4,1,1,4,2,0},
    {0,0,2,4,4,2,0,0},
    {0,0,0,2,2,0,0,0},
};
static const u8 kIconChainLightning[8][8] = {
    {0,0,0,0,4,0,0,0},
    {0,0,0,4,1,0,0,0},
    {0,0,4,1,0,0,0,0},
    {0,0,1,4,4,0,0,0},
    {0,0,0,0,1,4,0,0},
    {0,0,0,4,1,0,0,0},
    {0,0,4,1,0,0,0,0},
    {0,0,1,0,0,0,0,0},
};
static const u8 kIconMeteorStrike[8][8] = {
    {0,0,0,0,0,3,4,0},
    {0,0,0,0,3,4,3,0},
    {0,0,0,3,4,3,0,0},
    {0,0,3,4,3,0,0,0},
    {0,0,1,2,0,0,0,0},
    {0,1,2,2,1,0,0,0},
    {1,2,4,4,2,1,0,0},
    {0,1,2,2,1,0,0,0},
};
static const u8 kIconBloodNova[8][8] = {
    {0,0,0,4,4,0,0,0},
    {0,3,0,0,0,0,3,0},
    {0,0,1,0,0,1,0,0},
    {4,0,0,2,2,0,0,4},
    {4,0,0,2,2,0,0,4},
    {0,0,1,0,0,1,0,0},
    {0,3,0,0,0,0,3,0},
    {0,0,0,4,4,0,0,0},
};
static const u8 kIconPhaseDash[8][8] = {
    {0,0,0,0,0,0,0,4},
    {0,0,0,0,0,0,4,1},
    {3,0,0,0,0,4,1,0},
    {0,3,2,2,4,1,0,0},
    {0,0,3,2,4,1,0,0},
    {0,0,0,4,1,0,0,0},
    {0,0,4,1,0,0,0,0},
    {0,4,0,0,0,0,0,0},
};

// Blazing Arc: horizontal fire sweep icon
static const u8 kIconArcFire[8][8] = {
    {0,0,0,4,4,0,0,0},
    {0,0,4,1,1,4,0,0},
    {0,4,1,0,0,1,4,0},
    {4,1,0,0,0,0,1,4},
    {2,2,0,0,0,0,2,2},
    {1,4,1,0,0,1,4,1},
    {0,1,4,1,1,4,1,0},
    {0,0,1,4,4,1,0,0},
};

// Color palettes per skill icon
static void getSkillIconColors(u8 skillId, Vec3 cols[5]) {
    cols[0] = {0,0,0}; // transparent (unused)
    switch (static_cast<SkillId>(skillId)) {
        case SkillId::FROZEN_ORB:
            cols[1] = {0.5f, 0.8f, 1.0f}; cols[2] = {0.2f, 0.4f, 0.7f};
            cols[3] = {0.3f, 0.5f, 0.8f}; cols[4] = {0.9f, 0.95f, 1.0f};
            break;
        case SkillId::CHAIN_LIGHTNING:
            cols[1] = {0.4f, 0.6f, 1.0f}; cols[2] = {0.2f, 0.3f, 0.6f};
            cols[3] = {0.3f, 0.5f, 0.9f}; cols[4] = {1.0f, 1.0f, 0.6f};
            break;
        case SkillId::METEOR_STRIKE:
            cols[1] = {1.0f, 0.5f, 0.1f}; cols[2] = {0.8f, 0.3f, 0.1f};
            cols[3] = {0.6f, 0.3f, 0.1f}; cols[4] = {1.0f, 0.9f, 0.3f};
            break;
        case SkillId::BLOOD_NOVA:
            cols[1] = {0.8f, 0.1f, 0.1f}; cols[2] = {0.5f, 0.05f, 0.05f};
            cols[3] = {0.6f, 0.15f, 0.1f}; cols[4] = {1.0f, 0.3f, 0.2f};
            break;
        case SkillId::PHASE_DASH:
            cols[1] = {0.3f, 0.8f, 0.5f}; cols[2] = {0.15f, 0.5f, 0.3f};
            cols[3] = {0.2f, 0.6f, 0.4f}; cols[4] = {0.6f, 1.0f, 0.7f};
            break;
        case SkillId::ARC_FIRE:
            cols[1] = {1.0f, 0.5f, 0.1f}; cols[2] = {0.9f, 0.3f, 0.05f};
            cols[3] = {0.7f, 0.2f, 0.05f}; cols[4] = {1.0f, 0.85f, 0.2f};
            break;
        // Frenzy (glove passive) — amber gauntlet with hot speed chevrons
        case SkillId::FRENZY:
            cols[1] = {0.9f, 0.6f, 0.2f}; cols[2] = {0.7f, 0.35f, 0.1f};
            cols[3] = {0.5f, 0.3f, 0.15f}; cols[4] = {1.0f, 0.9f, 0.4f};
            break;
        // Warrior — red/steel
        case SkillId::CLEAVE: case SkillId::WAR_CRY:
        case SkillId::THUNDERCLAP: case SkillId::WHIRLWIND: case SkillId::EARTHQUAKE:
            cols[1] = {0.9f, 0.3f, 0.2f}; cols[2] = {0.6f, 0.2f, 0.1f};
            cols[3] = {0.4f, 0.15f, 0.1f}; cols[4] = {1.0f, 0.6f, 0.4f};
            break;
        // Sorcerer — fire/arcane
        case SkillId::FIREBALL:
            cols[1] = {1.0f, 0.5f, 0.1f}; cols[2] = {0.8f, 0.3f, 0.05f};
            cols[3] = {0.5f, 0.2f, 0.05f}; cols[4] = {1.0f, 0.9f, 0.3f};
            break;
        // Combat Engineer — electric blue
        case SkillId::SHOCK_BOLT: case SkillId::DEPLOY_TURRET:
        case SkillId::TESLA_COIL: case SkillId::MECH_OVERDRIVE:
            cols[1] = {0.3f, 0.6f, 1.0f}; cols[2] = {0.2f, 0.4f, 0.7f};
            cols[3] = {0.1f, 0.2f, 0.4f}; cols[4] = {0.6f, 0.9f, 1.0f};
            break;
        // Smoke Bomb — green
        case SkillId::POISON_CLOUD:
            cols[1] = {0.2f, 0.7f, 0.2f}; cols[2] = {0.1f, 0.5f, 0.1f};
            cols[3] = {0.1f, 0.3f, 0.1f}; cols[4] = {0.4f, 0.9f, 0.3f};
            break;
        // Paladin — gold/white
        case SkillId::HOLY_SMITE: case SkillId::HOLY_BOMBARDMENT:
        case SkillId::HOLY_NOVA:  case SkillId::DIVINE_JUDGMENT:
            cols[1] = {1.0f, 0.9f, 0.3f}; cols[2] = {0.8f, 0.6f, 0.1f};
            cols[3] = {0.6f, 0.5f, 0.1f}; cols[4] = {1.0f, 1.0f, 0.7f};
            break;
        // Marksman — amber/orange
        case SkillId::AIMED_SHOT: case SkillId::EXPLOSIVE_ROUND:
        case SkillId::OVERCHARGED_MAGAZINE: case SkillId::HEADSHOT:
            cols[1] = {1.0f, 0.7f, 0.2f}; cols[2] = {0.8f, 0.5f, 0.1f};
            cols[3] = {0.5f, 0.3f, 0.1f}; cols[4] = {1.0f, 0.9f, 0.5f};
            break;
        // Tinkerer — cyan/teal
        case SkillId::SWARM_DEPLOY: case SkillId::OVERCLOCK:
        case SkillId::DETONATE_SWARM: case SkillId::SWARM_QUEEN:
            cols[1] = {0.3f, 0.8f, 0.9f}; cols[2] = {0.2f, 0.5f, 0.6f};
            cols[3] = {0.1f, 0.3f, 0.4f}; cols[4] = {0.5f, 1.0f, 1.0f};
            break;
        // Rogue — purple
        case SkillId::FAN_OF_KNIVES: case SkillId::SHADOW_STEP:
        case SkillId::SHADOW_DANCE:
            cols[1] = {0.5f, 0.2f, 0.7f}; cols[2] = {0.3f, 0.1f, 0.5f};
            cols[3] = {0.2f, 0.1f, 0.3f}; cols[4] = {0.7f, 0.4f, 1.0f};
            break;
        // Ranger — green/brown
        case SkillId::VOLLEY: case SkillId::PIERCING_SHOT:
        case SkillId::BARRAGE: case SkillId::MARK_PREY:
            cols[1] = {0.4f, 0.7f, 0.2f}; cols[2] = {0.6f, 0.4f, 0.1f};
            cols[3] = {0.3f, 0.2f, 0.1f}; cols[4] = {0.6f, 0.9f, 0.3f};
            break;
        default:
            cols[1] = {0.6f, 0.6f, 0.6f}; cols[2] = {0.3f, 0.3f, 0.3f};
            cols[3] = {0.4f, 0.4f, 0.4f}; cols[4] = {0.9f, 0.9f, 0.9f};
            break;
    }
}

static const u8* getSkillIcon(u8 skillId) {
    switch (static_cast<SkillId>(skillId)) {
        // Warrior
        case SkillId::CLEAVE:              return &kIcon32_Cleave[0][0];
        case SkillId::WAR_CRY:            return &kIcon32_WarCry[0][0];
        case SkillId::THUNDERCLAP:         return &kIcon32_Thunderclap[0][0];
        case SkillId::WHIRLWIND:           return &kIcon32_Whirlwind[0][0];
        case SkillId::EARTHQUAKE:          return &kIcon32_Earthquake[0][0];
        // Sorcerer
        case SkillId::FIREBALL:            return &kIcon32_Fireball[0][0];
        // Legendary icons
        case SkillId::FROZEN_ORB:          return &kIcon32_FrozenOrb[0][0];
        case SkillId::CHAIN_LIGHTNING:     return &kIcon32_ChainLightning[0][0];
        case SkillId::METEOR_STRIKE:       return &kIcon32_MeteorStrike[0][0];
        case SkillId::BLOOD_NOVA:          return &kIcon32_BloodNova[0][0];
        case SkillId::PHASE_DASH:          return &kIcon32_PhaseDash[0][0];
        case SkillId::ARC_FIRE:            return &kIcon32_ArcFire[0][0];
        case SkillId::FRENZY:              return &kIcon32_Frenzy[0][0];
        // Combat Engineer
        case SkillId::SHOCK_BOLT:          return &kIcon32_ShockBolt[0][0];
        case SkillId::DEPLOY_TURRET:       return &kIcon32_DeployTurret[0][0];
        case SkillId::TESLA_COIL:          return &kIcon32_TeslaCoil[0][0];
        case SkillId::MECH_OVERDRIVE:      return &kIcon32_MechOverdrive[0][0];
        // Rogue (Smoke Bomb)
        case SkillId::POISON_CLOUD:        return &kIcon32_PoisonCloud[0][0];
        // Paladin
        case SkillId::HOLY_SMITE:          return &kIcon32_HolySmite[0][0];
        case SkillId::HOLY_BOMBARDMENT:    return &kIcon32_HolyBombardment[0][0];
        case SkillId::HOLY_NOVA:           return &kIcon32_HolyNova[0][0];
        case SkillId::DIVINE_JUDGMENT:     return &kIcon32_DivineJudgment[0][0];
        // Marksman
        case SkillId::AIMED_SHOT:          return &kIcon32_AimedShot[0][0];
        case SkillId::EXPLOSIVE_ROUND:     return &kIcon32_ExplosiveRound[0][0];
        case SkillId::OVERCHARGED_MAGAZINE:return &kIcon32_OverchargedMag[0][0];
        case SkillId::HEADSHOT:            return &kIcon32_Headshot[0][0];
        // Tinkerer
        case SkillId::SWARM_DEPLOY:        return &kIcon32_SwarmDeploy[0][0];
        case SkillId::OVERCLOCK:           return &kIcon32_Overclock[0][0];
        case SkillId::DETONATE_SWARM:      return &kIcon32_DetonateSwarm[0][0];
        case SkillId::SWARM_QUEEN:         return &kIcon32_SwarmQueen[0][0];
        // Rogue
        case SkillId::FAN_OF_KNIVES:       return &kIcon32_FanOfKnives[0][0];
        case SkillId::SHADOW_STEP:         return &kIcon32_ShadowStep[0][0];
        case SkillId::SHADOW_DANCE:        return &kIcon32_ShadowDance[0][0];
        // Ranger
        case SkillId::VOLLEY:              return &kIcon32_Volley[0][0];
        case SkillId::PIERCING_SHOT:       return &kIcon32_PiercingShot[0][0];
        case SkillId::BARRAGE:             return &kIcon32_Barrage[0][0];
        case SkillId::MARK_PREY:           return &kIcon32_MarkPrey[0][0];
        default:                           return nullptr;
    }
}

void HUD::drawClassSkillBar(u32 sw, u32 sh, f32 x, f32 y,
                              u8 activeSlot, u32 currentFloor,
                              const u8* unlockFloors, const u8* upgradeFloors,
                              const f32* cooldownTimers, const f32* maxCooldowns,
                              const f32* flashTimers, const u8* skillIds)
{
    f32 uiScale = static_cast<f32>(sh) / 720.0f;
    // Upgraded to 64px slots with 4px gaps for better readability
    f32 slotW = 64.0f * uiScale, slotH = 64.0f * uiScale, gap = 4.0f * uiScale;

    for (u8 s = 0; s < 4; s++) {
        f32 sx = x + s * (slotW + gap);
        bool unlocked = (currentFloor >= unlockFloors[s]);
        bool selected = (s == activeSlot);
        bool upgraded = (currentFloor >= upgradeFloors[s]);

        // Background fill
        Vec3 bgCol = unlocked ? Vec3{0.12f, 0.12f, 0.18f} : Vec3{0.06f, 0.06f, 0.08f};
        if (selected && unlocked) bgCol = {0.16f, 0.2f, 0.3f};
        for (f32 fy = 0; fy < slotH; fy += 1.0f) {
            pushLine(sx, y + fy, sx + slotW, y + fy, bgCol);
        }

        // Border — flash white when skill just became ready, else normal color
        Vec3 borderCol = selected ? Vec3{0.4f, 0.9f, 0.5f} : Vec3{0.25f, 0.25f, 0.35f};
        if (!unlocked) borderCol = {0.12f, 0.12f, 0.18f};
        if (upgraded) borderCol = {0.9f, 0.8f, 0.3f};
        if (flashTimers && flashTimers[s] > 0.0f) borderCol = {1.0f, 1.0f, 1.0f};
        pushQuad(sx, y, sx + slotW, y + slotH, borderCol);

        // 32×32 skill icon centered in slot
        if (skillIds && unlocked) {
            const u8* icon = getSkillIcon(skillIds[s]);
            if (icon) {
                Vec3 cols[5];
                getSkillIconColors(skillIds[s], cols);
                bool ready = (cooldownTimers[s] <= 0.0f);
                f32 iconX = sx + 16.0f * uiScale;
                f32 iconY = y + 16.0f * uiScale;
                f32 px = 1.0f * uiScale;
                for (u32 iy = 0; iy < 32; iy++) {
                    f32 rowY = iconY + (31 - iy) * px;
                    u32 ix = 0;
                    while (ix < 32) {
                        u8 c = icon[iy * 32 + ix];
                        if (c == 0) { ix++; continue; }
                        u32 runEnd = ix + 1;
                        while (runEnd < 32 && icon[iy * 32 + runEnd] == c) runEnd++;
                        f32 x0 = iconX + ix * px;
                        f32 x1 = iconX + runEnd * px;
                        Vec3 col = ready ? cols[c] : cols[c] * 0.4f;
                        pushLine(x0, rowY, x1, rowY, col);
                        ix = runEnd;
                    }
                }
            }
        }

        // Radial cooldown overlay — pie sweep from 12 o'clock, clockwise
        if (unlocked && cooldownTimers[s] > 0.0f) {
            f32 maxCD = (maxCooldowns && maxCooldowns[s] > 0.0f) ? maxCooldowns[s] : 1.0f;
            f32 cdFrac = cooldownTimers[s] / maxCD;
            if (cdFrac > 1.0f) cdFrac = 1.0f;
            f32 cx = sx + slotW * 0.5f;
            f32 cy = y + slotH * 0.5f;
            f32 radius = slotW * 0.45f;
            drawRadialCooldown(cx, cy, radius, cdFrac, {0.05f, 0.05f, 0.08f});
        }

        flushHUD();

        // Key symbol — show D-pad directions on controller, number keys on keyboard
        const char* skillLabel;
        if (Input::isGamepadConnected(0)) {
            static const char* dpadLabels[] = {"Up", "Rt", "Dn", "Lt"};
            skillLabel = dpadLabels[s];
        } else {
            static char numBuf[4][2] = {{'1',0}, {'2',0}, {'3',0}, {'4',0}};
            skillLabel = numBuf[s];
        }
        // Adjusted positions for the larger 64px slot
        drawKeySymbol(sw, sh, sx + 14.0f * uiScale, y + 16.0f * uiScale, skillLabel, selected && unlocked);

        // Locked text
        if (!unlocked) {
            char lockTxt[8];
            std::snprintf(lockTxt, sizeof(lockTxt), "F%u", unlockFloors[s]);
            FontSystem::drawText(sw, sh, sx + 12.0f * uiScale, y + 4.0f * uiScale, lockTxt, {0.35f, 0.25f, 0.25f}, 1);
        }
    }
}

void HUD::drawEquipSkillBar(u32 sw, u32 sh, f32 x, f32 y,
                              const EquipSkillSlot* slots, u32 slotCount)
{
    f32 uiScale = static_cast<f32>(sh) / 720.0f;
    // Upgraded to 64px slots with 4px gaps to match class skill bar
    f32 slotW = 64.0f * uiScale, slotH = 64.0f * uiScale, gap = 4.0f * uiScale;

    for (u32 s = 0; s < slotCount; s++) {
        const EquipSkillSlot& slot = slots[s];
        f32 sx = x + s * (slotW + gap);
        bool ready = (slot.cooldownTimer <= 0.0f);

        // Background fill
        Vec3 bgCol = ready ? Vec3{0.1f, 0.08f, 0.15f} : Vec3{0.06f, 0.06f, 0.08f};
        for (f32 fy = 0; fy < slotH; fy += 1.0f) {
            pushLine(sx, y + fy, sx + slotW, y + fy, bgCol);
        }

        // Border — gold for legendary feel
        Vec3 borderCol = ready ? Vec3{0.7f, 0.55f, 0.2f} : Vec3{0.3f, 0.25f, 0.15f};
        if (slot.isPassive) borderCol = ready ? Vec3{0.5f, 0.4f, 0.7f} : Vec3{0.25f, 0.2f, 0.35f};
        pushQuad(sx, y, sx + slotW, y + slotH, borderCol);

        // Radial cooldown overlay — pie sweep from 12 o'clock, clockwise
        if (slot.cooldownTimer > 0.0f) {
            f32 maxCD = (slot.maxCooldown > 0.0f) ? slot.maxCooldown : 1.0f;
            f32 cdFrac = slot.cooldownTimer / maxCD;
            if (cdFrac > 1.0f) cdFrac = 1.0f;
            f32 cx = sx + slotW * 0.5f;
            f32 cy = y + slotH * 0.5f;
            f32 radius = slotW * 0.45f;
            // Brighter overlay color so it's visible against the dark slot background
            drawRadialCooldown(cx, cy, radius, cdFrac, {0.15f, 0.12f, 0.2f});
        }

        // Draw 32x32 skill icon at 1:1 pixel scale, centered in 64px slot
        const u8* icon = getSkillIcon(slot.skillId);
        if (icon) {
            Vec3 cols[5];
            getSkillIconColors(slot.skillId, cols);
            f32 iconX = sx + 16.0f * uiScale;
            f32 iconY = y + 16.0f * uiScale;
            f32 px = 1.0f * uiScale;
            // Row-based run-length rendering: batch contiguous same-color pixels
            for (u32 iy = 0; iy < 32; iy++) {
                f32 rowY = iconY + (31 - iy) * px;
                u32 ix = 0;
                while (ix < 32) {
                    u8 c = icon[iy * 32 + ix];
                    if (c == 0) { ix++; continue; }
                    // Find run of same color
                    u32 runEnd = ix + 1;
                    while (runEnd < 32 && icon[iy * 32 + runEnd] == c) runEnd++;
                    f32 x0 = iconX + ix * px;
                    f32 x1 = iconX + runEnd * px;
                    Vec3 col = ready ? cols[c] : cols[c] * 0.4f;
                    pushLine(x0, rowY, x1, rowY, col);
                    ix = runEnd;
                }
            }
        }

        flushHUD();

        // Key label or "Auto" for passives — adjusted for larger 64px slot
        if (slot.isPassive) {
            FontSystem::drawText(sw, sh, sx + 8.0f * uiScale, y + 4.0f * uiScale, "auto",
                                 {0.5f, 0.4f, 0.7f}, 1);
        } else {
            drawKeySymbol(sw, sh, sx + 14.0f * uiScale, y - 22.0f * uiScale, slot.keyLabel, ready);
        }
    }
}

void HUD::drawSkillCooldown(u32 sw, u32 sh, f32 cooldownPct) {
    // Weapon indicator is at (sw - 120, 20) with size 100x16.
    // Place the skill cooldown square just below it.
    f32 x0 = static_cast<f32>(sw) - 120.0f;
    f32 y0 = 20.0f + 16.0f + 6.0f; // below weapon indicator

    f32 size = 16.0f;

    if (cooldownPct < 0.0f) cooldownPct = 0.0f;
    if (cooldownPct > 1.0f) cooldownPct = 1.0f;

    // Outline
    Vec3 outlineColor = (cooldownPct == 0.0f) ? Vec3{0.0f, 1.0f, 1.0f} : Vec3{0.2f, 0.2f, 0.3f};
    pushQuad(x0, y0, x0 + size, y0 + size, outlineColor);

    // Fill from bottom to top proportional to (1 - cooldownPct)
    f32 fillAmount = 1.0f - cooldownPct;
    f32 fillH = size * fillAmount;
    Vec3 fillColor = (cooldownPct == 0.0f) ? Vec3{0.0f, 1.0f, 1.0f} : Vec3{0.2f, 0.2f, 0.3f};
    for (f32 y = y0 + 2; y < y0 + 2 + fillH - 2; y += 1.0f) {
        pushLine(x0 + 2, y, x0 + size - 2, y, fillColor);
    }

    flushHUD();
}
