# Legendary Armor & Shield Expansion — Design

**Date:** 2026-07-16
**Status:** Approved (brainstormed + effect picks locked with the user)

## Goal

The legendary pool has 49 defs but exactly ONE legendary armor (Demonhide Cuirass, blood_nova,
lvl 7–10) and ONE legendary shield (Aegis of Blood, blood_nova, lvl 7–10) — both slots go
legendary-dead after floor 10 and share one effect. Add 4 items that ladder those slots into the
mid-game and endgame bands, each with a distinct new effect, and make the shield effects real in
co-op by putting block state on the wire (today a guest's block does nothing server-side).

## Locked decisions (from the user)

1. Effects: **Hemophage Shroud + Capacitor Mail** (armors), **Mirror Aegis + Thunderwall** (shields).
2. **No anti-mash mechanism.** Block-mashing to fish for perfect blocks is allowed by design —
   no proc cooldowns, no block re-raise lockout. "If ppl want to exploit that let them."
3. Block mechanic feel untouched: 0.2 s perfect window, omnidirectional, mashable.
4. Save format untouched — **verified empirically 2026-07-16**: load→save round-trip of a real
   save is byte-identical (timestamp masked) with 4 defs appended to items.json. SAVE_VERSION
   stays 3.

## The items (items.json — APPEND-ONLY, defIds 196–199)

| Item | Slot | Band | legendarySkill | baseHealth | Notes |
|---|---|---|---|---|---|
| Capacitor Mail | armor | 16–28 | `static_charge` (new) | 55 | copper/arc-blue tint |
| Hemophage Shroud | armor | 32–50 | `hemophage` (new) | 90 | dark blood-red tint |
| Thunderwall | offhand | 16–28 | `chain_lightning` (existing) | 46 | storm-blue tint |
| Mirror Aegis | offhand | 32–50 | `projectile_parry` (new) | 75 | polished-silver tint |

- All: `maxRarity`/`minRarity` legendary, `dropWeight` 0.5 (matches existing legendaries).
  `baseHealth` sits ~7–10% above the RARE ceiling of the same band (Broodsilk 50 / Void Plate
  85 armor; Carapace 42 / Void Shield 70 offhand) — legendaries lead their band without
  invalidating the next one.
- **No new meshes** — reuse the `armor`/`shield` meshes. Distinct looks come from new
  materials.json entries (tint-only, referencing existing textures; id == array index).
- Item icons: slot-default glyphs (no bespoke icons — YAGNI).

## New SkillIds

Append `STATIC_CHARGE`, `HEMOPHAGE`, `PROJECTILE_PARRY` before `COUNT` (SkillId is serialized
by ordinal — append-only, never insert). They are pure passives with **no skills.json def**, so
their tooltips go in the C++ fallback table (tier 3 of `resolveSkillDescription`). Thunderwall
reuses `CHAIN_LIGHTNING` (the Aegis-of-Blood precedent: same SkillId, slot-specific meaning) and
therefore **needs a tier-1 OFFHAND slot override** — chain_lightning has a SkillDef, so tier 2
would otherwise show the castable-skill text on the shield tooltip.

## Effects

### Capacitor Mail — `STATIC_CHARGE` (armor-aura slot)

Each tick where the wearer took damage (`lastDamageTaken > 0`) adds 1 charge stack (max 5).
Stacks decay 10 s after the last hit (single shared timer, Frenzy pattern). At 5 stacks the
armor auto-discharges chain lightning into the attacker (`lastDamageAttackerIdx`; fallback:
nearest enemy within 5 m, thorns-style) and resets to 0. Discharge damage reuses the
CHAIN_LIGHTNING SkillDef's numbers; kill credit = wearer.

- Fields: `Player.chargeStacks` (u8) + `chargeTimer` (f32); same pair on `NetPlayer`; mirrored
  in `seedRemoteView`/`writeBackRemoteView` (anything unmirrored is zeroed every frame).
- Runs in `tickArmorRingPassives` (host/SP) AND the remote-player armor switch in
  `serverNetPost` (the Blood Nova pattern) so guests get it authoritatively.
- HUD: new status row + 8×8 glyph via `tools/gen_status_icons.py` (both halves of the
  row-indexed table, per the pitfall). Guest stack count replicates via 3 spare
  `SnapPlayer.flags` bits (5–7; that byte uses only 0/1/3/4 today) — zero wire growth.
  (`statusFlags` bits 5–6 are NOT free: the shrine-buff type lives there.)

### Hemophage Shroud — `HEMOPHAGE` (armor-aura slot)

Enemies within 4 m take periodic drain damage — 3 damage every 0.5 s (= 6 dps) per enemy,
ticked rather than per-frame to bound `applyDamage` calls — attributed to the wearer; the
wearer heals for 100% of damage actually dealt, capped at maxHealth. Heals are ratio-safe on
the wire (health goes up, maxHealth untouched — no HP-bar lurch). Same host/SP + serverNetPost
dual-site placement as Capacitor.

### Thunderwall — `CHAIN_LIGHTNING` on offhand (perfect-block dispatch)

A perfect block fires chain lightning starting at the attacker. Sourceless perfect blocks
(projectiles/AoE carry attackerIdx 0xFFFF) fall back to the nearest enemy within 5 m. No
internal cooldown (locked decision 2).

### Mirror Aegis — `PROJECTILE_PARRY` (reflect at the projectile-hit site)

`Combat::applyDamageToPlayer` gains a **BlockOutcome return** (`NONE`/`BLOCKED`/`PERFECT`).
The projectile-vs-player hit site in `projectile.cpp` reflects on PERFECT when the blocker's
offhand legendarySkillId is PROJECTILE_PARRY: the projectile survives, flips to
`fromPlayer=true` + `ownerSlot = blocker`, reverses velocity back along its incoming path, and
doubles its damage. `fromPlayer=true` projectiles never AABB-test players, so the reflected
shot cannot re-hit the blocker (or teammates). The perfect-block callback's PROJECTILE_PARRY
case is a no-op (reflect lives at the projectile site); melee perfect blocks fall through to
the existing generic freeze bash.

### Perfect-block callback rework (shared)

Signature extends to `(Player& player, u16 attackerIdx)` — attackerIdx is already a parameter
of `applyDamageToPlayer`, just never forwarded. The callback dispatches on the **blocker's**
inventory resolved by net slot, fixing the documented mis-indexing (it reads
`m_localPlayerIndex` today, so a remote's block would read the host's offhand). Shield FX for
guests broadcast via the existing event machinery (NOVA_FX pattern).

## Co-op enablement — block on the wire

Today `NetInput` has no block bit, so `np.blocking` is never set for remotes: the server
applies full damage to a blocking guest and the 0.4× block move-slow mispredicts every tick.
Fix at the protocol level:

- Reuse the reserved dead bit: `INPUT_LOCK` (1<<6) → `INPUT_BLOCK`. Its comment explicitly
  blesses reuse; the flags-byte layout — and thus the wire format — is unchanged.
- Client sets the bit from the BLOCK action. Server drain (`updateNetPlayerFromInput`):
  edge-detect block start → `blocking=true, blockTimer=0`; tick blockTimer while held; clear on
  release. Apply the 0.4× move-slow there so prediction, replay, and server agree (this also
  fixes the pre-existing guest block rubber-band).
- `seedRemoteView` already mirrors `blocking`/`blockTimer` into server-side views, so guest
  damage negation works as soon as `np` is fed.
- `SnapPlayer.flags` bit 4 (blocking) already replicates for poses; its bits 5–7 gain the
  charge stack count. (`statusFlags` bits 5–6 belong to the shipped shrine-buff type — the
  charge count deliberately rides the OTHER flags byte.)
- `PROTOCOL_VERSION` 18 → **19**: no packet changes size, but the semantic change makes
  mixed-build sessions silently inconsistent — the bump turns that into a clean join-reject.

## Explicitly untouched

- **Save format**: SAVE_VERSION stays 3. ItemDefs are JSON-loaded (never serialized); new
  Player/NetPlayer fields are runtime-only; new SkillIds append. Verified by byte-identical
  round-trip test.
- **Block feel**: window, omnidirectionality, mashability — all as-is.
- **controls.json** (BLOCK action already exists) and all other prefs.

## Testing

- **Unit**: charge accumulate/discharge/reset (pure logic); `applyDamageToPlayer` BlockOutcome
  for none/normal/perfect; projectile reflect (owner flip, direction reversal, 2× damage,
  blocker immunity); INPUT_BLOCK round-trip in the existing input-wire test; legendary-pool
  test green with the 4 new defs.
- **Build**: debug + release + steam clean; full suite.
- **Runtime SP**: each of the four effects observable in-game.
- **Runtime co-op (the real test)**: guest blocks with no rubber-band (the reconciliation
  check); guest perfect-block procs fire authoritatively with FX on both screens; guest's
  Capacitor HUD shows stacks; guest Hemophage heals; a reflected projectile's kill credits the
  blocker.

## Risks

- Chain-lightning-from-a-proc needs a reusable fire entry point that doesn't assume a weapon
  cast; the plan must locate/extract it (Crown of Storms machinery is the trailhead).
- Reflected projectiles: verify the reversed velocity doesn't immediately re-collide with level
  geometry at the block position (nudge the spawn point along the reversed direction if so).
- Hemophage drain rate is a balance guess (~6 dps/enemy at 32–50); expect tuning after playtest.
