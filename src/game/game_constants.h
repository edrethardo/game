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
    // The town hub (post-Engine home base). Same sentinel mechanism as The Source: the host
    // broadcasts SV_LEVEL_SEED with this floor and clients build the same deterministic town.
    static constexpr u8 TOWN_SENTINEL_FLOOR   = 98u;
    // The PvP arena (Arena mode). Same sentinel mechanism again: SV_LEVEL_SEED carries this
    // floor and every peer builds the same deterministic colosseum (engine_arena.cpp). The
    // floor value IS the mode flag on the wire — nothing else marks a session as PvP.
    static constexpr u8 ARENA_SENTINEL_FLOOR  = 97u;

    // Enemy base stats (before floor scaling) — all HP includes +20% buff
    static constexpr f32 SKELETON_HEALTH     = 83.0f;   // 2026-07-22 global monster buff: all HP/damage x1.5 (was 55)
    static constexpr f32 SKELETON_SPEED      = 2.8f;
    static constexpr f32 SKELETON_DAMAGE     = 17.0f;   // was 8, stronger
    static constexpr f32 SKELETON_DET_RANGE  = 22.0f;   // long aggro range
    static constexpr f32 SKELETON_ATK_RANGE  = 3.5f;
    static constexpr f32 SKELETON_ATK_COOL   = 1.0f;    // was 1.2, attacks faster

    static constexpr f32 BAT_HEALTH          = 45.0f;   // was 25 (+20%)
    static constexpr f32 BAT_SPEED           = 6.0f;
    static constexpr f32 BAT_DAMAGE          = 11.0f;    // was 6 (+20%)
    static constexpr f32 BAT_DET_RANGE       = 22.0f;   // long aggro range
    static constexpr f32 BAT_ATK_RANGE       = 3.5f;
    static constexpr f32 BAT_ATK_COOL        = 0.8f;    // was 1.0, faster attacks

    static constexpr f32 SPIDER_HEALTH       = 63.0f;   // was 35 (+20%)
    static constexpr f32 SPIDER_SPEED        = 4.0f;
    static constexpr f32 SPIDER_DAMAGE       = 15.0f;   // was 8 (+20%)
    static constexpr f32 SPIDER_DET_RANGE    = 20.0f;   // long aggro range
    static constexpr f32 SPIDER_ATK_RANGE    = 3.0f;
    static constexpr f32 SPIDER_ATK_COOL     = 0.8f;    // was 1.0, faster attacks

    // Global speed multiplier — applied to player, NPCs, and enemies
    static constexpr f32 SPEED_MULT            = 1.6675f; // was 1.45, +15% global speed

    // Floor scaling — +12% of base per floor so difficulty ramps steadily to floor 50.
    // This is the HEALTH slope (and the linear baseline that floorHealthMult clamps against).
    // 0.10 -> 0.12 (2026-07-23, balance lab): the lab's first report measured Normal trash
    // MELTING faster with depth — TTK fell 0.77s at floor 1 to 0.21s at floor 40, an inverted
    // curve — because gear DPS grows with item level while this slope stood still. Steepening
    // it makes trash meatier wherever the linear curve governs (all of Normal, the first floors
    // of Nightmare); from effective floor 53 the compounding curve takes over (see
    // DIFFICULTY_HP_COMPOUND_RATE), so mid-Nightmare and Hell don't notice.
    static constexpr f32 FLOOR_STAT_MULT     = 0.12f;

    // Enemy DAMAGE has its own, steeper slope.
    //
    // It was raised to pay for a real bug fix, not a feel tweak: item health used to reach the
    // player not at all (Inventory::getEffectiveMaxHealth was correct and called by nothing), so a
    // geared character is now roughly 3x tankier than every enemy number was ever tuned against —
    // a Hell-50 paladin went from ~1,195 to ~3,722 HP. Gear health grows with ITEM LEVEL, i.e. with
    // depth, so the compensation has to grow with depth too.
    //
    // 0.10 -> 0.13 -> 0.16 -> 0.18 -> 0.17 -> 0.20 -> 0.24. This slope feeds EVERY difficulty, which is
    // exactly why it moves in small steps: it is the one lever that cannot be aimed at Hell alone. See
    // difficultyDamageBump below, which is per-tier and carries the heavy end of the Hell increase.
    // The two 2026-07-23 steps are the balance lab's first tuning session: its report measured deep
    // Normal as the safest place in the game (~30 hits-to-die at floor 40, trash TTK falling
    // 0.77s -> 0.21s with depth — inverted), and this slope is the depth-weighted lever that fixes
    // exactly that. 0.20 was pass 1; 0.24 is Aaron's second Normal push from the same session
    // (Normal's bump rose 1.40 -> 1.55 with it, and the NM + Hell bumps were RE-SOLVED
    // 2.80 -> 2.35 and 9.58 -> 8.03 so both tiers stand still at their pass-1 totals).
    //
    // WORTH KNOWING BEFORE YOU TOUCH THIS: raising the slope does essentially NOTHING to Hell. Hell
    // sits at effective floors 101-150, where (1 + slope*149) is dominated by the slope term and the
    // "+1" is noise — so steepening the slope and solving the Hell bump back down to hit the same
    // target cancels almost exactly (measured: Hell 1/25/50 land within 0.5% of each other whether the
    // slope is 0.18, 0.19 or 0.20). The slope is therefore a NORMAL-difficulty dial, not a Hell one.
    // The 2026-07-23 session is the worked example of exploiting that cancellation ON PURPOSE, twice:
    // slope 0.17 -> 0.20 -> 0.24 for Normal (and the meat of NM's ramp), Hell's bump re-solved down
    // each time (11.20 -> 9.58 -> 8.03), and Hell's total damage moved by under 0.5% across its whole
    // tier while Normal's slope-side damage rose +17% at floor 5 and +37% at floor 50 (+45%/+70%
    // total with its 1.25 -> 1.55 bump). Do not reach for this to make the endgame hurt — reach for
    // the bump.
    static constexpr f32 FLOOR_DAMAGE_MULT   = 0.24f;

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
    // Per-floor compounding rate for HEALTH: 3% -> 3.64% -> 4.41% -> 3.9%.
    //
    // 4.41% tripled Hell's HP and made a Hell-50 trash mob a 23-swing slog, so it was pulled back to
    // 3.9%: Hell floor 50 now sits at 299x base (1.45x what the 3.64% curve gave), which is a mob you
    // grind down in ~11 hits rather than 23.
    //
    // It is a COMPOUNDING rate, so it largely aims itself: against the steeper 0.12 linear slope
    // (2026-07-23) it only overtakes at effective floor 53, so ALL OF NORMAL (floors 1-50) RIDES
    // THE LINEAR SLOPE and compounding governs from early Nightmare (floor 3) onward. Normal's HP
    // curve therefore lives entirely in FLOOR_STAT_MULT; this rate is a Nightmare/Hell dial.
    // (Before the slope raise the crossover sat at effective floor 46, and Normal 46-50 used to
    // pick up a small compounding tail — that tail is gone, outgrown by the new linear baseline.)
    //
    // Compounding CANNOT be aimed at Hell alone — that is the nature of the lever, and it cuts BOTH
    // ways. It also sets Nightmare's HP (now x1.28), which is why Nightmare's DAMAGE bump has to be
    // re-solved whenever this number moves (see difficultyDamageBump). At 4.41% Nightmare was heading
    // for a bullet sponge; pulling back to 3.9% without touching its bump would have flipped it to the
    // opposite failure — a glass cannon hitting x1.97 while only x1.28 tankier. Neither is a fight.
    //
    // This preserves the invariant that enemy HP outscales enemy damage everywhere — but only JUST
    // at the very end of Hell (299x HP vs ~295x damage, under 1.4% of headroom). See
    // difficultyDamageBump: its Hell value is boxed in against this curve, so moving THIS rate
    // means re-solving that bump.
    static constexpr f32 DIFFICULTY_HP_COMPOUND_RATE = 0.039f;

    // Compounding HEALTH multiplier for an effective floor (1-based: floor 1 -> 1.0x).
    // Returns max(linear, compounding) so the compounding change can only ever make enemies
    // TOUGHER, never weaker — Normal (and the first Nightmare floors, until compounding
    // overtakes linear at effective floor 53) rides the plain FLOOR_STAT_MULT slope.
    // Called once per enemy spawn, never per frame.
    inline f32 floorHealthMult(u32 effectiveFloor) {
        if (effectiveFloor < 1) effectiveFloor = 1;
        f32 linear = 1.0f + static_cast<f32>(effectiveFloor - 1) * FLOOR_STAT_MULT;
        // (1 + rate)^(effectiveFloor-1) by repeated multiply — keeps this header free of
        // <cmath> and is exact enough; effectiveFloor <= ~150 so the loop is trivial.
        f32 comp = 1.0f;
        for (u32 i = 1; i < effectiveFloor; ++i) comp *= (1.0f + DIFFICULTY_HP_COMPOUND_RATE);
        return comp > linear ? comp : linear;
    }

    // Linear DAMAGE multiplier for an effective floor — the linear per-floor curve at slope
    // FLOOR_DAMAGE_MULT. Enemy damage intentionally does NOT compound (see difficultyDamageBump).
    inline f32 floorDamageMult(u32 effectiveFloor) {
        if (effectiveFloor < 1) effectiveFloor = 1;
        return 1.0f + static_cast<f32>(effectiveFloor - 1) * FLOOR_DAMAGE_MULT;
    }

    // Flat per-difficulty DAMAGE bump applied on top of floorDamageMult so Nightmare/Hell
    // are more lethal without compounding damage into instant-kills.
    inline f32 difficultyDamageBump(u8 difficulty) {
        // This bump is the ONLY Hell-isolated lever there is, so when the brief was "3x the damage
        // AND 3x the HP at the end of Hell, and Normal only a wee bit harder" it carried essentially
        // the whole increase — the shared slope cannot help (see FLOOR_DAMAGE_MULT: at Hell's
        // effective floors the slope and the bump cancel, so steepening the slope moves Hell by <1%
        // and only taxes Normal).
        //
        // HELL'S 8.03 IS A RE-SOLVE, NOT A NERF (2026-07-23, twice in one session). The shared
        // slope rose 0.17 -> 0.20 -> 0.24 for Normal/Nightmare, lifting Hell-50's slope factor
        // 26.33 -> 30.80 -> 36.76 — so holding Hell's total damage where it was (~295x base)
        // required 295.1 / 36.76 ~= 8.03. Measured: Hell's total damage moves by under 0.5% across
        // the whole tier (101-150) vs BOTH the pre-session curve and pass 1.
        //
        // 8.03 is also BOXED IN, not chosen by taste. Two hard requirements bracket it, and at the
        // 3.9% HP rate and 0.24 slope the window between them is only [7.98, 8.13]:
        //
        //   * damage must be >= 2x the pre-rework Hell-50 damage (146.6x)  =>  bump >= 7.98
        //   * enemy HP must still OUTSCALE enemy damage (299x HP)          =>  bump <  8.13
        //
        // The second is the invariant that stops deep enemies becoming glass cannons that delete the
        // player before they can be hit back. An earlier pass (bump 15.80 against the then-0.17
        // slope) broke it outright (416x damage against 299x HP) and one-shot a fully geared
        // paladin. Do not raise this bump without either raising DIFFICULTY_HP_COMPOUND_RATE to buy
        // headroom, or knowingly inverting the invariant.
        //
        // Result: a Hell-50 trash mob hits a 3,722 HP geared paladin for ~2,187 — it kills him in 1.7
        // hits and takes ~11 to put down. Lethal, but not a one-shot. (Still true after both
        // re-solves: 36.76 x 8.03 = 295.2x vs the original 26.33 x 11.20 = 294.9x, a +0.1% drift.)
        //
        // NIGHTMARE runs DELIBERATELY hotter than HP parity — Aaron's 2026-07-23 call off the
        // balance lab's first report (NM read 12-19 hits-to-die mid-late tier; the verdict was "a
        // bit harder"). The pre-session 2.30 was SOLVED so damage growth tracked NM's x1.28 HP
        // growth (the compounding rate sets NM's HP whether anyone means it to or not); pass 1 set
        // the deliberate heat at NM-50 = 58.2x base — x1.82 damage growth vs x1.28 HP — and 2.35 is
        // that SAME heat re-solved against the 0.24 slope (58.24 / 24.76 ~= 2.35; the steeper slope
        // now carries more of NM's ramp, so the bump gives some back, and NM's totals drift under
        // 1% across the tier vs pass 1). The x1.28 parity solve is still the REFERENCE POINT: if
        // the HP rate ever moves, re-derive parity first and re-apply the deliberate heat on top —
        // do not treat 2.35 itself as a parity number. Drift still breaks NM both ways: too high is
        // a glass cannon, too low a bullet sponge.
        //
        // Consequence worth knowing: the bump is flat across a tier, so Hell FLOOR 1 is also ~2x, not
        // just floor 50. That is unavoidable — the slope cannot carry a tier's ramp (see
        // FLOOR_DAMAGE_MULT: at Hell's effective floors the slope and bump cancel).
        switch (difficulty) {
            case 1:  return 2.35f;  // Nightmare — pass-1's deliberate heat (NM-50 58.2x) re-solved vs the 0.24 slope
            case 2:  return 8.03f;  // Hell      — re-solved vs the 0.24 slope: ~295x stands still
            default: return 1.55f;  // Normal    (was 1.25 -> 1.40) — flat raise on top of the steeper slope
        }
    }

    // --- Corpse / resurrection ------------------------------------------------------------------
    // A killed enemy squashes into the ground over ENEMY_DEATH_DURATION: Combat::killEntity stamps
    // deathTimer with it, EntitySystem::tickTimers counts it DOWN, and the slot is freed at 0. So a
    // corpse's age is (ENEMY_DEATH_DURATION - deathTimer), not deathTimer itself.
    static constexpr f32 ENEMY_DEATH_DURATION = 1.0f;

    // A necromancer/shaman may not raise a corpse until it has been dead this long. Without the
    // window a kill could be undone on the same frame it landed, which reads as the kill simply not
    // having counted; the delay guarantees the death animation is legible as a death first.
    // NOTE: this is HALF the corpse's whole lifetime, so it also halves the raise window (a corpse
    // is now raisable only during its final 0.5 s, not for its full second on the ground).
    static constexpr f32 REVIVE_LOCKOUT = 0.5f;

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
    // ...but only BEYOND this radius. Inside it you are standing on the thing, and which way you
    // happen to be facing must not decide whether you can pick it up. The old exemption was 0.1 m on
    // a 0.7 m-wide player, so an item you had just walked over was routinely behind your eyeline and
    // the aim cone refused it — the "pickup is sometimes flaky" bug. 1.2 m is a comfortable arm's
    // reach: well outside the body, well inside the 3.5 m aimed range.
    static constexpr f32 INTERACT_GRAB_RADIUS = 1.2f;

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
    // Burrower eruption radius (Burrowing Widow). Proximity-only — no weeping-angel watch
    // rule, because there is nothing to see: the trap fires under the victim's feet.
    static constexpr f32 BURROW_TRIGGER_DIST = 3.0f;
    static constexpr f32 MIMIC_HEALTH        = 90.0f;
    static constexpr f32 MIMIC_DAMAGE        = 30.0f;

    // Inventory UI
    static constexpr f32 DBLCLICK_TIME       = 0.3f;
    static constexpr s32 DRAG_DEADZONE_SQ    = 9;
}
