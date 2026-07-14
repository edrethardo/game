// champion.cpp — the champion affix ROLL rules (pure; no engine state).
//
// The affix BEHAVIOURS live at their existing hooks, not here:
//   VAMPIRIC / SHIELDING / THUNDERING / HEALTH_LINK -> Combat::applyDamage
//   MOLTEN / FROZEN (death novas)                   -> Engine::handleDeathPreamble
//   MOLTEN (trail) / TELEPORTING / EXTRA_FAST       -> EnemyAI::update
// Keeping the roll pure is what makes it unit-testable — see tests/game/test_champion.cpp.

#include "game/champion.h"

namespace {

// All rollable affix bits, in a fixed order. Not a loop over (1 << i) so the set can be reordered
// or thinned without silently changing which bit a given index means.
constexpr u8 kAllAffixes[ChampAffix::COUNT] = {
    ChampAffix::MOLTEN,      ChampAffix::FROZEN,
    ChampAffix::VAMPIRIC,    ChampAffix::EXTRA_FAST,
    ChampAffix::SHIELDING,   ChampAffix::TELEPORTING,
    ChampAffix::THUNDERING,  ChampAffix::HEALTH_LINK,
};

// Same LCG constants as the level generator's GenRNG — callers own the stream.
inline u32 nextRand(u32& s) { s = s * 1664525u + 1013904223u; return s; }
inline u32 randRange(u32& s, u32 n) { return n ? (nextRand(s) >> 8) % n : 0; }

} // namespace

u8 Champion::affixCountForFloor(u32 effectiveFloor) {
    if (effectiveFloor <= 15) return 1;
    if (effectiveFloor <= 35) return 2;
    return 3;
}

bool Champion::affixesValid(u8 mask, bool hasMinions) {
    // Unfightable: TELEPORTING closes every gap you make, EXTRA_FAST means you can't make one.
    if ((mask & ChampAffix::EXTRA_FAST) && (mask & ChampAffix::TELEPORTING)) return false;
    // HEALTH_LINK splits damage onto living minions — with no minions it would be a no-op affix,
    // i.e. a champion that visibly advertises a power it does not have.
    if ((mask & ChampAffix::HEALTH_LINK) && !hasMinions) return false;
    return true;
}

u8 Champion::rollAffixes(u32 effectiveFloor, bool hasMinions, u32& rngState) {
    const u8 want = affixCountForFloor(effectiveFloor);

    // Draw distinct affixes, skipping any that would make the mask illegal. Rejecting per-candidate
    // (rather than re-rolling the whole mask) means the exclusion rules can never loop forever: the
    // candidate pool only shrinks.
    u8 mask = ChampAffix::NONE;
    u8 placed = 0;
    u8 remaining[ChampAffix::COUNT];
    u8 remainingCount = 0;
    for (u8 i = 0; i < ChampAffix::COUNT; i++) remaining[remainingCount++] = kAllAffixes[i];

    while (placed < want && remainingCount > 0) {
        const u32 pick = randRange(rngState, remainingCount);
        const u8  bit  = remaining[pick];

        // Remove the candidate whether or not we take it — an affix rejected once is rejected for
        // this roll, and leaving it in the pool could spin.
        remaining[pick] = remaining[--remainingCount];

        const u8 candidate = static_cast<u8>(mask | bit);
        if (!affixesValid(candidate, hasMinions)) continue;

        mask = candidate;
        placed++;
    }
    return mask;
}

Vec3 Champion::tintFor(u8 mask) {
    // Dominant-affix colour: the FIRST set bit in kAllAffixes order wins, so a mask always maps to
    // exactly one colour and the client (which only has the mask) agrees with the host by
    // construction. A blended average would mush every multi-affix champion into the same grey.
    if (mask & ChampAffix::MOLTEN)      return {1.00f, 0.35f, 0.10f};  // ember orange
    if (mask & ChampAffix::FROZEN)      return {0.45f, 0.80f, 1.00f};  // ice blue
    if (mask & ChampAffix::VAMPIRIC)    return {0.80f, 0.10f, 0.25f};  // blood red
    if (mask & ChampAffix::EXTRA_FAST)  return {1.00f, 0.90f, 0.30f};  // quick yellow
    if (mask & ChampAffix::SHIELDING)   return {0.75f, 0.80f, 0.95f};  // pale steel
    if (mask & ChampAffix::TELEPORTING) return {0.70f, 0.35f, 1.00f};  // warp violet
    if (mask & ChampAffix::THUNDERING)  return {0.55f, 0.85f, 1.00f};  // arc cyan
    if (mask & ChampAffix::HEALTH_LINK) return {0.40f, 1.00f, 0.55f};  // link green
    return {1.0f, 1.0f, 1.0f};  // not a champion — no tint
}

namespace {
// Rolled champion names. Deliberately a fixed table indexed by a rolled byte rather than generated
// strings: the index is what goes on the wire, so the client rebuilds the same name from the same
// data. Add to the end freely — the index simply wraps, so a short table can never crash.
constexpr const char* kChampNames[Champion::NAME_COUNT] = {
    "Grimfang",  "Skarn",     "Vorlok",    "Mordath",
    "Bilewretch","Ashgrave",  "Thornclaw", "Rotmaw",
    "Draghul",   "Sorrowbane","Ironhusk",  "Nightmar",
    "Gorehowl",  "Cindervex", "Blackmire", "Zhargul",
    "Ruinspite", "Hollowfen", "Baelgrim",  "Vexmourn",
    "Duskrender","Foulhorn",  "Direlok",   "Gravewhisper",
};
} // namespace

const char* Champion::baseName(u8 nameIdx) {
    return kChampNames[nameIdx % NAME_COUNT];   // wrap, so an out-of-range roll can never crash
}

const char* Champion::titleFor(u8 mask) {
    // Dominant affix, in the SAME priority order as tintFor — so the name and the colour always
    // describe the same thing. A player who learns "orange = Molten" must not then meet a
    // differently-titled orange champion.
    if (mask & ChampAffix::MOLTEN)      return "the Molten";
    if (mask & ChampAffix::FROZEN)      return "the Frozen";
    if (mask & ChampAffix::VAMPIRIC)    return "the Vampiric";
    if (mask & ChampAffix::EXTRA_FAST)  return "the Swift";
    if (mask & ChampAffix::SHIELDING)   return "the Warded";
    if (mask & ChampAffix::TELEPORTING) return "the Blinking";
    if (mask & ChampAffix::THUNDERING)  return "the Thundering";
    if (mask & ChampAffix::HEALTH_LINK) return "the Bound";
    return "the Champion";   // total: a champion always has a title, even on a mask we don't know
}

void Champion::formatName(char* out, u32 outSize, u8 nameIdx, u8 mask) {
    if (!out || outSize == 0) return;
    const char* base  = baseName(nameIdx);
    const char* title = titleFor(mask);
    u32 w = 0;
    for (const char* p = base;  *p && w + 1 < outSize; ++p) out[w++] = *p;
    if (w + 1 < outSize) out[w++] = ' ';
    for (const char* p = title; *p && w + 1 < outSize; ++p) out[w++] = *p;
    out[w] = '\0';
}

const char* Champion::affixName(u8 singleBit) {
    switch (singleBit) {
        case ChampAffix::MOLTEN:      return "Molten";
        case ChampAffix::FROZEN:      return "Frozen";
        case ChampAffix::VAMPIRIC:    return "Vampiric";
        case ChampAffix::EXTRA_FAST:  return "Extra Fast";
        case ChampAffix::SHIELDING:   return "Shielding";
        case ChampAffix::TELEPORTING: return "Teleporting";
        case ChampAffix::THUNDERING:  return "Thundering";
        case ChampAffix::HEALTH_LINK: return "Health Link";
        default:                      return "";
    }
}
