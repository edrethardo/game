#pragma once
#include "core/types.h"
#include "game/item.h"
#include "game/weapon.h"
#include "game/weapon_dps.h"

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

// --- weapon-damage skill scaling (the Marksman/Ranger case) ---------------------------------------
// Ranged-class skills scale off the weapon's PER-HIT damage — skill_marksman.cpp / skill_ranger.cpp
// cast `s_weaponDamage * 1.2 .. 2.5`, where s_weaponDamage is the equipped weapon's per-hit number.
// So between two RANGED weapons of comparable sustained DPS, the harder-hitting one is strictly
// better because it also amplifies those skills. This credits a slice of per-hit ON TOP of the DPS
// term for the Ranged column only — sized as a TIEBREAK: enough to decide a DPS tie toward the
// bigger hitter, never enough to overturn a genuine DPS gap (a ~20% DPS lead still wins outright).
// The scorer is per-CELL, not per-class, so the Ranged column is the proxy for "a class whose
// skills scale off weapon damage"; it also happens to be a mild positive for plain ranged fire
// (fewer, bigger hits waste less on overkill), so the approximation never hurts.
inline f32 weaponSkillScaleBonus(u8 col, f32 perHit) {
    return (col == 2) ? perHit * REF_SWING * 0.15f : 0.0f;
}

// --- granted-skill value (the legendary-shield / -helmet case) -------------------------------------
// A legendary item can grant an ACTIVE skill its base stats and affixes don't reflect at all
// (Thunderwall -> Chain Lightning, Aegis of Blood -> Blood Nova, a legendary helmet -> its G-rail
// skill). These two helpers reduce a SkillDef to the same currency the rest of the scorer speaks so
// the engine can precompute a per-item scalar (ItemDef::legendarySkillOffense/Defense) once at load.
//
// OFFENSE is the skill's sustained DPS-equivalent under the "30 s on a stationary dummy" lens the
// weapons already use: damage per cast times a small multi-target credit, over an effective cast
// interval floored well ABOVE the raw cooldown. The floor stands in for the energy + positioning
// gate a real fight imposes — a 0.3 s-cooldown, 25-energy Chain Lightning cannot actually fire three
// times a second for 30 s straight — without the scorer needing the energy-regen model. Discounted
// vs a weapon of the same raw DPS because a granted skill is a BONUS on top of the weapon, not the
// primary output. A skill with no damage stat (a reactive/defensive one) returns 0 here and is
// valued on the defense side instead.
inline f32 skillOffense(const SkillDef& s) {
    if (s.damage <= 0.0f) return 0.0f;                  // reactive/defensive: see skillDefense
    constexpr f32 kIntervalFloor = 1.5f;                // energy/positioning gate (the 30 s lens)
    constexpr f32 kSkillDiscount = 0.60f;               // a granted skill is a sidearm, not the weapon
    const f32 interval = (s.cooldown > kIntervalFloor) ? s.cooldown : kIntervalFloor;
    f32 targets = 1.0f;
    if (s.bounces > 0)  targets += static_cast<f32>(s.bounces) * (s.damageFalloff > 0.0f ? s.damageFalloff : 0.7f);
    if (s.radius > 3.0f) targets += 1.0f;               // an AoE clips a small cluster
    const f32 dps = s.damage * targets / interval;
    return dps * REF_SWING * kSkillDiscount;
}

// DEFENSE credit for a granted skill: an effective-HP-flavoured value for skills that protect rather
// than (or as well as) deal damage. Derived from the SkillDef where the fields exist — an invuln
// window (Phase Dash), an ally/self heal (Holy Nova), a parry stun window (Deflect). A stat-less
// reactive skill with NO SkillDef at all (Mirror Aegis's projectile parry) can't be derived here and
// is hand-valued by the engine resolver instead (there is nothing to read).
inline f32 skillDefense(const SkillDef& s) {
    f32 v = 0.0f;
    v += s.invulnDuration * REF_HP * 0.5f;              // i-frames ~ a fraction of the pool per cast
    v += s.allyHealPct    * REF_HP;                     // a heal is direct effective HP
    v += s.activeWindow   * 20.0f;                      // a parry window is defensive uptime
    return v;
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
        // Glass Cannon: damage above almost all else. Sharpened from 3.0/1.0 so it RELIABLY picks the
        // highest-damage option — a defense roll now barely moves its score, so between two pieces the
        // bigger-damage one wins even when the other is much tankier (Aaron: "glass cannon reliably
        // chooses the builds with the highest damage"). Still 0.5, not 0, so a pure tie breaks toward
        // not-dying. The 3.5+0.5 sum stays 4 — load-bearing for the cross-cell better-build nudge.
        case 2:  offW = 3.5f; defW = 0.5f; break;   // Glass Cannon
    }
}

inline f32 score(const ItemInstance& item, const ItemDef& def, u8 cell) {
    if (item.defId == 0xFFFF) return 0.0f;                       // empty scores nothing
    // A minipet is not gear: it claims the ring slot only to satisfy the loader, and its high
    // rarity would otherwise leak a phantom tiebreak score into the RING column — a bag pet was
    // "the best fieldable ring" for a fresh character, suppressing real ring pickups. Pets are
    // handled by name in worthPickingUp/isKeeper; as GEAR they are worth exactly nothing.
    if (def.petSummon) return 0.0f;
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
            dps = WeaponDps::sustained(perHit, effCd, shots, reload);
        } else {
            dps = WeaponDps::sustained(perHit, effCd, 0.0f, 0.0f);
        }
        if (def.baseProjectileSpeed > 0.0f)
            dps *= 1.0f + projSpd * 0.004f;              // +40% roll => +16% effective DPS
        off += dps * REF_SWING;
        // Ranged-column weapons carry an extra tiebreak for PER-HIT damage, because Marksman/Ranger
        // skills scale off it (weaponSkillScaleBonus): comparable-DPS ranged weapons rank by the
        // harder hit. Melee/Magic skills don't read weapon damage, so their columns get nothing here.
        off += weaponSkillScaleBonus(col, perHit);
    } else {
        // A non-weapon's damage rolls accelerate the WEAPON: convert via the reference weapon DPS
        // (a +10% ring is worth 10% of ~60 DPS, not a flat "10"), attack speed likewise.
        off += def.baseDamage;
        off += dmgFlat * 0.5f;
        off += REF_DPS * (dmgPct + atkSpd) * 0.01f * REF_SWING;
        // CDR on a NON-weapon (a helmet, a ring) speeds the WEAPON too, not only skills:
        // getEffectiveWeapon divides the weapon cooldown by (1 - CDR) for EVERY class (inventory.cpp).
        // The weapon branch above already folds this into its own DPS; here the else branch used to
        // drop it, so a helmet full of CDR scored only the skill-cast-rate half below and read as
        // weak defense-adjacent utility. Crediting the weapon half as real DPS is what lets a Glass
        // Cannon prefer a CDR helmet over a defensive one (Aaron: "helmets that provide good CDR over
        // good defense as glass cannon"). Same 50% engine cap the weapon branch uses.
        const f32 cdrEffNW = (cdr > 50.0f) ? 50.0f : cdr;
        off += REF_DPS * (1.0f / (1.0f - cdrEffNW * 0.01f) - 1.0f) * REF_SWING;
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

    // LEGENDARY GRANTED SKILL. A legendary item's active skill (precomputed onto the def at load, see
    // ItemDef::legendarySkillOffense/Defense) is worth its DPS-equivalent / effective-HP that the base
    // stats never showed. Only at LEGENDARY rarity, where the skill actually activates
    // (hud_inventory.cpp). Folded into the SAME off/def buckets so the row weights decide it: a Glass
    // Cannon's heavy offense weight makes a skill-granting shield beat a plain high-defense one
    // (Aaron: skill shields "as glass cannon better than higher defense"), while a Tanky build still
    // values the piece for its base armor + any defensive grant.
    if (item.rarity == Rarity::LEGENDARY) {
        off  += def.legendarySkillOffense;
        def_ += def.legendarySkillDefense;
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
// MINIPETS are unconditionally worth grabbing: a petSummon consumable has no gear stats (score 0
// in every cell — they claim the ring slot only to satisfy the loader), so the stat filter would
// walk past what is some of the rarest loot in the game. The gear-side never sees them anyway:
// auto-equip refuses petSummon and the prune/evict passes exempt them.
inline bool worthPickingUp(const ItemInstance& cand, const ItemDef& def,
                           const PlayerInventory& inv, const ItemDef* defs, u32 defCount) {
    if (def.petSummon) return true;
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
// MINIPETS are always keepers — engine callers already skip petSummon before asking, but the pure
// answer must agree with the pickup filter or a future caller could discard what the vacuum just
// declared unconditionally worth grabbing.
inline bool isKeeper(const PlayerInventory& inv, const ItemDef* defs, u32 defCount, u8 backpackIdx) {
    const ItemInstance& it = inv.backpack[backpackIdx];
    if (it.defId == 0xFFFF || it.defId >= defCount) return false;
    const ItemDef& def = defs[it.defId];
    if (def.petSummon) return true;
    for (u8 cell = 0; cell < BUILD_ROWS * BUILD_COLS; cell++) {
        const f32 s = score(it, def, cell);
        if (s <= 0.0f) continue;
        if (s >= bestSlotScore(inv, defs, defCount, def.slot, cell, static_cast<s32>(backpackIdx)))
            return true;
    }
    return false;
}

// The backpack slot holding the strongest RANGED-family weapon, or -1 if the bag has none. Used by
// the Autoplay melee sidearm (engine_autoplay.cpp): a melee build keeps ranged weapons in its bag
// because worthPickingUp reasons over all nine build cells, so the sidearm it needs is usually
// already there. Scored against a Moderate-Ranged reference cell — the family gate makes every
// non-ranged weapon (and armor/rings/pets) score 0, so only ranged weapons can win.
inline s32 bestRangedBackpackIdx(const PlayerInventory& inv, const ItemDef* defs, u32 defCount) {
    constexpr u8 kRangedRefCell = 1 * BUILD_COLS + 2;   // Moderate / Ranged
    s32 best = -1;
    f32 bestScore = 0.0f;
    for (u8 i = 0; i < MAX_INVENTORY_ITEMS; i++) {
        const ItemInstance& it = inv.backpack[i];
        if (it.defId == 0xFFFF || it.defId >= defCount) continue;
        const ItemDef& def = defs[it.defId];
        if (def.slot != ItemSlot::WEAPON) continue;
        const f32 s = score(it, def, kRangedRefCell);   // 0 for non-ranged (family gate) and pets
        if (s > bestScore) { bestScore = s; best = (s32)i; }
    }
    return best;
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
