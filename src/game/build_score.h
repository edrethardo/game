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

// --- reference constants: what a roll is actually WORTH in play ------------------------------
// The DPS fix taught the lesson: score the EFFECT, not the raw number. These references convert
// rolls into comparable units. Calibrated against the shipped tables (affixes.json ranges, class
// base HP 90-150, weapon DPS 60-70) — change them only with the numbers in hand.
static constexpr f32 REF_HP        = 150.0f; // a mid-game health pool: converts % HP and armor to HP
static constexpr f32 REF_FIGHT     = 10.0f;  // seconds a hard fight lasts: converts HP/s to HP
static constexpr f32 REF_HIT_RATE  = 2.0f;   // hits/s: converts life-on-hit to HP/s
static constexpr f32 REF_DPS       = 60.0f;  // your damage output: converts lifesteal% to HP/s
static constexpr f32 REF_SWING     = 0.5f;   // the weapon scale anchor (also used by the DPS term)
static constexpr f32 DEF_SCALE     = 0.5f;   // effective-HP -> score units (keeps off/def parity)

// Skill ("spell") DPS baseline per COLUMN: a Magic build's skills ARE its output, a blade or gun
// build casts on the side. Spell-damage and cooldown rolls multiply THIS, which is what makes them
// worth real score on a caster and modest score elsewhere.
inline f32 refCastDps(u8 col) { return (col == 0) ? 70.0f : 15.0f; }

// --- the scorer ----------------------------------------------------------------------------------
// Row weights: what "Tanky / Moderate / Glass Cannon" MEAN, numerically. The 3:1 spread is strong
// enough that a defense roll beats a damage roll on a Tanky build, without making offense worthless
// (a tank still has to kill things).
//
// EVERY row's weights sum to 4. That is load-bearing for the better-build nudge, which compares
// gear totals ACROSS cells: with Moderate at 1.5/1.5 (sum 3) the middle row was penalized by
// construction and the nudge suggested leaving Moderate on the STARTING loadout (measured: 78 vs
// 39 — a 2x artifact of the weight sums, not of the gear). Equal sums make cross-cell totals
// measure the gear's SHAPE, which is the thing the nudge is for.
inline void rowWeights(u8 row, f32& offW, f32& defW) {
    switch (row) {
        case 0:  offW = 1.0f; defW = 3.0f; break;   // Tanky
        default: offW = 2.0f; defW = 2.0f; break;   // Moderate
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

    // One pass: gather every roll into the component it actually changes.
    f32 dmgFlat = 0, dmgPct = 0, atkSpd = 0;                 // weapon DPS terms
    f32 spellFlat = 0, spellPct = 0, cdr = 0;                // skill DPS terms
    f32 hpFlat = 0, hpPct = 0, armor = 0;                    // effective-HP terms
    f32 regen = 0, loh = 0, lifesteal = 0, thorns = 0;       // sustain (defense) terms
    f32 clipPct = 0, reloadPct = 0, projSpd = 0;             // clip-cycle / projectile terms
    f32 utility = 0;
    for (u8 i = 0; i < item.affixCount && i < MAX_AFFIXES_PER_ITEM; i++) {
        const Affix& a = item.affixes[i];
        switch (a.type) {
            case AffixType::DAMAGE_FLAT:        dmgFlat  += a.value; break;
            case AffixType::DAMAGE_PCT:         dmgPct   += a.value; break;
            case AffixType::ATTACK_SPEED_PCT:   atkSpd   += a.value; break;
            case AffixType::SPELL_DAMAGE_FLAT:  spellFlat+= a.value; break;
            case AffixType::SPELL_DAMAGE_PCT:   spellPct += a.value; break;
            case AffixType::COOLDOWN_REDUCTION: cdr      += a.value; break;
            case AffixType::HEALTH_FLAT:        hpFlat   += a.value; break;
            case AffixType::HEALTH_PCT:         hpPct    += a.value; break;
            case AffixType::ARMOR:              armor    += a.value; break;
            case AffixType::HEALTH_REGEN:       regen    += a.value; break;
            case AffixType::LIFE_ON_HIT:        loh      += a.value; break;
            case AffixType::LIFESTEAL_PCT:      lifesteal+= a.value; break;
            case AffixType::THORNS_PCT:         thorns   += a.value; break;
            case AffixType::CLIP_SIZE_PCT:      clipPct  += a.value; break;
            case AffixType::RELOAD_SPEED_PCT:   reloadPct+= a.value; break;
            case AffixType::PROJECTILE_SPEED:   projSpd  += a.value; break;
            case AffixType::DAMAGE_TO_FLYING:   utility  += a.value * 2.0f; break; // situational dmg
            case AffixType::MOVE_SPEED_FLAT:
            case AffixType::ENERGY_FLAT:
            case AffixType::MANASTEAL_PCT:
            case AffixType::MANA_ON_KILL:
            case AffixType::CONE_ANGLE:         utility  += a.value; break;
            default: break;   // deprecated/unknown types contribute nothing
        }
    }

    // --- OFFENSE -----------------------------------------------------------------------------
    f32 off = utility * 0.25f;   // utility speeds fights up, but never beats a real damage roll

    if (def.slot == ItemSlot::WEAPON) {
        // Weapons are scored on SUSTAINED DPS, mirroring what getEffectiveWeapon actually computes
        // (per-hit ranked a Heavy Crossbow 3.5x a Rusty Dagger whose real DPS is HIGHER):
        //   * cooldown is divided by attack speed AND reduced by CDR — the engine applies CDR to
        //     the weapon swing, not just skills, so a CDR roll is melee/ranged DPS too;
        //   * CLIP weapons (guns) pay the reload cycle: shots*cd + reload per magazine — a Pistol's
        //     sustained output is ~29% below its burst, and reload%/clip% rolls buy that tax back;
        //   * PROJECTILE weapons get a hit-reliability credit from projectile-speed rolls (a faster
        //     shot lands more; heuristic 0.4x the percent — this one cannot be derived, only tuned).
        const f32 cdBase = (def.baseCooldown > 0.05f) ? def.baseCooldown : 0.2f;
        const f32 cdrEffW = (cdr > 50.0f) ? 50.0f : cdr;
        const f32 effCd  = cdBase * (1.0f - cdrEffW * 0.01f) / (1.0f + atkSpd * 0.01f);
        const f32 perHit = (def.baseDamage + dmgFlat) * (1.0f + dmgPct * 0.01f);
        f32 dps;
        if (def.baseClipSize > 0) {
            const f32 shots  = static_cast<f32>(def.baseClipSize) * (1.0f + clipPct * 0.01f);
            f32 reload = def.baseReloadTime * (1.0f - reloadPct * 0.01f);
            if (def.baseReloadTime > 0.0f && reload < 0.2f) reload = 0.2f;   // engine floor
            dps = shots * perHit / (shots * effCd + reload);
        } else {
            dps = perHit / effCd;
        }
        if (def.baseProjectileSpeed > 0.0f)
            dps *= 1.0f + projSpd * 0.004f;              // +40% roll => +16% effective DPS
        off += dps * REF_SWING;
    } else {
        // A non-weapon's damage rolls accelerate the WEAPON: convert via the reference weapon DPS
        // (a +10% ring is worth 10% of ~60 DPS, not a flat "10"), attack speed likewise.
        off += def.baseDamage;
        off += dmgFlat * 0.5f;
        off += REF_DPS * (dmgPct + atkSpd) * 0.01f * REF_SWING;
        // Clip/reload/projectile rolls on a non-weapon accelerate the WEAPON (cross-slot, which a
        // per-item score cannot see exactly) — modest reference credit rather than zero.
        off += REF_DPS * (clipPct + reloadPct + projSpd) * 0.01f * REF_SWING * 0.3f;
    }

    // Skill ("spell") output, the same way: spell damage multiplies the column's cast DPS, and
    // cooldown reduction multiplies the CAST RATE (1/(1-cdr)) — Aaron's ask: CDR and spell damage
    // must be modeled as the multipliers they are, not utility dribble. Contribution is the DELTA
    // over the no-roll baseline, so an item with no spell rolls adds exactly 0 here.
    {
        const f32 base   = refCastDps(col);
        const f32 cdrEff = (cdr > 50.0f) ? 50.0f : cdr;                 // engine caps CDR anyway
        const f32 out    = (base + spellFlat * 0.5f) * (1.0f + spellPct * 0.01f)
                           / (1.0f - cdrEff * 0.01f);
        off += (out - base) * REF_SWING;
    }

    // --- DEFENSE: everything converts to EFFECTIVE HP ------------------------------------------
    // armorMitigation is armor/(armor+100) capped 80%, so the effective-HP multiplier is exactly
    // 1 + armor/100: armor A = +A% of the pool. %HP likewise. Sustain (regen / life-on-hit /
    // lifesteal) is healing over a reference fight — Aaron's call: lifesteal is TANKINESS, not
    // offense, and it now lives here (it used to count as damage, which no tank build ever felt).
    f32 eHp = def.baseHealth + hpFlat
            + REF_HP * (hpPct + armor) * 0.01f
            + REF_FIGHT * (regen + loh * REF_HIT_RATE + lifesteal * 0.01f * REF_DPS)
            + thorns;                                  // raw: reflected damage, modest by range
    f32 def_ = eHp * DEF_SCALE;

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

// --- multi-build inventory reasoning -------------------------------------------------------------
// Auto mode keeps the best gear for EVERY build cell, not just the active one — switching builds
// should find gear waiting. These helpers are the pure core of that: what is the best score this
// inventory can field for (slot, cell)? is this item the best at ANYTHING? which build could field
// the strongest total right now?

// Human names for the notification + UI ("Tanky Ranged has better gear").
inline const char* rowName(u8 cell) {
    switch (buildRow(cell)) { case 0: return "Tanky"; case 2: return "Glass Cannon"; default: return "Moderate"; }
}
inline const char* colName(u8 cell) {
    switch (buildCol(cell)) { case 0: return "Magic"; case 2: return "Ranged"; default: return "Melee"; }
}

// Best score this inventory can field for (slot, cell), across the WORN piece and every backpack
// item of that slot. excludeBackpackIdx lets a bag item ask "what is the best WITHOUT me?" — the
// self-exclusion the dominance test needs.
inline f32 bestSlotScore(const PlayerInventory& inv, const ItemDef* defs, u32 defCount,
                         ItemSlot slot, u8 cell, s32 excludeBackpackIdx = -1) {
    f32 best = 0.0f;
    const ItemInstance& worn = inv.equipped[static_cast<u32>(slot)];
    if (worn.defId != 0xFFFF && worn.defId < defCount)
        best = score(worn, defs[worn.defId], cell);
    for (u8 bi = 0; bi < MAX_INVENTORY_ITEMS; bi++) {
        if (static_cast<s32>(bi) == excludeBackpackIdx) continue;
        const ItemInstance& it = inv.backpack[bi];
        if (it.defId == 0xFFFF || it.defId >= defCount) continue;
        if (defs[it.defId].slot != slot) continue;
        const f32 s = score(it, defs[it.defId], cell);
        if (s > best) best = s;
    }
    return best;
}

// PICKUP filter: grab a ground item only if it would be a real upgrade over everything we can
// already field, for at least ONE build cell (hysteresis included, so near-duplicates of gear we
// own stay on the ground — this is the "do not pick up worse gear" half).
inline bool worthPickingUp(const ItemInstance& cand, const ItemDef& def,
                           const PlayerInventory& inv, const ItemDef* defs, u32 defCount) {
    for (u8 cell = 0; cell < BUILD_ROWS * BUILD_COLS; cell++) {
        const f32 s = score(cand, def, cell);
        if (s <= 0.0f) continue;
        if (isUpgrade(s, bestSlotScore(inv, defs, defCount, def.slot, cell)))
            return true;
    }
    return false;
}

// PRUNE test: a bag item is a KEEPER if, for at least one build cell, nothing else we own beats it
// (>= against the best-without-me — deliberately weaker than the pickup filter, so an item we
// decided to keep is not dropped by the very next pass: asymmetry is what prevents churn).
inline bool isKeeper(const PlayerInventory& inv, const ItemDef* defs, u32 defCount, u8 backpackIdx) {
    const ItemInstance& it = inv.backpack[backpackIdx];
    if (it.defId == 0xFFFF || it.defId >= defCount) return false;
    const ItemDef& def = defs[it.defId];
    for (u8 cell = 0; cell < BUILD_ROWS * BUILD_COLS; cell++) {
        const f32 s = score(it, def, cell);
        if (s <= 0.0f) continue;
        if (s >= bestSlotScore(inv, defs, defCount, def.slot, cell, static_cast<s32>(backpackIdx)))
            return true;
    }
    return false;
}

// Total gear score a build cell could field right now (sum of best-in-slot over every slot).
inline f32 gearScoreForCell(const PlayerInventory& inv, const ItemDef* defs, u32 defCount, u8 cell) {
    f32 total = 0.0f;
    for (u32 sl = 0; sl < static_cast<u32>(ItemSlot::COUNT); sl++)
        total += bestSlotScore(inv, defs, defCount, static_cast<ItemSlot>(sl), cell);
    return total;
}

// The build cell that could field the strongest total. outScore gets its total.
inline u8 bestBuildCell(const PlayerInventory& inv, const ItemDef* defs, u32 defCount, f32& outScore) {
    u8 best = 0; outScore = -1.0f;
    for (u8 cell = 0; cell < BUILD_ROWS * BUILD_COLS; cell++) {
        const f32 s = gearScoreForCell(inv, defs, defCount, cell);
        if (s > outScore) { outScore = s; best = cell; }
    }
    return best;
}

// An item's best score over all nine cells — the eviction metric when the bag genuinely overflows
// with keepers: the item least useful to ANY build goes first.
inline f32 maxCellScore(const ItemInstance& it, const ItemDef& def) {
    f32 best = 0.0f;
    for (u8 cell = 0; cell < BUILD_ROWS * BUILD_COLS; cell++) {
        const f32 s = score(it, def, cell);
        if (s > best) best = s;
    }
    return best;
}

// A better build exists when some other cell's achievable total beats the current one by this
// factor — 10%, comfortably past scoring noise, so the nudge only fires when switching would
// actually matter.
static constexpr f32 BUILD_SUGGEST_FACTOR = 1.10f;

} // namespace BuildScore
