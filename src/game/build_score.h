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

    f32 off  = 0.0f;
    f32 def_ = def.baseHealth * 0.5f;    // armor brings defense (flat HP numbers run large)

    if (def.slot == ItemSlot::WEAPON) {
        // Weapons are scored on DPS, not damage per hit. Per-hit ranked a Heavy Crossbow (50 dmg,
        // 0.78 s) 3.5x a Rusty Dagger (14 dmg, 0.2 s) when their real output is nearly equal
        // (64 vs 70 DPS) — per-hit scoring would systematically purge fast weapons from every
        // build. The item's own damage and attack-speed rolls fold in MULTIPLICATIVELY, because
        // that is what they do to DPS in play; REF_SWING (0.5 s, a mid-roster cooldown) scales the
        // result back into the same range as the old per-hit numbers so armor/affix contributions
        // keep their relative weight.
        f32 flatDmg = 0.0f, pctDmg = 0.0f, atkSpd = 0.0f;
        for (u8 i = 0; i < item.affixCount && i < MAX_AFFIXES_PER_ITEM; i++) {
            switch (item.affixes[i].type) {
                case AffixType::DAMAGE_FLAT:      flatDmg += item.affixes[i].value; break;
                case AffixType::DAMAGE_PCT:       pctDmg  += item.affixes[i].value; break;
                case AffixType::ATTACK_SPEED_PCT: atkSpd  += item.affixes[i].value; break;
                default: affixContribution(item.affixes[i], col, off, def_); break;
            }
        }
        static constexpr f32 REF_SWING = 0.5f;
        const f32 cd  = (def.baseCooldown > 0.2f) ? def.baseCooldown : 0.2f;   // floor: no div-blowups
        const f32 dps = (def.baseDamage + flatDmg) * (1.0f + pctDmg * 0.01f)
                        * (1.0f + atkSpd * 0.01f) / cd;
        off += dps * REF_SWING;
    } else {
        // Non-weapons keep the additive model — there is no swing rate to fold into, and a glove's
        // attack-speed roll speeds up the WEAPON, a cross-slot effect a per-item score cannot see
        // (it stays a generic offense contribution in affixContribution).
        off += def.baseDamage;
        for (u8 i = 0; i < item.affixCount && i < MAX_AFFIXES_PER_ITEM; i++)
            affixContribution(item.affixes[i], col, off, def_);
    }

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
