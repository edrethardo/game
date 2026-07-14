#pragma once

#include "core/types.h"

// Centralised tuning constants. Replaces magic numbers scattered across
// engine.cpp, enemy_ai.cpp, item.cpp, projectile.cpp, and hud.cpp.
// Changing a value here takes effect everywhere it's consumed.

namespace GameConst {
    // --- Demo build -------------------------------------------------------------
    // kDemoBuild is true only when the engine is compiled with -DDEMO_BUILD=ON (see the
    // top-level CMakeLists option). It's a compile-time constant so every demo-only branch
    // (`if (GameConst::kDemoBuild)`) is dead-stripped in the normal build and vice-versa —
    // no runtime cost, and the full game's behavior is byte-for-byte unchanged.
    // The demo exposes only singleplayer + local couch co-op and ends after FINAL_FLOOR.
#ifdef DEMO_BUILD
    static constexpr bool kDemoBuild = true;
#else
    static constexpr bool kDemoBuild = false;
#endif
    // Last playable floor before the game ends: the demo stops at floor 20; the full game
    // ends a difficulty tier at floor 50 (then advances Normal->Nightmare->Hell). NOTE: this
    // is ONLY the end-of-run boundary — the "50" used for effective-floor enemy scaling
    // (floor + difficulty*50) is a separate, unchanged quantity.
    static constexpr u32 FINAL_FLOOR = kDemoBuild ? 20u : 50u;

    // Sentinel "floor" for The Source — the secret superboss chamber that lives OFF the numbered
    // 1-50 ladder. Used as the floor-99 BossDef key (assets/config/bosses.json) and as the
    // SV_LEVEL_SEED floor value that tells co-op clients to build The Source instead of a normal
    // floor (floor is a u8 on the wire, so this needs no packet-format change). See ~/.claude/plans.
    static constexpr u8 SOURCE_SENTINEL_FLOOR = 99u;

    // Enemy base stats (before floor scaling) — all HP includes +20% buff
    static constexpr f32 SKELETON_HEALTH     = 55.0f;   // was 40, buffed (+20% + stronger)
    static constexpr f32 SKELETON_SPEED      = 2.8f;
    static constexpr f32 SKELETON_DAMAGE     = 11.0f;   // was 8, stronger
    static constexpr f32 SKELETON_DET_RANGE  = 22.0f;   // long aggro range
    static constexpr f32 SKELETON_ATK_RANGE  = 3.5f;
    static constexpr f32 SKELETON_ATK_COOL   = 1.0f;    // was 1.2, attacks faster

    static constexpr f32 BAT_HEALTH          = 30.0f;   // was 25 (+20%)
    static constexpr f32 BAT_SPEED           = 6.0f;
    static constexpr f32 BAT_DAMAGE          = 7.0f;    // was 6 (+20%)
    static constexpr f32 BAT_DET_RANGE       = 22.0f;   // long aggro range
    static constexpr f32 BAT_ATK_RANGE       = 3.5f;
    static constexpr f32 BAT_ATK_COOL        = 0.8f;    // was 1.0, faster attacks

    static constexpr f32 SPIDER_HEALTH       = 42.0f;   // was 35 (+20%)
    static constexpr f32 SPIDER_SPEED        = 4.0f;
    static constexpr f32 SPIDER_DAMAGE       = 10.0f;   // was 8 (+20%)
    static constexpr f32 SPIDER_DET_RANGE    = 20.0f;   // long aggro range
    static constexpr f32 SPIDER_ATK_RANGE    = 3.0f;
    static constexpr f32 SPIDER_ATK_COOL     = 0.8f;    // was 1.0, faster attacks

    // Global speed multiplier — applied to player, NPCs, and enemies
    static constexpr f32 SPEED_MULT            = 1.6675f; // was 1.45, +15% global speed

    // Floor scaling — 10% per floor so difficulty ramps steadily to floor 50.
    // This is the HEALTH slope (and the legacy linear baseline that floorHealthMult clamps against).
    static constexpr f32 FLOOR_STAT_MULT     = 0.10f;

    // Enemy DAMAGE has its own, steeper slope (0.10 -> 0.13).
    //
    // It was raised to pay for a real bug fix, not a feel tweak: item health used to reach the
    // player not at all (Inventory::getEffectiveMaxHealth was correct and called by nothing), so a
    // geared character is now roughly 3x tankier than every enemy number was ever tuned against —
    // a Hell-50 paladin went from ~1,195 to ~3,722 HP. Gear health grows with ITEM LEVEL, i.e. with
    // depth, so the compensation has to grow with depth too.
    //
    // 0.10 -> 0.13 -> 0.16. This slope feeds EVERY difficulty, which is exactly why it moves in
    // small steps: it is the one lever that cannot be aimed at Hell alone. See difficultyDamageBump
    // below, which is per-tier and carries the heavy end of the Hell increase.
    static constexpr f32 FLOOR_DAMAGE_MULT   = 0.16f;

    // --- Difficulty / floor enemy scaling ----------------------------------------
    // Every enemy scales by its "effective floor" = raw floor + difficulty*50
    // (Normal +0, Nightmare +50, Hell +100). Two curves feed off that number:
    //   * HEALTH compounds (floorHealthMult) so the +50/+100 difficulty offset ramps
    //     enemies EXPONENTIALLY. The old flat +10%/floor made Nightmare/Hell feel barely
    //     tougher than late Normal (linear growth flattens relatively as it climbs).
    //   * DAMAGE stays linear (floorDamageMult) with a flat per-tier bump
    //     (difficultyDamageBump) instead — compounding damage would one-shot the player,
    //     whose HP scales far slower than the enemy's effective-floor count.
    //
    // Per-floor compounding rate for HEALTH: 3% -> 3.64%, solved so an enemy at the end of Hell has
    // exactly 2.5x the HP it used to (81.8x -> 205.9x of base).
    //
    // It is a COMPOUNDING rate, so it aims itself: compounding only overtakes the legacy linear
    // curve at effective floor 52, which means the whole of NORMAL is bit-for-bit untouched, and the
    // increase lands where it was asked for — Nightmare 50 x1.85, Hell 50 x2.52.
    //
    // Paired with the damage raise (see difficultyDamageBump), this also RESTORES the invariant that
    // enemy HP outscales enemy damage: tripling the damage alone had inverted it (147x damage vs 82x
    // HP), which would have made deep enemies glass cannons. At 205.9x HP vs 146.7x damage the
    // ordering holds again, and a Hell-50 trash mob is a real fight: it kills a geared paladin in
    // 3.4 hits and takes 7.8 to put down.
    static constexpr f32 DIFFICULTY_HP_COMPOUND_RATE = 0.0364f;

    // Compounding HEALTH multiplier for an effective floor (1-based: floor 1 -> 1.0x).
    // Returns max(legacy-linear, compounding) so the change can only ever make enemies
    // TOUGHER, never weaker — Normal (and early Nightmare, before compounding overtakes
    // linear) is left exactly as it was. Called once per enemy spawn, never per frame.
    inline f32 floorHealthMult(u32 effectiveFloor) {
        if (effectiveFloor < 1) effectiveFloor = 1;
        f32 linear = 1.0f + static_cast<f32>(effectiveFloor - 1) * FLOOR_STAT_MULT;
        // (1 + rate)^(effectiveFloor-1) by repeated multiply — keeps this header free of
        // <cmath> and is exact enough; effectiveFloor <= ~150 so the loop is trivial.
        f32 comp = 1.0f;
        for (u32 i = 1; i < effectiveFloor; ++i) comp *= (1.0f + DIFFICULTY_HP_COMPOUND_RATE);
        return comp > linear ? comp : linear;
    }

    // Linear DAMAGE multiplier for an effective floor — the original +10%/floor curve.
    // Enemy damage intentionally does NOT compound (see difficultyDamageBump).
    inline f32 floorDamageMult(u32 effectiveFloor) {
        if (effectiveFloor < 1) effectiveFloor = 1;
        return 1.0f + static_cast<f32>(effectiveFloor - 1) * FLOOR_DAMAGE_MULT;
    }

    // Flat per-difficulty DAMAGE bump applied on top of floorDamageMult so Nightmare/Hell
    // are more lethal without compounding damage into instant-kills.
    inline f32 difficultyDamageBump(u8 difficulty) {
        // Raised again: Hell's endgame still wasn't lethal. The brief was "3x the damage at the end
        // of Hell", and the split matters because the two levers have different blast radii:
        //
        //   * the SLOPE (FLOOR_DAMAGE_MULT, 0.13 -> 0.16) feeds EVERY tier, so it can only move a
        //     little before Normal takes collateral damage — it buys +17% at Normal 5, +30% at
        //     Normal 50.
        //   * this per-tier BUMP is the only Hell-ISOLATED lever, so it carries the rest. 5.90 is
        //     not a taste value: it is solved so that (1 + 0.16*149) * bump lands exactly on 3.00x
        //     the old Hell-50 damage (48.9x -> 146.7x).
        //
        // Consequence worth knowing: the bump is flat across the tier, so Hell FLOOR 1 is also ~3x,
        // not just floor 50. Making ONLY the tier's end 3x would need the slope to carry it, and the
        // slope cannot be steepened that far without making Normal brutal.
        //
        // For the real geared paladin (3,722 HP, 61% armour) a Hell-50 trash mob goes from hitting
        // for 362 to 1,087 — from 10.3 hits to kill him down to 3.4.
        switch (difficulty) {
            case 1:  return 1.90f;  // Nightmare (was 1.75)
            case 2:  return 5.90f;  // Hell      (was 2.40) — solved for exactly 3x at floor 50
            default: return 1.25f;  // Normal    (was 1.15)
        }
    }

    // --- Enemy opening strike -----------------------------------------------------------------
    // How long after an enemy ENTERS attack range its first swing lands. This is NOT the attack
    // cooldown: an arriving enemy has always skipped straight to a short opening window rather than
    // waiting a full attack cycle.
    //
    // ONLY the plain charge moves (0.30 -> 0.20). It was the slowest opening of the four and the one
    // the player meets most, so it is where the pressure was missing; the ambush / flank / surround
    // openings were already fast and are left exactly as they were.
    //
    // No value here is ever ABOVE the one it replaced, so an enemy can only open faster, never
    // slower. That guarantee is the point: taking "first hit at half the attack delay" literally
    // would have been a NERF — 20 of the 38 enemies have cooldowns above 0.6 s, and half of those is
    // LONGER than the 0.30 s opening they already used, so pressure would have gone DOWN while
    // looking like it went up.
    //
    // The spread is tactical: an ambusher bursts on reveal, a flanker arrives already committed, a
    // surrounder is taking a slot, and a straight charge is the most telegraphed of the four.
    //
    // The spread between them is tactical and deliberate: an ambusher bursts on reveal, a flanker
    // arrives already committed, a surrounder is taking a slot, and a straight charge is the most
    // telegraphed of the four — so it stays the slowest.
    static constexpr f32 OPEN_STRIKE_AMBUSH   = 0.00f;  // burst immediately on reveal (unchanged)
    static constexpr f32 OPEN_STRIKE_FLANK    = 0.10f;  // unchanged — arrives already committed
    static constexpr f32 OPEN_STRIKE_INRANGE  = 0.10f;  // unchanged — already in range on arrival
    static constexpr f32 OPEN_STRIKE_SURROUND = 0.20f;  // unchanged — taking an encircle slot
    static constexpr f32 OPEN_STRIKE_CHASE    = 0.20f;  // 0.30 -> 0.20: the plain charge into range

    // --- Melee commit ------------------------------------------------------------------------
    // A squad's melee enemies are assigned ROLE_RUSH for the first two and ROLE_FLANK for everyone
    // else, and a flanker in CHASE diverts to a flank cell for 4 s. That redirect had NO distance
    // gate: a melee enemy standing right next to the player would turn around and walk away to
    // "flank" — repeatedly, since the tactical timer refreshes. Measured at 24-30% of all enemy
    // time, and the single biggest drain on melee pressure in the game.
    //
    // Inside this multiple of its attack range, a flanker COMMITS: it stops manoeuvring and goes
    // for the kill. Flanking stays what it should be — a way to APPROACH from an angle — instead of
    // a reason to disengage from a fight you are already in.
    static constexpr f32 MELEE_COMMIT_RANGE_MULT = 2.0f;

    // Seconds of "calm" at the start of each floor: hostile enemies don't
    // auto-aggro and friendly NPCs hold position with the player, so the world
    // isn't already fighting at spawn. Ends early the moment the player attacks.
    // Damage-driven aggro (Combat::applyDamage) is never gated by this.
    static constexpr f32 SPAWN_CALM_SECONDS  = 0.4f;

    // Interaction (the E / X button)
    // Loot is what the hand reaches for a hundred times a floor, so an ITEM always wins a tap —
    // a shrine or an exit you happen to be standing on must never eat the grab. Holding is the
    // deliberate override for the two things you do once. 0.35 s is long enough that no ordinary
    // grab trips it and short enough that it never feels like waiting.
    static constexpr f32 INTERACT_HOLD_SEC   = 0.35f;
    static constexpr f32 INTERACT_RANGE      = 3.5f;   // XZ range for aimed pickup / activation
    static constexpr f32 INTERACT_MIN_DOT    = 0.3f;   // must be in the front ~140° arc

    // Combat
    static constexpr f32 LOOT_DROP_CHANCE    = 0.40f;
    static constexpr f32 GLOBE_DROP_CHANCE   = 0.55f;  // single globe type, drops often
    static constexpr f32 GLOBE_HEAL_PCT      = 0.30f;  // restores 30% max HP
    static constexpr f32 GLOBE_ENERGY_PCT    = 0.30f;  // restores 30% max energy
    static constexpr f32 POTION_HEAL_PCT     = 0.60f;  // restores 60% max HP
    static constexpr f32 POTION_ENERGY_PCT   = 0.30f;  // restores 30% max energy
    static constexpr f32 POTION_COOLDOWN     = 5.0f;

    // Shared low-HP threshold: HP fraction at/below which the screen-edge red vignette
    // ramps up AND the potion flask pulses red. One constant so both fire together.
    static constexpr f32 LOW_HP_FRACTION     = 0.25f;

    // --- Netplay activation leniency (skills + potion) ---------------------------
    // Ticks of slack added to every skill/potion cooldown gate. In MP the client
    // predicts its own activations locally while the server validates them; the
    // client can legitimately run a few ticks ahead of the server (clock-sync / RTT
    // skew), and the MAX(local,snapshot) cooldown adoption can nudge a timer forward
    // by a tick. With a zero-tolerance gate either makes a perfectly-timed press drop
    // silently ("I pressed it and nothing happened"). 6 ticks (~100ms) covers that
    // skew yet stays far below any real cooldown (whole seconds = 120+ ticks), so it
    // can't be abused to re-fire early. Client feel > 1:1 sim, per design intent.
    static constexpr u32 ACTIVATION_GRACE_TICKS = 6;

    // Shared lenient cooldown gate. Both the client prediction path and the server
    // validation path call this with the press's tick (m_clientTick locally,
    // input->clientTick server-side) so they agree by construction — guaranteed
    // identical because it's one function, not two formulas kept in sync by hand.
    // lastActivationTick == 0 is the "never activated" sentinel = always ready.
    inline bool cooldownReady(u32 now, u32 lastActivationTick, u32 cooldownTicks) {
        if (lastActivationTick == 0) return true;
        // Ticks are monotonic so now >= lastActivationTick; the subtraction can't
        // underflow. Add grace before comparing so a press up to ACTIVATION_GRACE_TICKS
        // early still passes.
        return (now - lastActivationTick + ACTIVATION_GRACE_TICKS) >= cooldownTicks;
    }

    // World items
    static constexpr f32 ITEM_SCALE          = 1.4f;
    static constexpr f32 GLOBE_SCALE         = 0.4f;
    static constexpr f32 GLOBE_PICKUP_RADIUS = 2.5f;

    // NPC base health by class (before equipment bonuses) — kept modest
    // so the player is clearly the strongest party member
    static constexpr f32 NPC_HEALTH_CLERIC   = 15.0f;
    static constexpr f32 NPC_HEALTH_ARCHER   = 8.0f;
    static constexpr f32 NPC_HEALTH_MAGE     = 9.0f;
    static constexpr f32 NPC_HEALTH_ROGUE    = 10.0f;
    static constexpr f32 NPC_HEALTH_PALADIN  = 22.0f;
    static constexpr f32 NPC_FOLLOW_DIST     = 4.0f;
    // Per-floor equipment upgrade multiplier for surviving NPCs
    static constexpr f32 NPC_EQUIP_UPGRADE_MULT = 0.20f;
    static constexpr f32 MIMIC_TRIGGER_DIST  = 2.5f;
    static constexpr f32 MIMIC_HEALTH        = 60.0f;
    static constexpr f32 MIMIC_DAMAGE        = 20.0f;

    // Inventory UI
    static constexpr f32 DBLCLICK_TIME       = 0.3f;
    static constexpr s32 DRAG_DEADZONE_SQ    = 9;
}
