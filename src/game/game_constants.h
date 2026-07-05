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

    // Floor scaling — 10% per floor so difficulty ramps steadily to floor 50
    static constexpr f32 FLOOR_STAT_MULT     = 0.10f;

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
    // Per-floor compounding rate for HEALTH. 3% is chosen so the compounding curve stays
    // BELOW the legacy linear curve through all of Normal and only overtakes it around
    // mid-Nightmare, reaching ~82x base by Hell floor 50 (effective floor 150).
    static constexpr f32 DIFFICULTY_HP_COMPOUND_RATE = 0.03f;

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
        return 1.0f + static_cast<f32>(effectiveFloor - 1) * FLOOR_STAT_MULT;
    }

    // Flat per-difficulty DAMAGE bump applied on top of floorDamageMult so Nightmare/Hell
    // are more lethal without compounding damage into instant-kills.
    inline f32 difficultyDamageBump(u8 difficulty) {
        switch (difficulty) {
            case 1:  return 1.5f;  // Nightmare
            case 2:  return 2.0f;  // Hell
            default: return 1.0f;  // Normal (and any unexpected value)
        }
    }

    // Seconds of "calm" at the start of each floor: hostile enemies don't
    // auto-aggro and friendly NPCs hold position with the player, so the world
    // isn't already fighting at spawn. Ends early the moment the player attacks.
    // Damage-driven aggro (Combat::applyDamage) is never gated by this.
    static constexpr f32 SPAWN_CALM_SECONDS  = 0.4f;

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
