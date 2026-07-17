# Crowd Control System — Design Spec

**Date:** 2026-07-17
**Status:** Approved (brainstorm complete) — ready for implementation plan
**Scope:** One coupled feature: a player CC-Resistance stat, player-facing stun, a
counter-CC legendary, PvP block hardening, expanded per-class CC, and a PvP anti-chain
diminishing-returns rule.

## Context

The stun audit (2026-07-17) established two imbalances:

1. **No player-facing defense against control.** The player can be slowed, frozen (a 95%
   slow — `player.h:53` notes it "was a full immobilize" and was deliberately softened),
   poisoned, burned, and cursed, but there is **no stat anywhere that reduces control
   effects**, and no hard stun on the player at all.
2. **Lopsided class CC.** Warrior & Paladin each carry two hard stuns; Rogue/Sorcerer have
   one control tool; Engineer has 0.1 s micro-staggers; and **Ranger, Marksman, Tinkerer,
   and Wanderer have no CC whatsoever.**

The just-shipped Arena (PvP) makes both problems acute: with class CC now able to hit rival
players, a target with no CC defense and classes with no CC offense produce unfair duels.

**Outcome:** a unified "CC Resistance" gear stat with a chase-item legendary, a fair,
themed CC identity for every class, player-facing stun that is meaningful but not
rage-inducing, and PvP fairness rails (block hardening + stun diminishing returns) so the
Arena doesn't become a stun-fest.

## Locked decisions (from the user)

| Area | Decision |
|---|---|
| **CC scope** | Resistance reduces **movement/action CC only** — stun, slow, freeze. Poison/burn/curse untouched (they're damage). |
| **Reduction model** | **% duration cut (tenacity):** `dur *= (1 − ccResist)`. |
| **PvP scope** | New class CC **and** the resistance apply in **both PvE and PvP**. |
| **Slots** | Boots = **signature** (big roll); helmet & armor = **small** roll. Not gloves/ring/offhand. |
| **Cap** | **60%** hard cap. |
| **Stat shape** | **One unified** "CC Resistance" affix. |
| **Innate resist** | **None** — gear only, all classes start at 0%. |
| **Legendary** | **Yes** — "Steadfast Greaves" boots: CC-Resist roll + perfect-dodge clears CC + F active cleanse. |
| **Player stun feel** | **Action lock, camera free** — no move/attack/cast/dodge, but you can still look. |
| **Boots passive** | A **perfect (i-frame) dodge negates/clears active CC** ("perfect rolls win"). |
| **Boots active (F)** | "Break Free" on the `BOOT_SKILL` (=F) rail: cleanse all CC + brief immunity, on cooldown. |
| **PvE stun** | **PvP-only** — enemies never hard-stun the player; in PvE the stat still cuts slow/freeze. |
| **Block abuse fix** | **Energy drain while held** (PvP-scoped). **No perfect-block cooldown** — a perfect block is a timing feat and is *always* rewarded. |
| **Block vs CC** | **Only a perfect block negates CC**; a held block stops damage but the CC lands. |
| **Perfect block/dodge** | **Always available, never on an added cooldown** — the ~0.2 s execution window is the gate; skill is always rewarded. |
| **Fairness model** | **Baseline-up** — fill the four have-nots, keep Warrior/Paladin as CC kings. |
| **Ranger CC** | **Barrage → ~40% slow zone.** |
| **Marksman CC** | **Explosive Round → knockback + stagger.** |
| **Tinkerer CC** | **Detonate Swarm → ~0.6 s AoE EMP stun.** |
| **Wanderer CC** | **Deflect → ~0.75 s stun the attacker** (reactive). |
| **Engineer CC** | **Unchanged** — 0.1 s micro-staggers on Tesla + Shock Bolt are sufficient. |
| **Anti-chain** | **Diminishing returns on stun** (100→50→25%→immune within 8 s), **player-victims only**. |

Concrete numbers below are the designer defaults; tune during implementation.

---

## Component 1 — CC Resistance stat

A new `AffixType::CC_RESIST`, wired the **save-safe on-demand way** — NOT as a cached
`PlayerInventory` field. `PlayerInventory` is serialized raw (`static_assert(sizeof ==
1676)`), so a new cached `bonus*` field would force a SAVE_VERSION bump. The defensive pack
(Armor/Thorns/Health-Regen) and spell damage already dodge this by **summing on demand** and
stamping the result into a **transient, never-serialized `Player` field** each frame — CC
resist follows that exact pattern (no save-format change):

1. `AffixType::CC_RESIST` appended **before `COUNT`** in `src/game/item.h` (never inserted —
   affix types are serialized by ordinal).
2. `affixTypeFromString("cc_resist")` case in `item_loader.cpp`.
3. **`f32 Inventory::ccResist(const PlayerInventory&)`** = `min(0.60f,
   sumEquippedAffix(inv, AffixType::CC_RESIST))` — the same one-liner shape as
   `Inventory::armorRating`/`thornsPct`. **No `PlayerInventory` field, no `recalculateStats`
   change, no save bump.** (The legendary boots simply carry a high-value `CC_RESIST` affix, so
   the sum picks it up with no special-casing.)
4. **Stamp** `m_localPlayer.ccResist = Inventory::ccResist(inv)` into a **transient
   never-serialized `Player.ccResist` field** in `tickPassiveEquipment`
   (`engine_update_skills.cpp:177`, right beside `armorRating`).
5. **Consume** `player.ccResist` in the single choke function (Component 3's
   `applyCCToPlayer`), so every CC source scales through exactly one place.

`affixes.json` gets one entry with per-slot value bands:

- `boots`: 0.15–0.30 (signature)
- `helmet`, `armor`: 0.05–0.12 (small)

`validSlots` bitmask = boots | helmet | armor.

**Reduction covers stun, slow, and freeze durations only.** Poison/burn/curse timers are set
directly (unchanged). Root does not exist in the game and none of the chosen class CC
introduces it, so the stat covers the three existing timers.

## Component 2 — Steadfast Greaves (legendary boots)

Append **one item def** to `items.json` (append-only; `defId` is the saved array index):

- Slot `boots`, legendary, a normal boots stat line (baseHealth). **No forced CC-Resist stat**
  (updated during implementation — Aaron's call): the Greaves can *roll* CC_RESIST affixes like
  any boots, but force none. Their signature is the two anti-CC layers below (2a + 2b), not a
  guaranteed resist number.
- `legendarySkillId = BREAK_FREE` (new `SkillId`, Component 2b).
- New mesh via `tools/gen_mesh.py`, registered in **both** `asset_manifest.h` and
  `build_assets.py` (either alone fails the asset build).

**2a — Dodge beats CC (passive).** "Perfect rolls win." While the Steadfast Greaves are
equipped, three linked effects (detected in the boots-passive dispatch — armor/ring/gloves/
**boots** legendary dispatch already exists):

1. **You may dodge even while stunned** — the deliberate exception to the stun's dodge-lock
   (Component 3). Without the boots, a stun locks dodge; with them, the roll is your escape.
   This is **never on an added cooldown** beyond the roll's own inherent recovery — a perfect
   dodge is a timing feat and is always rewarded.
2. **During the dodge i-frame window, incoming CC is negated** — `applyCCToPlayer` returns
   early while the roll's invuln is active, **gated on a boots-set `ccDodgeImmune` flag** (a
   base dodge does NOT shrug off CC — that would make CC universally weak and rob the boots of
   their identity). This is the "immune while dodging also negates CC" part: you must be
   mid-roll when the CC arrives, so it rewards timing.
3. **Starting a dodge clears active CC** — on the frame a roll begins, zero all three CC
   timers. So even a stun already ticking is shed by rolling.

(2) is prevention, (3) is cleanse; together a well-timed roll both dodges an incoming stun and
sheds one already on you.

**2b — "Break Free" active skill (F).** A new `SkillId::BREAK_FREE` with a `SkillDef` in
`skills.json` (cooldown ~20 s). It **rides the existing `BOOT_SKILL` rail** — `BOOT_SKILL` is
already bound to `SDL_SCANCODE_F` (`input.cpp:217`) and fully net-wired
(`INPUT_EX_BOOT_SKILL`, per-slot `m_bootSkillStates`, `bootSkillLastActivationTick`
replication). Its `tryActivate` branch: **cleanse all CC** (zero stun/slow/freeze) **+** set a
short **CC-immunity timer** (~1.5 s). No new key, no bespoke handler — F and the cooldown come
free. Tooltip text resolves through `HUD::resolveSkillDescription` (a def-having skill → text
lives in `skills.json`).

## Component 3 — Player-facing stun (new)

**Add `f32 stunTimer` to `Player`** (there is none today). Semantics: **action lock,
camera free** — while `stunTimer > 0`, suppress the movement, attack, skill-cast, and dodge
inputs in the player update / input consumption path, but **leave look/camera untouched**. The
one exception: the Steadfast Greaves passive lets a perfect dodge fire and clear it.

**Single choke helper.** Introduce `Combat::applyCCToPlayer(Player&, CcType, f32 duration)`
(or an engine-side equivalent) that ALL player-CC application routes through:

```
applyCCToPlayer(p, type, dur, isPvp):
    if p.ccImmuneTimer > 0: return          # Break Free / post-CC immunity blocks ALL CC
    if p.ccDodgeImmune and p.dodgeInvulnActive(): return  # boots 2a ONLY (not a base dodge)
    dur *= (1 - p.ccResist)                 # transient stamped field; 60% cap in Inventory::ccResist
    if type == STUN:
        if isPvp: dur *= advanceStunDr(p)   # Comp. 6: reads AND advances the DR counter/window
        p.stunTimer  = fmaxf(p.stunTimer, dur)
    elif type == SLOW:  p.slowTimer   = fmaxf(p.slowTimer,   dur)
    elif type == FREEZE:p.freezeTimer = fmaxf(p.freezeTimer, dur)
```

`advanceStunDr` both returns the current multiplier (1.0/0.5/0.25/0.0) **and** increments the
count + refreshes the 8 s window as a side effect, so the next stun in the window is weaker. DR
is applied to **PvP victims only** (`isPvp`); a stun on an enemy in PvE never diminishes.

The existing player-CC sites (`enemy_ai_states.cpp:556/558` slow/freeze, `projectile.cpp`
onHit slow/freeze, the arena PvP CC from Component 5) call this instead of writing timers
directly. DoT effects (poison/burn) and curse keep their direct writes.

**Stun source is PvP-only.** Enemies do not stun the player. Player stun is produced by the
new class CC landing on a rival player in the Arena (Component 5 via the `Combat::pvp*`
helpers → `Engine::pvpApplyHit`). In PvE, `applyCCToPlayer` still runs for slow/freeze, so
CC-Resist boots earn their slot in the campaign too.

## Component 4 — Block anti-abuse (PvP-scoped)

Today `classifyBlock` (`combat.h:67`) returns PERFECT (full negate) for the first 0.2 s of any
block and BLOCKED (−50% dmg) after — so holding block is a permanent 50% mitigation, and there
is no cost to turtling. **The design principle (from the user): a perfect block is a timing
feat and must ALWAYS be rewarded — never gated by a cooldown.** So the real abuse to fix is the
*cost-free held block*, addressed with a resource cost, not a lockout. **Both changes are gated
to PvP** (arena / `pvpActive`) so the campaign feel is untouched:

1. **Energy drain while held (the sole throttle).** Holding block drains energy per second in
   PvP; at 0 energy the block drops (input treated as released). Ticked in the player/block
   update. A genuine perfect block is a quick tap that costs negligible energy, so **timing is
   never punished** — only sustained turtling runs you dry. This replaces the perfect-block
   cooldown idea entirely: no `perfectBlockCd`, no PERFECT-classification gate. `classifyBlock`
   is unchanged (PERFECT stays available on every well-timed block).
2. **Only a perfect block negates CC.** In `landPvpHit`/`pvpApply`, a **perfect** block negates
   the hit's damage **and** its CC; a held/normal block stops damage per the −50% but **the CC
   still lands** (routes through `applyCCToPlayer`). This stops "hold block = immune to all CC"
   while making a timed block a genuine, repeatable skill answer to CC — exactly the reward the
   principle calls for.

## Component 5 — Class CC (baseline-up)

Each new CC also lands on **Arena players** by adding the matching `Combat::pvp*` twin beside
the entity query (the mandatory pattern from the engine-how-to pitfall — attacker slot from
`s_castingPlayer` on skill paths). Warrior, Paladin, Rogue, Sorcerer, Engineer are **unchanged**.

- **Ranger — Barrage → slow zone.** The Barrage rain leaves a lingering **~40% slow field**
  (~3 s) at its impact area; enemies/players inside are slowed (`applyCCToPlayer(..., SLOW)`
  vs players). Reuses the barrage placement; add a persisting zone tick (the scorch-zone
  pattern in `skill_warrior.cpp` is the model for a lingering AoE field).
- **Marksman — Explosive Round → knockback + stagger.** The explosion applies a **knockback
  impulse** (reuse `Combat::applyDamage`'s knockback path / a player displacement in PvP) plus
  a **0.2 s stagger** (a brief stun). Knockback is a displacement, **not** duration-based, so
  CC Resistance does not reduce it; only the 0.2 s stagger is resisted.
- **Tinkerer — Detonate Swarm → EMP stun.** On detonation, a **~0.6 s AoE stun** in the
  detonation radius (`applyCCToPlayer(..., STUN)` vs players; `stunTimer` vs enemies).
- **Wanderer — Deflect → stagger attacker.** A **successful Deflect** applies a **~0.75 s
  stun** to the attacker (reactive). Hooks the Deflect success path; in PvP the "attacker" is a
  rival player, stunned via `applyCCToPlayer`.

## Component 6 — Anti-chain CC (diminishing returns)

Prevents perma-stun in the Arena, on top of the 60% cap and the boots. **Player-victims only**
(so existing PvE Warrior/Paladin stun combos on enemies are untouched):

- Track per-player **stun DR state**: a count and an 8 s window timer (`f32 stunDrTimer`,
  `u8 stunDrCount`) on `Player`/`NetPlayer`.
- `stunDrScale`: 1st stun in the window ×1.0, 2nd ×0.5, 3rd ×0.25, 4th ×0.0 (brief immunity).
  Each new stun refreshes the 8 s window; when it lapses, the count resets to 0.
- **Slow/freeze are exempt** — only hard stun diminishes. A single stun always lands full.

---

## Networking / "powerfully wired"

Player CC is server-authoritative and replicated (the `shadowDanceTimer` pattern — done
correctly — is the template; **not** `overdriveTimer`, which has no `NetPlayer` field and is
broken in co-op):

- **New `NetPlayer` fields:** `stunTimer`, `ccImmuneTimer`, `stunDrTimer`, `stunDrCount`.
  (`bonusCcResist` and the `ccDodgeImmune` flag are recomputed server-side from the player's
  equipment like other bonus stats, not sent per-tick. No `perfectBlockCd` — perfect block has
  no cooldown; the energy drain rides the existing replicated energy value.)
- **Mirror in BOTH `seedRemoteView` and `writeBackRemoteView`** (`engine.cpp:~1317/~1351`) —
  `seedRemoteView` starts from `Player{}`, so anything unmirrored is zeroed every frame.
- **`SnapPlayer`:** the client must *adopt* stun/immunity for prediction (so a stunned client
  actually loses input locally, self-healing and mid-join safe). Reuse the free `statusFlags`
  bits + one quantized duration byte, exactly as the shrine buff did. The DR counters stay
  server-side (not predicted).
- Class CC vs players lands through `Engine::pvpApplyHit` (fresh `seedRemoteView` →
  land → `writeBackRemoteView`), so `applyCCToPlayer` runs on the authoritative view and the
  result replicates.
- **PROTOCOL_VERSION bump** (currently 20 → 21) because `SnapPlayer` gains the stun status
  bits + duration byte. A mismatch gives a clean `SV_JOIN_REJECT`.

## Performance requirements — the "Rocket League" bar

Non-negotiable: **60 FPS locked, 16.6 ms frame budget, tight predictive netcode, zero hitches
and zero rubber-banding**, on the low-end target (Core 2 Quad / Switch). Every part of this
feature is designed to cost effectively nothing per frame. Concrete constraints:

- **No heap in hot paths.** All new state is plain fields on the existing pool-allocated
  structs: CC timers are `f32` on `Player`/`NetPlayer`, DR is a `u8` count + `f32` window, the
  boots flags are `bool`. `applyCCToPlayer`, `advanceStunDr`, and the block energy tick are all
  **O(1), branchy-not-loopy, and allocate nothing**. No `new`/`delete`, no `std::vector`/
  `std::string`, no per-frame containers. Any transient scratch uses the 1 MB `FrameAllocator`.
- **Decay rides existing per-tick loops.** The CC timer decrements fold into the same
  `tickTimers` / player-update pass that already ticks `slowTimer`/`freezeTimer` — a few extra
  `f32` subtractions per player per 1/60 tick, invisible against the budget. Nothing polls or
  scans.
- **Ranger slow-zone is bounded, not a sweep.** The lingering field is a single capped AoE
  record ticked like the existing scorch-zone (`skill_warrior.cpp` pattern) — no per-cell
  iteration, no growth, no allocation; one radius test per already-queried candidate.
- **Draw-call & particle budget (300–500 calls) untouched.** Visual tells REUSE existing
  systems — the stun tell rides the entity status-tint / `s_novaCallback` path (the boss-shield
  tint precedent), the slow-zone uses the existing scorch/nova FX draw. **No new render pass, no
  new shader, no new GPU resource**, and emitter counts stay bounded so the particle pool never
  spikes.
- **Netcode: predictive, reconciled, delta-cheap (the RL-feel part).** The stun input-lock is
  applied **client-side predictively** — the client adopts `stunTimer` from `SnapPlayer` and
  locks its own input the same frame, so a stun feels instant, never a round-trip late — and it
  flows through the **existing rollback-replay reconciliation** (a stunned client's stored
  inputs replay through the identical server step), so there is **no rubber-band**. New
  `SnapPlayer` state is a stun event + a decaying quantized byte, so ack-driven **delta
  compression sends it ~once per CC, not every tick** (a byte or two per event). DR counters
  stay **server-side only** (not predicted), so they never create a client/server disagreement
  the reconciler must fight.
- **Wire discipline.** `SnapPlayer` growth is **bits in the existing `statusFlags` + one
  quantized duration byte** (the shrine-buff precedent), never new full fields, so the snapshot
  stays tiny and the `memcmp` delta stays cheap; `SNAP_PLAYER_WIRE`/`sizeof(SnapPlayer)` stay
  pinned by `static_assert`.
- **Measured, not assumed.** Wrap the CC choke + slow-zone tick in a `PROFILE_SCOPE`; confirm
  per-frame cost is within noise. Re-run the netcode soak rig (`--net-loss 15 --net-latency 100
  --bot-walk` + F9 net-graph) with CC live and confirm the measured envelope holds: **0 hard
  snaps, deltas engaged, steady snap-Hz, and a stunned guest with no visible rubber-band.**

## Testing

Pure logic first (the `game/` header + doctest pattern, forward-only):

1. **CC Resistance math** — `dur * (1 - resist)`; cap clamps at 0.60; boots+helmet+armor
   accumulation never exceeds the cap.
2. **Stun diminishing returns** — the 1.0/0.5/0.25/0.0 ladder; window refresh; reset after
   8 s; slow/freeze exempt.
3. **Break Free / immunity** — cleanse zeroes all three timers; immunity window blocks a new
   CC and expires.
4. **Block classify** — perfect stays available on every well-timed block (no cooldown); held
   block never negates CC; perfect negates both (PvP only); holding block drains energy to 0
   then drops.

Runtime (co-op is the real test): a stun on a guest actually locks the guest's input and shows
on their screen; CC-Resist boots visibly shorten a slow on the guest with no rubber-banding
(the reconciliation check); a perfect dodge in the Steadfast Greaves clears a stun; F cleanses
+ grants immunity on a cooldown; two CC classes focusing one Arena player hit DR immunity
instead of perma-lock; held block in the Arena still eats a stun while perfect block dodges it.

Performance gate (the RL bar, a hard release blocker — see the section above): with CC active
in a full Arena match, frame time stays within the 16.6 ms budget on the low-end target
(`PROFILE_SCOPE` on the CC choke + slow-zone tick shows within-noise cost), draw calls stay in
the 300–500 band, and the netcode soak (`--net-loss 15 --net-latency 100 --bot-walk`) shows 0
hard snaps, deltas engaged, steady snap-Hz, and a stunned guest with zero rubber-band. **A
regression on any of these fails the feature, regardless of a green test suite.**

## Risks / out of scope

- **PvP-scoping of block + DR** is deliberate (keeps the campaign untouched) but means block
  and DR behave differently in PvE vs PvP — documented, gated on `pvpActive`/arena.
- **Marksman knockback** is displacement, not a timed CC — outside the tenacity stat by design.
- **No innate class CC-resist** and **no PvE player-stun** — both explicitly deferred; can be
  added later if duels/campaign feel demand it.
- **Root** is not introduced (no chosen class CC needs it); the stat covers the three existing
  timers.
- `PROTOCOL_VERSION` 21 hard-breaks older clients — fine for a feature release, not a hotfix.
