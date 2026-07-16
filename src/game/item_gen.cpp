// item_gen.cpp — Procedural item generation: rarity roll, affix selection, full item roll.
#include "game/item.h"
#include "core/log.h"

// ============================================================
//  ItemGen
// ============================================================

static u32 s_rngState = 12345;
static u32 s_uidCounter = 1;

// LCG — fast, deterministic, small footprint (suitable for Switch)
static inline u32 lcgNext() {
    s_rngState = s_rngState * 1664525u + 1013904223u;
    return s_rngState;
}

// Returns a value in [0.0, 1.0)
static inline f32 randF01() {
    return static_cast<f32>(lcgNext() >> 8) / static_cast<f32>(1u << 24);
}

// Normal-distribution variance multiplier centered at +5%, clamped to [1.0, 1.1].
// Approximates a bell curve via central limit theorem (sum of 3 uniforms).
static f32 rollVariance() {
    f32 sum = randF01() + randF01() + randF01();
    f32 normalized = (sum - 1.5f) / 0.866f; // mean=0, stddev≈1
    f32 v = 1.05f + normalized * 0.033f;     // center +5%, stddev 3.3%
    if (v < 1.0f) v = 1.0f;
    if (v > 1.1f) v = 1.1f;
    return v;
}

// Returns a value in [0, range)
static inline u32 randU32(u32 range) {
    if (range == 0) return 0;
    return lcgNext() % range;
}

void ItemGen::init(u32 seed) {
    s_rngState   = seed;
    s_uidCounter = 1;
    LOG_INFO("ItemGen: RNG seeded with %u", seed);
}

Rarity ItemGen::rollRarity(u8 enemyLevel) {
    // Base rates (%) — sum = 100
    f32 commonPct    = 60.0f;
    f32 magicPct     = 28.0f;
    f32 rarePct      = 10.0f;
    f32 legendaryPct =  2.0f;

    // Per level above 1: common drops faster, legendary rises faster so deep
    // floors feel rewarding.  At floor 10 legendary is ~11%, floor 25 ~26%.
    f32 levelsAbove1 = static_cast<f32>(enemyLevel > 1 ? enemyLevel - 1 : 0);
    commonPct    -= levelsAbove1 * 1.5f;
    legendaryPct += levelsAbove1 * 1.0f;

    // Clamp to sane ranges
    if (commonPct    < 0.0f)   commonPct    = 0.0f;
    if (legendaryPct > 50.0f)  legendaryPct = 50.0f;

    // Remaining budget split evenly between magic and rare to keep sum = 100
    f32 remaining = 100.0f - commonPct - legendaryPct;
    if (remaining < 0.0f) remaining = 0.0f;
    magicPct = remaining * (28.0f / 38.0f);
    rarePct  = remaining * (10.0f / 38.0f);

    f32 roll = randF01() * 100.0f;

    if (roll < legendaryPct)                          return Rarity::LEGENDARY;
    if (roll < legendaryPct + rarePct)                return Rarity::RARE;
    if (roll < legendaryPct + rarePct + magicPct)     return Rarity::MAGIC;
    return Rarity::COMMON;
}

void ItemGen::rollAffixes(ItemInstance& item, u8 itemLevel, ItemSlot slot,
                           const AffixDef* affixDefs, u32 affixDefCount,
                           WeaponType weaponType) {
    // Determine how many affixes to roll based on rarity
    u8 minAffixes = 0;
    u8 maxAffixes = 0;
    switch (item.rarity) {
        case Rarity::COMMON:    minAffixes = 0; maxAffixes = 0; break;
        case Rarity::MAGIC:     minAffixes = 1; maxAffixes = 2; break;
        case Rarity::RARE:      minAffixes = 2; maxAffixes = 4; break;
        // Legendaries are the exclusive named uniques (skill identity + top tier) — they never
        // roll BELOW a good rare. The old 2-3 band meant an orange could carry fewer affixes
        // than a yellow, which is exactly the "just a legendary-colored normal item" feel.
        case Rarity::LEGENDARY: minAffixes = 3; maxAffixes = 4; break;
        default: break;
    }

    u8 affixCount = (maxAffixes > minAffixes)
        ? static_cast<u8>(minAffixes + randU32(maxAffixes - minAffixes + 1))
        : minAffixes;

    if (affixCount > MAX_AFFIXES_PER_ITEM)
        affixCount = MAX_AFFIXES_PER_ITEM;

    // Build list of valid affix candidates for this slot, filtering nonsensical stats
    u32 slotBit = 1u << static_cast<u32>(slot);
    u32 candidateIndices[MAX_AFFIX_DEFS];
    u32 candidateCount = 0;
    for (u32 i = 0; i < affixDefCount; i++) {
        if (!(affixDefs[i].validSlots & slotBit)) continue;
        // Skip weapon-type-specific affixes that don't make sense
        if (slot == ItemSlot::WEAPON) {
            AffixType at = affixDefs[i].type;
            // Reload/clip only on hitscan weapons
            if ((at == AffixType::RELOAD_SPEED_PCT || at == AffixType::CLIP_SIZE_PCT) &&
                weaponType != WeaponType::HITSCAN) continue;
            // Projectile speed only on projectile weapons
            if (at == AffixType::PROJECTILE_SPEED && weaponType != WeaponType::PROJECTILE) continue;
            // Cone angle only on melee weapons
            if (at == AffixType::CONE_ANGLE && weaponType != WeaponType::MELEE) continue;
        }
        candidateIndices[candidateCount++] = i;
    }

    item.affixCount = 0;

    // Track which AffixTypes have already been assigned (no duplicate types)
    bool usedTypes[static_cast<u32>(AffixType::COUNT)] = {};

    f32 linearScale = 1.0f + 0.06f * static_cast<f32>(itemLevel);

    for (u8 a = 0; a < affixCount && item.affixCount < MAX_AFFIXES_PER_ITEM; a++) {
        // Shuffle-pick a random candidate that has an unused type
        // Collect remaining valid candidates
        u32 valid[MAX_AFFIX_DEFS];
        u32 validCount = 0;
        for (u32 c = 0; c < candidateCount; c++) {
            u32 idx = candidateIndices[c];
            u32 typeIdx = static_cast<u32>(affixDefs[idx].type);
            if (!usedTypes[typeIdx])
                valid[validCount++] = idx;
        }
        if (validCount == 0) break;

        u32 pick = valid[randU32(validCount)];
        const AffixDef& ad = affixDefs[pick];

        Affix affix;
        affix.type  = ad.type;
        affix.value = (ad.minValue + randF01() * (ad.maxValue - ad.minValue)) * linearScale * rollVariance();

        item.affixes[item.affixCount++] = affix;
        usedTypes[static_cast<u32>(ad.type)] = true;
    }
}

ItemInstance ItemGen::rollItem(u8 enemyLevel, const ItemDef* defs, u32 defCount,
                                const AffixDef* affixDefs, u32 affixDefCount,
                                Rarity rarityFloor) {
    // Wrap enemy level into 1-50 range so Nightmare/Hell repeat the same drop tables.
    // Level 51 (Nightmare floor 1) → wrappedLevel 1, level 100 → 50, level 101 → 1, etc.
    u8 wrappedLevel = ((enemyLevel - 1) % 50) + 1;

    // Rarity FIRST — the tier decides which pool the def comes from. A def is a candidate
    // only when its [minRarity, maxRarity] window contains the tier, which makes the named
    // uniques (minRarity == LEGENDARY) the ONLY possible legendary drops and keeps them out
    // of every lower tier. This replaced the old def-first roll that clamped rarity to
    // def.maxRarity — under that scheme "legendary" was just a color a normal item could
    // wear, and a unique spent most of its drops as a grey stat stick.
    Rarity rolled = rollRarity(enemyLevel);
    if (rolled < rarityFloor) rolled = rarityFloor;  // guaranteed drops raise the tier

    u32 validIndices[MAX_ITEM_DEFS];
    u32 validCount = 0;

    // Candidate search: three widening level passes per tier, degrading the tier only when
    // NO def anywhere supports it. Pass order matters — a guarantee (rarityFloor) must
    // out-rank the level schedule, so the band widens before the tier drops:
    //   pass 0: def's authored level band contains wrappedLevel (the normal case)
    //   pass 1: any def the player could have seen by now (wrappedLevel >= minLevel)
    //   pass 2: any ROLLABLE def at this tier (minLevel <= 50 — keeps minLevel-255
    //           unrollables like pet consumables excluded from even this last resort)
    Rarity tier = rolled;
    for (;;) {
        for (u32 pass = 0; pass < 3 && validCount == 0; pass++) {
            for (u32 i = 0; i < defCount; i++) {
                if (defs[i].minRarity > tier || defs[i].maxRarity < tier) continue;
                if (pass == 0 && !(wrappedLevel >= defs[i].minLevel &&
                                   wrappedLevel <= defs[i].maxLevel)) continue;
                if (pass == 1 && wrappedLevel < defs[i].minLevel) continue;
                if (pass == 2 && defs[i].minLevel > 50) continue;
                validIndices[validCount++] = i;
            }
        }
        if (validCount > 0 || tier == Rarity::COMMON) break;
        tier = static_cast<Rarity>(static_cast<u8>(tier) - 1); // no def at this tier at all
    }

    if (validCount == 0) {
        LOG_WARN("ItemGen: no valid item defs for enemy level %u", enemyLevel);
        // GCC 13 ICE workaround: return a named local instead of `ItemInstance{}`
        // — see comment in engine_update.cpp::handleDropRequest.
        ItemInstance empty;
        return empty;
    }

    // Weighted random selection using dropWeight
    f32 totalWeight = 0.0f;
    for (u32 i = 0; i < validCount; i++)
        totalWeight += defs[validIndices[i]].dropWeight;

    f32 roll = randF01() * totalWeight;
    u32 pick = validIndices[0]; // fallback
    f32 cumulative = 0.0f;
    for (u32 i = 0; i < validCount; i++) {
        cumulative += defs[validIndices[i]].dropWeight;
        if (roll < cumulative) {
            pick = validIndices[i];
            break;
        }
    }
    const ItemDef& def = defs[pick];

    ItemInstance item;
    item.defId     = static_cast<u16>(pick);
    item.itemLevel = enemyLevel;
    item.uid       = s_uidCounter++;

    // The candidate window guarantees def.minRarity <= tier <= def.maxRarity — no clamp.
    item.rarity = tier;

    // Scale base stats — gentle curve so early-floor items aren't overpowered
    f32 levelMult = 1.0f + 0.08f * static_cast<f32>(enemyLevel);
    item.damage      = def.baseDamage  * levelMult * rollVariance();
    item.bonusHealth = def.baseHealth  * levelMult * rollVariance();

    // Roll affixes — pass weapon type to filter nonsensical stats
    rollAffixes(item, enemyLevel, def.slot, affixDefs, affixDefCount, def.weaponType);

    // Legendary wands/staffs always get CDR + max energy bonuses
    if (item.rarity == Rarity::LEGENDARY && def.weaponSubtype == WeaponSubtype::WAND) {
        f32 levelScale = 1.0f + 0.06f * static_cast<f32>(enemyLevel);
        // Inject CDR if not already present
        bool hasCdr = false;
        for (u8 a = 0; a < item.affixCount; a++)
            if (item.affixes[a].type == AffixType::COOLDOWN_REDUCTION) hasCdr = true;
        if (!hasCdr && item.affixCount < MAX_AFFIXES_PER_ITEM) {
            item.affixes[item.affixCount++] = {AffixType::COOLDOWN_REDUCTION, (8.0f + randF01() * 7.0f) * levelScale};
        }
        // Inject energy bonus if not already present
        bool hasEnergy = false;
        for (u8 a = 0; a < item.affixCount; a++)
            if (item.affixes[a].type == AffixType::ENERGY_FLAT) hasEnergy = true;
        if (!hasEnergy && item.affixCount < MAX_AFFIXES_PER_ITEM) {
            item.affixes[item.affixCount++] = {AffixType::ENERGY_FLAT, (15.0f + randF01() * 20.0f) * levelScale};
        }
    }

    return item;
}
