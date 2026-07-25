// autoplay_doctrine.h — how the Autoplay bot PLAYS a given 3x3 build cell.
//
// The build cell already decides what gear the bot wears (Auto Loot & Equip / BuildScore). This
// table decides how it FIGHTS with that gear: the column (0 Magic / 1 Melee / 2 Ranged) sets the
// engagement band as a fraction of the weapon's attackRange; the row (0 Tanky / 1 Moderate /
// 2 Glass Cannon) sets risk posture (when to drink, whether to hold-block or proactively dodge,
// whether to fight from cover). Pure — engagement is unitless (x attackRange) so the same table
// serves a 2 m sword and a 40 m wand. Consumed by autoplay_combat.h / autoplay_brain.
#pragma once
#include "core/types.h"
#include "game/item.h"          // PlayerClass (the fresh-character build seed below)
#include "game/build_score.h"

namespace Autoplay {

// --- fresh-character build seed -------------------------------------------------------------
// A brand-new Autoplay hero has NO persisted build choice, and PlayerInventory's untouched default
// cell is Moderate/MELEE for every class — so Auto Loot & Equip handed a Sorcerer a sword and the
// bot played melee with a caster, which defeats the mode's whole "play the build" promise. Seed
// the COLUMN (damage archetype) from the class and keep the Moderate row.
//
// The column is an EXPLICIT table, deliberately NOT derived from `ClassDef::preferredWeapon`:
// that enum cannot tell Magic from Ranged (Sorcerer and Ranger are BOTH `PROJECTILE`), and the
// HITSCAN gun classes have to land in the same Ranged column as the bow classes. Every entry
// instead matches the family `BuildScore::weaponInFamily` puts the class's STARTING weapon in
// (wand -> 0 Magic, blade -> 1 Melee, gun/bow -> 2 Ranged), so the first auto-equip pass keeps the
// weapon the class was born with instead of scoring it 0 and throwing it away.
inline u8 buildColForClass(PlayerClass c) {
    switch (c) {
        case PlayerClass::SORCERER:        return 0;   // Wand of Sparks -> Magic
        case PlayerClass::WARRIOR:                     // Iron Sword
        case PlayerClass::ROGUE:                       // Rusty Dagger
        case PlayerClass::PALADIN:                     // Iron Sword
        // Wanderer is MELEE, not Ranged: its ClassDef starts it on an Iron Sword and its
        // preferredWeapon (the +20% damage bonus) is MELEE — a Ranged column would score its own
        // starting weapon 0 on frame one.
        case PlayerClass::WANDERER:        return 1;
        case PlayerClass::RANGER:                      // Short Bow
        case PlayerClass::COMBAT_ENGINEER:             // Pistol
        case PlayerClass::MARKSMAN:                    // Revolver
        case PlayerClass::TINKERER:        return 2;   // Pistol
        default:                           return 1;   // CLASS_COUNT / future class: the safe middle
    }
}

// The full 3x3 cell a fresh Autoplay hero starts on: its class's column on the MODERATE row (1).
// Moderate is the deliberate default posture — Tanky/Glass are opinions a player picks, archetype
// is an identity the class already states.
inline u8 defaultCellForClass(PlayerClass c) {
    return static_cast<u8>(1 * BuildScore::BUILD_COLS + buildColForClass(c));
}

// The same build cell with its column forced to Ranged (2), keeping the risk ROW. Used when a melee
// build equips a temporary ranged sidearm (engine_autoplay.cpp): the doctrine must then be a ranged
// one — hold ground and shoot — not the melee "close the distance" band, or the bot would try to
// walk a gun into melee reach. The row (potion threshold, dodge/block posture) is preserved.
inline u8 rangedCellFor(u8 cell) {
    return static_cast<u8>(BuildScore::buildRow(cell) * BuildScore::BUILD_COLS + 2);
}

struct Doctrine {
    f32  engageMin = 0.0f;   // hold no closer than this * attackRange (kite floor; 0 = commit)
    f32  engageMax = 1.0f;   // close to at least this * attackRange to fire
    f32  potionHpFrac = 0.5f;// drink when hp/maxHp drops below this
    u8   disengageCount = 3; // this many enemies inside melee arc => break off (per-cell; default 3 = Moderate)
    bool blocks = false;             // hold-block between actions / during reloads
    bool dodgesProactively = false;  // roll out of an INCOMING melee swing (Autoplay::swingIsIncoming)
    // Minimum seconds between two proactive rolls, enforced by the DRIVER (the engine's own dodge
    // cooldown is 1 s, which as a behaviour rate reads as constant panicked twitching). Glass Cannon
    // keeps the shortest leash — "never get touched" is its identity — but is still bounded.
    f32  dodgeCooldownSec = 4.0f;
    bool usesCover = false;          // reload / recast from findCoverCell, break LOS on cooldown
    bool preferHighGround = false;    // seek balconies/ramps on stacked floors
};

inline Doctrine doctrineFor(u8 cell) {
    const u8 row = BuildScore::buildRow(cell);   // 0 Tanky / 1 Moderate / 2 Glass
    const u8 col = BuildScore::buildCol(cell);   // 0 Magic / 1 Melee / 2 Ranged
    Doctrine d;

    // Column: engagement band as x attackRange. engageMax rises with reach — melee lowest (only
    // fires up close, then keeps closing since engageMin=0), ranged highest (fires at the edge of
    // range and kites), magic between. On a short melee range 0.60x is still a hug; on a 40 m wand
    // 0.75x is genuine mid-range.
    switch (col) {
        case 1: d.engageMin = 0.00f; d.engageMax = 0.60f; break;  // Melee: hug
        case 2: d.engageMin = 0.55f; d.engageMax = 1.00f; break;  // Ranged: kite band
        // Magic: mid-range, but AGGRESSIVE with the wand. engageMax is the fire gate (out.fire fires
        // anything within engageMax x weaponRange, and the same gate drives skill-casting), so 0.90
        // makes a caster poke with its wand and cast across most of its reach instead of holding fire
        // until mid-range; the low engageMin keeps it holding ground and shooting rather than kiting.
        // (Was 0.30/0.75 — "make magic autoplay shoot the weapon more aggressively".)
        default: d.engageMin = 0.15f; d.engageMax = 0.90f; break;  // Magic: mid, shoots aggressively
    }

    // Row: risk posture.
    switch (row) {
        case 0: // Tanky
            d.potionHpFrac = 0.35f; d.blocks = true;  d.dodgesProactively = false;
            d.usesCover = false; d.disengageCount = 6; break;
        case 2: // Glass Cannon
            d.potionHpFrac = 0.60f; d.blocks = false; d.dodgesProactively = true;
            // usesCover is intentional even for Glass/Melee, whose hug band (engageMax 0.60) keeps
            // it close: there it means retreat-to-cover BETWEEN strikes (hit-and-run), not stand
            // off. For Glass/Ranged & Glass/Magic it is the usual break-LOS-while-recasting.
            d.usesCover = true;  d.disengageCount = 2; d.dodgeCooldownSec = 2.5f;
            d.engageMax = (col == 1) ? d.engageMax : 1.00f;      // ranged/magic go max-range
            // MAXIMUM DAMAGE OUTPUT: hold ground and DPS instead of kiting. Kiting trades firing
            // uptime for spacing, and a Glass Cannon's whole identity is output, not positioning — it
            // survives on its proactive dodge (i-frames) and its early potion (60%), not on distance.
            // Melee already hugs (engageMin 0); ranged/magic stop giving ground early (the 4 m
            // KITE_HOLD_GROUND_M floor still lets them keep a melee threat at arm's length) so almost
            // every tick is spent on target. ("glass cannon builds focus on maximum damage output".)
            d.engageMin = (col == 1) ? 0.00f : 0.15f;
            // Only Glass/Ranged seeks high ground — a deliberate asymmetry: ranged gains the most
            // from open sightlines, while melee wants to close and magic fights mid, so neither
            // pays the traversal cost to climb.
            d.preferHighGround = (col == 2); break;
        default: // Moderate
            d.potionHpFrac = 0.50f; d.blocks = (col != 2); d.dodgesProactively = (col == 2);
            d.usesCover = false; d.disengageCount = 3; break;
    }

    // RANGED BUILDS BLOCK TOO. The row table used to leave the shield off for Moderate/Ranged and
    // Glass/Ranged on the theory that a bow user has no business behind a shield. Two engine facts
    // say otherwise: a PERFECT block negates ALL damage and is a pure timing feat the game always
    // rewards, and blocking does NOT gate firing — the only cost is 0.4x move speed for the ~0.15 s
    // the tap lasts. So the shield is free damage prevention for any build that can time it, and
    // the bot taps rather than holds (see PERFECT_BLOCK_* in autoplay_combat.h). Applied AFTER the
    // row switch so it wins over the row's default; the dodge/cover posture the row sets is
    // untouched, because a tap and a roll are different buttons and can both fire in a fight.
    if (col == 2) d.blocks = true;
    return d;
}

} // namespace Autoplay
