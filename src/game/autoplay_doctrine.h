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
#include "game/build_score.h"

namespace Autoplay {

struct Doctrine {
    f32  engageMin = 0.0f;   // hold no closer than this * attackRange (kite floor; 0 = commit)
    f32  engageMax = 1.0f;   // close to at least this * attackRange to fire
    f32  potionHpFrac = 0.5f;// drink when hp/maxHp drops below this
    u8   disengageCount = 3; // this many enemies inside melee arc => break off (per-cell; default 3 = Moderate)
    bool blocks = false;             // hold-block between actions / during reloads
    bool dodgesProactively = false;  // roll away from closing enemies before they hit
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
        default: d.engageMin = 0.30f; d.engageMax = 0.75f; break;  // Magic: mid
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
            d.usesCover = true;  d.disengageCount = 2;
            d.engageMax = (col == 1) ? d.engageMax : 1.00f;      // ranged/magic go max-range
            // Only Glass/Ranged seeks high ground — a deliberate asymmetry: ranged gains the most
            // from open sightlines, while melee wants to close and magic fights mid, so neither
            // pays the traversal cost to climb.
            d.preferHighGround = (col == 2); break;
        default: // Moderate
            d.potionHpFrac = 0.50f; d.blocks = (col != 2); d.dodgesProactively = (col == 2);
            d.usesCover = false; d.disengageCount = 3; break;
    }
    return d;
}

} // namespace Autoplay
