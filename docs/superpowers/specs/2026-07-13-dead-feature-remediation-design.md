# Make the Game Do What It Says — Dead-Feature Remediation + Contract Harness

**Status:** approved design, ready for planning
**Date:** 2026-07-13

---

## Context

An audit of the codebase found roughly **25 features that exist, look live, and do nothing**. This
is not a list of unrelated bugs. Every one of them has the same shape:

> **A producer was written — a JSON key, an enum value, a callback setter, a packet type — and the
> consumer was either never written, or was later replaced without deleting the producer.**

The loaders are all *forgiving* (`entry.value(key, default)`), so **a field nothing reads is
indistinguishable from a field that works.** Nothing fails to compile. No test fails. The data
simply lies, quietly, for months.

We know the sample is not the population. In the week before this audit, reading code (not playing
the game) turned up nine more of the same class: the quickbar's click targets never overlapped the
drawn bar at any resolution; the mouse wheel never worked at all; the quickbar was unusable on the
entire Switch build; three skills were uncastable; Thunderclap's "upgrade" made it *worse*; Poison
Arrow applied no poison; tooltips described behaviour the code never implemented; a guest's weapon
swap silently dealt the old weapon's damage.

CLAUDE.md's testing policy says tests are "forward-only on new code — no backfill for existing
combat/AI/item-gen/render." That is precisely where all of these live.

**So the goal of this work is not "fix 25 bugs." It is to make this entire class of defect
mechanically detectable, then drive the count to zero and keep it there.**

### Verified before writing (do not re-litigate these)

Each was confirmed by grep against the tree at commit `55a572e`:

| Claim | Evidence |
|---|---|
| Boss VFX callback never registered | `grep -r 'BossAI::' src/engine/` → **0 hits** |
| `bonusDamageToFlying` has no reader | 0 readers outside loader/tooltip/save-mirror |
| `hasAuraBuff` is not replicated | 0 hits in `src/net/` |
| `aiPreference` has no reader | The **only** hit outside the loader is the *comment* at `engine_spawn.cpp:292` claiming it reads it |
| Phase Dash `invulnDuration` has no reader | 0 readers |
| `WorldItemSystem::spawn` return ignored | 0 checked call sites in `engine_death.cpp` |
| `EnemyDef::dropWeight` has no reader | 0 readers |
| `setMagicBurstCallback` never called | Only the declaration and the definition exist |

---

## Architecture: build the checker first

The spine of this work is a **contract checker**, not a list of fixes. Written first, it *generates*
the backlog; the fixes are then whatever it takes to get it green.

This ordering is the whole design. A hand-carried list of 25 items is a snapshot that rots the day
it's written and cannot tell you when you're done. A checker is a ratchet: it enumerates the work,
proves each fix, and fails CI the next time someone adds a producer without a consumer.

### `tools/check_data_contracts.py` (new), run in CI

Four mechanical rules, each targeting one producer/consumer pair:

1. **JSON → reader.** For every field of `SkillDef` / `EnemyDef` / `ItemDef` / `AffixDef` / `BossDef`
   that a loader parses, there must be at least one read site *outside* the loader and outside the
   struct declaration.
   *Catches:* `aiPreference`, `dropWeight`, `invulnDuration`, `corridorWidth`, `slashRadius`,
   `slashDamageMult`, Deflect's `stunDuration`, Mech Overdrive's `duration`.

2. **Callback wiring — both ends.** A callback is only alive if it is *both* registered *and*
   invoked. Check both directions, because the two failures look identical from the player's side:
   - every `set*Callback` setter must have a call site that registers a non-null callback;
   - every callback slot must have at least one invocation site.

   *Catches:* `BossAI::setMagicBurstCallback` (never registered → 8 no-op call sites),
   `BossAI::setDamageNumberCallback` (**neither** registered nor invoked), and
   `ProjectileSystem::setDamageNumberCallback` (registered, **never invoked** — which the
   registration-only half of this rule would have missed).

3. **Wire liveness.** Every `NetPacketType` value must be both sent and handled. Every `INPUT_*` /
   `INPUT_EX_*` flag must be both set and read.
   *Catches:* `CL_EQUIP_ITEM`, `SV_INVENTORY_SYNC`, `INPUT_FIRE`, `INPUT_EX_INVENTORY`,
   `INPUT_LOCK`, and the phantom `SV_SKILL_RESULT` (referenced in four comments; **not an enum value
   at all**).

4. **Reachability.** Every `SkillId` with a `skills.json` def must be in some `ClassDef.skills[4]` or
   be some item's `legendarySkill`. Every `AffixType` that can roll must have a consumer in the
   damage/stat pipeline.
   *Catches:* the 12 legacy skills, Cleave, Second Wind, `of the Hawk`.

A fifth rule is **explicitly out of scope for automation** because it cannot be checked by grep:
*"a field that drives a render tint must be on the wire."* This is what makes `hasAuraBuff` invisible
to guests. We fix the instance by hand and record the rule in the knowledge skills, rather than
pretending a script can catch it.

### The allowlist

`tools/dead_code_allowlist.json` — every intentionally-dead entry, with a **reason** and the date it
was allowed. This is the mechanism that turns "dead" from *an accident nobody noticed* into *a
decision someone signed.*

The 12 legacy skills go here (per decision below), as does anything the checker surfaces that is out
of scope for this pass.

### Scope control

The checker will very likely find **more than the 25 already known**. The terminating rule:

- **Everything player-visible gets fixed in this spec.**
- **Everything else gets allowlisted with a written reason** and a follow-up note.

Without that rule this spec never finishes.

---

## Part 1 — Player-visible fixes

These are the ones a player can currently *experience as a lie*.

### 1.1 Every boss VFX telegraph is a silent no-op

`BossAI::setMagicBurstCallback` (`src/game/boss_ai.h:32`, `boss_ai.cpp:21`) is **never called by the
engine**. `s_magicBurstCb` is therefore permanently `nullptr`, and `BossAI::magicBurst()`
(`boss_ai.cpp:22-24`) is a guarded no-op at **8 call sites** in `src/game/enemy_ai_boss.cpp`
(lines 73, 104, 122, 190, 214, 233) and `boss_ai.cpp:48-50`.

Those calls are the **teleport telegraph, the summon flash, the "recompile" flash, and the
minion-shield burst**. So bosses — the audit's own verdict is that they are the game's strongest
content — currently blink, summon and shield **with no visual tell whatsoever.**

**Fix:** register both `BossAI` callbacks in `engine_init_callbacks.cpp` alongside the existing FX
callbacks. `BossAI::setDamageNumberCallback` (`boss_ai.cpp:27-28`) is doubly dead — never registered
*and* never invoked — so it needs a registration **and** call sites, or deletion. Prefer
registration: boss skill damage currently produces no floating number at all.

### 1.2 The "of the Hawk" affix does nothing

`AffixType::DAMAGE_TO_FLYING` is loaded (`item_loader.cpp:165`), rolls freely (`item_gen.cpp:105-115`),
accumulates into `PlayerInventory::bonusDamageToFlying` (`inventory.cpp:49`), and is shown in the
tooltip as `+Dmg vs Flying` (`hud_inventory.cpp:29`).

**No damage calculation anywhere reads it.** A player can equip it, read the stat, and receive
nothing. `affixes.json:73-79` rolls it on weapon+ring at 10–40.

**Fix:** apply it in `Combat::applyDamage` when the target has `ENT_FLYING`.

### 1.3 Phase Dash grants no invulnerability

`skills.json:51-60` declares `"invulnDuration": 0.3` and `"corridorWidth": 1.0`. Both are parsed into
`SkillDef` (`item_loader.cpp:407-408`) and **neither is read**. The implementation
(`skill_legendary.cpp:200-228`) hardcodes a 30° cone and sets **no i-frames at all**.

**Fix:** read both from the def; grant the i-frames the tooltip promises.

### 1.4 The aura-buff tell is invisible in co-op

`Entity.hasAuraBuff` drives a red-orange tint at `engine_render_entities.cpp:172-176`, but the field
is **not in `SnapEntity`** — so on a guest's screen, an aura-buffed enemy looks identical to a normal
one. The host sees the tell; nobody else does.

The correct pattern already exists two lines below: the boss shield/invuln tint
(`engine_render_entities.cpp:178-192`) is driven purely by wire-synced data and is explicitly
commented *"so host and clients match."*

**Fix:** replicate it. `Entity.flags` is copied verbatim into `SnapEntity.flags`
(`snapshot.cpp:135`), and **`ENT_` bits 5/6/7 are free** — so an `ENT_AURA_BUFFED` bit reaches the
client with **no wire-size change and no protocol bump.**

### 1.5 Delete Cleave and Second Wind

Both are fully implemented, tooltipped, and unobtainable.

- **Second Wind** — working ring-passive implementation (`engine_update_skills.cpp:350`,
  `engine_net.cpp:653`), a tooltip promising *"Below 20% HP: heal 30% and gain 1.5s invulnerability"*
  — and `grep second_wind assets/config/items.json` → **0 hits.** No item grants it.
- **Cleave** — a fire branch (`skill_system.cpp:415`), a `skills.json` entry, and **its own
  hand-drawn 32×32 HUD icon** (`kIcon32_Cleave`). The Warrior kit is
  `{THUNDERCLAP, WAR_CRY, WHIRLWIND, EARTHQUAKE}`; no item grants it. That icon can never appear on
  screen.

**Decision: delete both** — implementations, tooltips, `skills.json` entries, Cleave's icon, and the
`item_loader` string-map entries.

> **CRITICAL:** the `SkillId` **enum values must NOT be deleted.** `SkillId` is serialized by ordinal;
> removing one shifts every later skill and corrupts every existing save. Delete the *implementation*,
> keep the *enum value*, and allowlist the now-orphan enum entries with the reason "kept for save
> compat".

---

## Part 2 — Wire up the data the game already authored

Each of these is content the designer wrote, which the code silently ignores.

### 2.1 `aiPreference` — the single cheapest gameplay win in the codebase

`engine_spawn.cpp:292` reads:

```cpp
// Set initial AI state from JSON aiPreference
```

…and the code directly beneath it branches on `def.role & EnemyRole::AMBUSH` instead. **That comment
is the only "reader" of `aiPreference` in the entire tree.**

`enemies.json` authors a non-default preference on **17 of 38 enemies** — `strafe`×7, `retreat`×5,
`flank`×2, `dormant`×2, `surround`×1 — and every one is discarded. The parse is already correct
(`enemy_loader.cpp:43-53` maps the string to an `AIState`).

**Fix:** `ent->aiState = static_cast<AIState>(def.aiPreference);` at that comment, in **both** the JSON
and fallback spawn paths. One line. Seventeen enemies begin fighting the way they were designed to.

This matters beyond its size: it is the reason the game reads as "everything just runs at you," and it
is a hard prerequisite for the parked champion-affix work (an affix multiplies base behaviour; if all
base behaviour is identical, affixes only add hazards, not tactics).

### 2.2 `EnemyDef::dropWeight` — parsed, never read

Present on **all 38** enemies. Loot chance is a flat formula in `engine_death.cpp:489`
(`LOOT_DROP_CHANCE + enemyLevel*0.01`, capped 0.70). An enemy authored as a rich drop source drops
exactly like every other enemy.

*(Note the name collision that hid this: `ItemDef::dropWeight` **is** used, at `item_gen.cpp:182`.)*

**Fix:** factor `EnemyDef::dropWeight` into the drop-chance calculation.

### 2.3 Three more ignored tuning values

- **Death's Dance** — `slashRadius: 3.0`, `slashDamageMult: 1.0` parsed at `item_loader.cpp:423-424`,
  never read; `engine_init_callbacks.cpp:545-573` hardcodes both. The values *happen to match today*,
  so tuning the JSON silently does nothing. This is the most insidious kind: it will look like it
  works right up until someone changes a number.
- **Deflect** — `skills.json:470` sets `"stunDuration": 2.0` and the field comment says *"stun applied
  on melee parry"*. The deflect path (`combat.cpp:263-268`, `engine_update_player.cpp:89-125`) applies
  **no stun anywhere**.
- **Mech Overdrive** — `duration: 5.0` ignored; `fireMechOverdrive` (`skill_engineer.cpp:120-127`)
  hardcodes `5.0f`. Separately, `player.h:56` and `skill_engineer.cpp:119` both claim a *"damage/speed"*
  buff, but `overdriveTimer` is only ever consumed for **move speed** (`engine_update.cpp:1152`,
  `*= 1.3f`). The `skills.json` description is honest; **the C++ comments are the lie** — fix the
  comments, don't invent a damage component.

---

## Part 3 — Shrines: finish the half-built feature

`Player::shrineBuff` / `shrineBuffValue` (`player.h:64-65`) are **read but never written.**
`engine_update.cpp:1151` already applies a speed multiplier for `shrineBuff == 2`. Values `1` (power)
and `3` (vitality) have **no reader at all**. There is no shrine entity, prop, or spawn code anywhere
in `src/` or `assets/`.

So half the consumer exists, and the feature does not.

**Decision: implement shrines.** Shrine props placed during floor generation, granting one of three
timed buffs — power (1), speed (2), vitality (3) — completing the consumers for all three values.

This is the one place this spec adds new content rather than removing lies, and it earns its place:
it *completes* a feature the code already half-believes in, rather than leaving a stray `if` statement
that reads like a bug forever.

*(Also stale: `skill_warrior.cpp:102`'s comment says War Cry works "via shrine speed buff". It does
not — it uses `overdriveTimer`, which works fine. Fix the comment.)*

---

## Part 4 — Delete the dead

### 4.1 Unreachable fallback tables — and make load failure fatal

- `kBosses[10]` (`engine_spawn.cpp:167-181`) — reachable only if `findBossDefIdx` fails, but
  `bosses.json` covers **exactly** the floors it covers (5,10,…,50). Entirely dead. It contains
  **Andariel, Mephisto, Baal, Lich Lord, Spider Queen, Demon Knight, Arch Mage** with the wrong meshes.
- `kTier1`–`kTier5` (`engine_spawn.cpp:80-152`) — reachable only if `collectTierDefs` returns 0, but
  `enemies.json` populates all five tiers.

**Decision: delete both, and make a failed content load FATAL.**

The rationale is worth stating plainly, because it looks like we're removing a safety net. We are —
but that net does not save the player; it silently ships **the wrong game**, spawning placeholder
bosses under the wrong names and meshes. **A loud crash beats quietly shipping the wrong content.**
Fail fast at load with a clear error.

### 4.2 Dead wire surface

| Symbol | State |
|---|---|
| `CL_EQUIP_ITEM = 0x03` (`net.h:86`) | Declared. Never sent, never handled. |
| `SV_INVENTORY_SYNC = 0x16` (`net.h:119`) | Declared; the comment itself says *"reserved, unused"*. |
| `SV_SKILL_RESULT` | **Does not exist.** Referenced in 4 comments (`pending_skill_ring.h:6,:45`, `engine_update_skills.cpp:238`, `engine.h:359`) — not an enum value, never sent, never parsed. |
| `PendingSkillRing` | **Write-only.** `record` is called 3×; `ack()` and `expireOlderThan()` are called by **nobody**. An entire `.h`/`.cpp` pair whose only effect is burning 16×8 bytes. |
| `INPUT_FIRE` (bit 5) | Set every tick at `player.cpp:370`; **zero read sites**. Firing goes through `CL_FIRE_WEAPON` now. Transmitted at 60 Hz to no one. |
| `INPUT_EX_INVENTORY` (bit 5) | Set at `player.cpp:425`, explicitly preserved in the server's mask (`server.cpp:49`), and **never tested**. |
| `INPUT_LOCK` (bit 6) | Already correctly labelled RESERVED (fixed this week). |

**Fix:** delete the dead packet types and the `PendingSkillRing` pair; stop *setting* the dead input
bits and mark them RESERVED.

> **Wire-safety note:** removing a `NetPacketType` enum *value* is safe **only if every remaining
> value keeps its numeric id** — do not renumber. Ceasing to set an `INPUT_*` bit changes no layout
> (the bit simply reads 0), so **no `PROTOCOL_VERSION` bump is required** for any of this.

### 4.3 Orphan fields and silent loot loss

- `WeaponState::recoilOffset` (`weapon.h:53`) — zero reads, zero writes. Real recoil is
  `ViewmodelState::recoilKick`.
- `PlayerInventory::bonusRange` / `EquipmentStats::bonusRange` — never accumulated, never read.
  Fossil of `AffixType::_REMOVED_RANGE_BONUS`. *(The enum value stays — save compat.)*
- `ProjectileSystem::setDamageNumberCallback` — registered at `engine_init_callbacks.cpp:407` and
  **never invoked**. Redundant rather than broken (numbers do appear, via `s_hitCallback`), so delete
  the setter and its dead lambda.
- **`WorldItemSystem::spawn` fails silently.** It returns `false` when the 32-slot pool is full
  (`world_item.cpp:43-68`) and **every caller in `engine_death.cpp` ignores the return value.** Loot
  simply vanishes. Compounding it: items self-expire after 60 s — **except LEGENDARY, which never
  despawns**, so the pool can fill with legendaries and then eat every subsequent drop.
  **Fix:** check the return; log and, at minimum, don't silently lose a guaranteed drop.

---

## Part 5 — Allowlist the 12 legacy skills

`MULTI_SHOT`, `RAIN_OF_ARROWS`, `POISON_ARROW`, `SHADOW_SHOT`, `KNIFE_BURST`, `SHADOW_STRIKE`,
`CONSECRATION`, `DIVINE_SHIELD`, `RAPID_FIRE`, `COMBAT_DRONE`, `SWARM_DRONES`, `STUN_GRENADE`.

All are implemented with `skill_system.cpp` fire branches, all have `skills.json` defs, none is in a
class kit or on an item. They are marked `// legacy — kept for save compat` in the enum, so their
deadness is *partly* intentional.

**Decision: allowlist as intentionally dead.** They stay exactly as they are, recorded in
`tools/dead_code_allowlist.json` with the reason "legacy — kept for save compat; unreachable by
design". This gets the checker to green **honestly**, without a risky deletion pass and without
pretending ~25% of `skills.json` is live content.

---

## Testing

The checker **is** the primary test, and it runs in CI. Beyond it:

1. **Unit tests for each newly-live behaviour** — these are the regression guards that prove the fix
   actually reached the player:
   - damage-vs-flying multiplier applies to `ENT_FLYING` targets and only to them;
   - Phase Dash sets i-frames for `invulnDuration` and clears them after;
   - `aiPreference` maps each JSON string to the correct initial `AIState` (a pure function — test all
     8 values);
   - `dropWeight` shifts drop chance in the expected direction;
   - `WorldItemSystem::spawn` reports failure on a full pool, and the caller notices.
2. **The checker's own tests** — feed it a synthetic producer with no consumer and assert it fails.
   A silent checker is worse than none.
3. **Green checker = definition of done.**

## Docs to update

- **CLAUDE.md** (Conventions): *"A JSON key with no reader is a bug. Adding a field to a `*Def` means
  adding a reader AND a test in the same change. `tools/check_data_contracts.py` enforces this in CI."*
- **`engine-how-to`** (Pitfalls): the producer-without-consumer trap, its four shapes (JSON key,
  callback setter, wire enum, unreachable content), and the un-automatable fifth: **a field that drives
  a render tint must be on the wire, or the tell is invisible to every guest** (`hasAuraBuff`).
- **`engine-reference`**: the allowlist's location and what belongs in it.

## Risks

- **Deleting the fallback tables removes a safety net.** Deliberate — replaced by a fatal load error.
  A crash is strictly better than silently shipping Andariel-with-the-wrong-mesh.
- **`SkillId` / `AffixType` enum values must never be deleted** — serialized by ordinal; deleting one
  corrupts every save. Delete implementations, keep enum values, allowlist the orphans. This is the
  single highest-consequence mistake available in this work.
- **The checker will find more than 25.** Expected. The terminating rule (fix player-visible;
  allowlist the rest with a reason) is what makes this spec finite.
- **`aiPreference` changes enemy behaviour on every floor.** 17 enemies will start strafing, retreating
  and flanking who previously charged. This is the *point*, but it is a real difficulty change and
  needs a play-test, not just a green test suite.
- **No `PROTOCOL_VERSION` bump is needed** for anything in this spec — the `ENT_AURA_BUFFED` tell uses a
  free `Entity.flags` bit that is already replicated verbatim. Keep it that way; if a change starts
  demanding a bump, that is a signal the scope has drifted.

## Explicitly out of scope

- **Champion/elite monsters with rolled affixes, and the floor-event framework + loot goblin.** Parked,
  not cancelled. They will land far better on enemies that actually fight differently (Part 2.1), and
  on a codebase where a new `EnemyDef` field cannot silently do nothing.
- **`assets/config/weapons.json`** — never loaded by any code; an inline 3-weapon table
  (`weapon.h:77-108`) wins. Left alone by explicit decision; **allowlist it** so the checker doesn't
  reopen the argument.
- **A boss health bar / champion nameplate** — does not exist today; not needed here.
