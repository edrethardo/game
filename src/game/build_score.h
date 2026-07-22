#pragma once
#include "core/types.h"
#include "game/item.h"
#include "game/weapon.h"

// build_score.h — the pure scoring core of Auto Loot & Equip (spec:
// docs/superpowers/specs/2026-07-22-auto-loot-equip-design.md).
//
// A build is one cell of the 3x3 grid the player picks in the inventory:
//   rows    = SURVIVAL POSTURE: 0 Tanky / 1 Moderate / 2 Glass Cannon
//   columns = DAMAGE ARCHETYPE: 0 Magic / 1 Melee / 2 Ranged
// encoded as buildCell = row*3 + col (a u8, persisted in PlayerInventory).
//
// score() reduces an item to one number under that cell so auto-equip can compare candidates. It
// is STAT-DERIVED, not authored: it reads the def's base stats and the instance's rolled affixes,
// so every item in items.json — and every future one — participates with zero tagging burden, and
// two copies of the same def score differently when their rolls differ (which is the whole point:
// a "tanky" ring that rolled pure damage IS a damage ring).
//
// Everything here is pure and header-only so dungeon_tests links it for free.

namespace BuildScore {

// --- build-cell encoding -------------------------------------------------------------------------
static constexpr u8 BUILD_ROWS = 3, BUILD_COLS = 3;
static constexpr u8 DEFAULT_BUILD_CELL = 1 * BUILD_COLS + 1;   // Moderate / Melee

inline u8 buildRow(u8 cell) { return static_cast<u8>((cell / BUILD_COLS) % BUILD_ROWS); }
inline u8 buildCol(u8 cell) { return static_cast<u8>(cell % BUILD_COLS); }

// Auto-equip only fires when the candidate beats the worn piece by this factor. Without hysteresis
// two near-equal drops in a row would swap gear back and forth (each syncing inventory to the
// server), which reads as flicker and spams the wire for no player-visible gain.
static constexpr f32 UPGRADE_FACTOR = 1.05f;

// --- weapon family gate (the column axis) --------------------------------------------------------
// Magic is exactly WAND: every staff/wand in items.json is subtype "wand" (checked against the
// shipped table — there is no separate STAFF subtype). Guns, bows and thrown weapons are Ranged;
// blades are Melee. NONE (unarmed/none rolled) belongs to no family and scores 0 as a weapon.
inline bool weaponInFamily(WeaponSubtype st, u8 col) {
    switch (st) {
        case WeaponSubtype::WAND:
            return col == 0;
        case WeaponSubtype::SWORD: case WeaponSubtype::DAGGER: case WeaponSubtype::AXE:
        case WeaponSubtype::CLAYMORE: case WeaponSubtype::CLEAVER:
            return col == 1;
        case WeaponSubtype::PISTOL: case WeaponSubtype::SMG: case WeaponSubtype::CARBINE:
        case WeaponSubtype::REVOLVER: case WeaponSubtype::BOW: case WeaponSubtype::CROSSBOW:
        case WeaponSubtype::THROWING_KNIFE: case WeaponSubtype::MOLOTOV: case WeaponSubtype::CHAKRAM:
            return col == 2;
        default:
            return false;
    }
}

// --- affix classification ------------------------------------------------------------------------
// Split every rolled affix into an offense or defense contribution (utility affixes count weakly as
// offense — clip/reload/move-speed make you kill faster in practice, but they must not outweigh a
// real damage roll). Values are already comparable magnitudes in affixes.json; the per-type factor
// levels the flat-vs-percent scale differences (a 10% damage roll ≈ a 10-point flat roll in play).
inline void affixContribution(const Affix& a, u8 col, f32& off, f32& def) {
    switch (a.type) {
        // offense
        case AffixType::DAMAGE_FLAT:       off += a.value;          break;
        case AffixType::DAMAGE_PCT:        off += a.value;          break;
        case AffixType::ATTACK_SPEED_PCT:  off += a.value * ((col != 0) ? 1.5f : 1.0f); break;
        case AffixType::SPELL_DAMAGE_FLAT: off += a.value * ((col == 0) ? 2.0f : 1.0f); break;
        case AffixType::SPELL_DAMAGE_PCT:  off += a.value * ((col == 0) ? 2.0f : 1.0f); break;
        case AffixType::DAMAGE_TO_FLYING:  off += a.value * 0.5f;   break;   // situational
        case AffixType::LIFESTEAL_PCT:     off += a.value;          break;   // scales with damage dealt
        case AffixType::LIFE_ON_HIT:       off += a.value * 0.5f;   break;
        // defense
        case AffixType::HEALTH_FLAT:       def += a.value * 0.5f;   break;   // flat HP rolls run large
        case AffixType::HEALTH_PCT:        def += a.value;          break;
        case AffixType::ARMOR:             def += a.value;          break;
        case AffixType::HEALTH_REGEN:      def += a.value * 2.0f;   break;   // per-second — small numbers
        case AffixType::THORNS_PCT:        def += a.value;          break;
        case AffixType::CC_RESIST:         def += a.value;          break;
        // utility — weak offense (they speed the fight up, but never beat a real damage roll)
        case AffixType::MOVE_SPEED_FLAT:
        case AffixType::COOLDOWN_REDUCTION:
        case AffixType::CLIP_SIZE_PCT:
        case AffixType::RELOAD_SPEED_PCT:
        case AffixType::ENERGY_FLAT:
        case AffixType::MANASTEAL_PCT:
        case AffixType::MANA_ON_KILL:
        case AffixType::PROJECTILE_SPEED:
        case AffixType::CONE_ANGLE:        off += a.value * 0.25f;  break;
        default: break;   // deprecated/unknown types contribute nothing
    }
}

// --- the scorer ----------------------------------------------------------------------------------
// Row weights: what "Tanky / Moderate / Glass Cannon" MEAN, numerically. The 3:1 spread is strong
// enough that a defense roll beats a damage roll on a Tanky build, without making offense worthless
// (a tank still has to kill things).
inline void rowWeights(u8 row, f32& offW, f32& defW) {
    switch (row) {
        case 0:  offW = 1.0f; defW = 3.0f; break;   // Tanky
        default: offW = 1.5f; defW = 1.5f; break;   // Moderate
        case 2:  offW = 3.0f; defW = 1.0f; break;   // Glass Cannon
    }
}

inline f32 score(const ItemInstance& item, const ItemDef& def, u8 cell) {
    if (item.defId == 0xFFFF) return 0.0f;                       // empty scores nothing
    const u8 row = buildRow(cell), col = buildCol(cell);

    // The family gate: a weapon outside the build's archetype is worth exactly 0, so auto-equip can
    // never wander a Magic build onto an axe however good its rolls are. Only weapons are gated —
    // armor/rings/offhands serve any archetype.
    if (def.slot == ItemSlot::WEAPON && !weaponInFamily(def.weaponSubtype, col)) return 0.0f;

    f32 off = def.baseDamage;            // weapons bring offense…
    f32 def_ = def.baseHealth * 0.5f;    // …armor brings defense (flat HP numbers run large)

    for (u8 i = 0; i < item.affixCount && i < MAX_AFFIXES_PER_ITEM; i++)
        affixContribution(item.affixes[i], col, off, def_);

    f32 offW, defW;
    rowWeights(row, offW, defW);

    // Rarity nudge: equal-stat ties resolve toward the higher rarity (and it keeps a fresh
    // legendary from being auto-dropped as "the worst item" ahead of a common with one lucky roll).
    const f32 rarityBonus = 2.0f * static_cast<f32>(item.rarity);

    return off * offW + def_ * defW + rarityBonus;
}

// True when `candidate` should replace `worn` under this build — the hysteresis rule. An empty worn
// slot is always an upgrade (score 0 * factor is 0, but make the intent explicit).
inline bool isUpgrade(f32 candidateScore, f32 wornScore) {
    if (wornScore <= 0.0f) return candidateScore > 0.0f;
    return candidateScore > wornScore * UPGRADE_FACTOR;
}

} // namespace BuildScore
