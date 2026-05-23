# Hit Feedback — Design Spec

**Date:** 2026-05-23
**Status:** Approved concept, pending implementation plan
**Targets:** Nintendo Switch + low-end PC. 60 FPS, 16.6 ms budget. Co-op (couch + listen-server) safe.

## Context & Goal
Combat currently lands without much "punch." The engine already has the raw ingredients but uses them sparingly and ad-hoc: `ScreenShake` (camera.h), a crosshair hit marker (`m_hitMarkerTimer`), entity white-flash on hit (`flashTimer`), floating damage numbers (`spawnDamageNumber`, with `isHeal`/`isCrit`), and a batched particle system (`spawnBlood/Sparks/Explosion/Debris/Smoke`). Knockback exists only for a couple of skills; there is **no** hit-stop, no knockback on normal hits, no crit/kill emphasis, and thin take-damage feedback.

Goal: make hitting and being hit feel impactful by **unifying and amplifying** these primitives behind one coherent, tunable system — not by scattering more trigger calls (the codebase audit already flagged scattered `shake.trigger` calls and triplicated weapon-fire code).

**Explicitly deferred:** hit-stop / freeze-frame. It interacts badly with the co-op listen-server (freezing the sim freezes everyone), so v1 leaves a hook (`hitStopMs` in the tier table) and ships without it.

## Approach
**Centralized, event-driven feedback fed from the two combat choke points, with a tunable per-tier table.**

- Cosmetic feedback is emitted as a single `HitEvent` from the two places damage is already resolved — `Combat::applyDamage` (you hit something) and `Combat::applyDamageToPlayer` (you got hit). One engine-side handler fans out to shake/particles/numbers, mirroring the existing `s_damageNumberCallback` / `s_deathCallback` registration pattern.
- **Authoritative** feedback (knockback = entity velocity change) is applied *directly inside* `applyDamage`, not in the cosmetic handler — so it's server-authoritative and clients receive it through the normal snapshot + interpolation path (MP-correct for free).
- Per-tier parameters live in **one `constexpr` table** in `game/game_constants.h`. No new JSON loader (YAGNI); the table is the single place to tune game feel.

Rejected alternatives: inline triggers at each hit site (scattered spaghetti, and `handleWeaponFire` is triplicated so it'd be written 3×); a full JSON-driven feedback config (over-engineered for ~4 tiers).

## Impact-Tier Model
Every hit is classified **once**, inside `applyDamage`, into one tier that selects a feedback recipe. Because classification is at the single choke point, it applies uniformly across melee / hitscan / projectile / DoT.

| Tier | Trigger | Recipe (target being hit) |
|---|---|---|
| **LIGHT** | normal hit below heavy threshold | white flash · tiny shake (only if local player dealt it) · small blood/spark puff · normal number |
| **HEAVY** | damage ≥ threshold (% of target max HP) | flash · medium shake · **knockback impulse** · larger blood burst · bold number |
| **CRIT** | `isCrit` flag (reuses existing crit path) | crit-styled larger number · yellow spark burst · medium-heavy shake · medium knockback |
| **KILL** | hit drops target to 0 HP | strongest shake · debris + smoke burst · strong knockback · *(reserved: `hitStopMs`)* |

Tiers combine sensibly: a crit that kills uses the KILL recipe **plus** the crit-styled number. Classification is pure (damage + flags in, tier out) and unit-testable.

## Knockback (outgoing hits only)
- Applied in `Combat::applyDamage` as a horizontal velocity impulse along `normalize(targetPos - damageOrigin)`. `applyDamage` already accepts an optional `damageOrigin`; melee/hitscan/projectile pass it. **No `damageOrigin` (DoT/environmental) → no knockback.**
- Magnitude from the tier table, then scaled by entity mass/size: larger `halfExtents` → less push. **Bosses (`bossDefIdx != 0xFF`) get ~0 knockback** and a stronger flash/shake "stagger" substitute instead (a sliding boss reads wrong).
- The impulse is added to `Entity.velocity` and carried/decayed by the existing `Collision::moveAndSlide`. *To verify in planning:* whether entity velocity already damps; if not, add a short knockback decay so pushed enemies settle.
- **The player is never positionally knocked back** by enemy hits (losing control feels bad). The player's hit reaction is camera-kick + vignette (below). A future opt-in could add knockback for specific boss slams — out of scope for v1.

## Take-Damage Feedback (player gets hit) — local, per-client
Emitted from `applyDamageToPlayer` (which already receives `attackerPos`). All effects are client-local and cosmetic.
- **Red vignette pulse** — fullscreen overlay, intensity ∝ damage/maxHP, quick fade. Reuse the existing fade-overlay plumbing (`m_fadeFromBlack`: unlit shader + quad).
- **Directional damage indicator** — an edge wedge/arc pointing toward `attackerPos`, fading over ~1 s. HUD element.
- **Camera kick** — a short inward/downward punch, lighter and distinct from the outgoing-hit shake so "dealing" and "taking" feel different.
- **Controller rumble** — short pulse scaled by damage. *To verify in planning:* gamepad rumble support in the Input/platform layer (e.g. `SDL_GameControllerRumble`); Switch supports it.
- *Optional (cheap, high-value):* subtle persistent vignette + slow heartbeat under ~25% HP as a "danger" state. Include if low-cost; otherwise defer.

## Architecture / Components
- **`HitEvent`** (plain struct): `Vec3 pos; Vec3 dir; f32 damage; ImpactTier tier; bool isCrit; bool isKill; u8 dealtByLocalPlayer;` — cosmetic payload.
- **Tier classification** — a small pure function in `Combat` (or alongside the tier table): `(damage, targetMaxHp, isCrit, isKill) -> ImpactTier`.
- **Tier table** — `constexpr HitFeedbackTier kHitTiers[4]` in `game_constants.h`: `{ f32 shakeIntensity, shakeDuration, knockback; u8 bloodCount, sparkCount, debrisCount; f32 hitStopMs /*=0 for now*/ }`.
- **Cosmetic handler** — engine-side callback (registered like the existing damage-number/death callbacks) that reads the tier recipe and calls `ScreenShake::trigger`, the `Particles::spawn*` functions, and `spawnDamageNumber` (crit/kill styling). Lives with the other engine FX dispatch, not scattered in combat.
- **Knockback** — applied directly in `applyDamage` (authoritative), gated by tier + `damageOrigin` + boss/size resistance.
- **Take-damage** — handler off `applyDamageToPlayer`: vignette state, directional-indicator state, camera kick, rumble.

Data flow: `Combat::applyDamage` → classify tier → apply knockback (authoritative) → emit `HitEvent` → engine handler → shake/particles/number. `Combat::applyDamageToPlayer` → emit player-hit event → vignette/indicator/kick/rumble.

## Performance (Switch)
- Particles are already batched into a single draw call; counts are capped per tier and scaled down on Switch.
- Shake/vignette are uniform/overlay updates; the vignette is one fullscreen quad (reuse fade overlay). Directional indicator is a handful of HUD verts. Rumble is one SDL call.
- Knockback is a velocity add — no allocation, no extra pass.
- No per-frame heap allocation anywhere. Comfortably within 16.6 ms.

## Co-op / MP correctness
- Knockback is authoritative (in `applyDamage` on host/SP) → clients get it via snapshots; no desync.
- All cosmetic feedback (shake, particles, numbers, vignette, indicator, rumble) is per-client local and reads from the events/snapshots each client already has.
- Hit-stop is omitted precisely because it can't be made co-op-safe cheaply; the `hitStopMs` table field is the single switch to enable an SP-only version later.

## Out of Scope (v1)
- Hit-stop / freeze-frame (hook reserved).
- Player positional knockback from enemy hits.
- New audio (combat SFX is a separate, parallel improvement).

## Verification
- **Tier classification:** pure-function unit checks (light/heavy/crit/kill boundaries).
- **In-game, dealing damage:** light hits → flash + small particles + number; heavy hits → shake + visible knockback + blood; crits → styled number + sparks; kills → debris + smoke + strong shake. Bosses don't slide (stagger instead).
- **In-game, taking damage:** vignette + edge indicator points at the attacker; camera kick; rumble on gamepad; danger state under 25% HP (if included).
- **Co-op:** host + client both see correct knockback (entity ends up in the same place on both ends) and each gets their own local screen feedback; no stutter or desync introduced.
- **Perf:** profiler frame stays within budget during a 50-enemy fight (F5 spawn) with heavy crits/kills firing.
