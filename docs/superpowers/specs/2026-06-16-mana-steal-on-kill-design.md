# Mana Steal & Mana on Kill — Design

## Context

Skills ("spells") are powered by the engine's **energy** resource (`SkillState.energy` /
`maxEnergy`; the code already calls spending it "spent mana", e.g.
[engine_update_skills.cpp:258](../../../src/engine/engine_update_skills.cpp#L258)). Today the only
energy affix is `ENERGY_FLAT` (bigger pool) — there is **no way to *refuel* energy through combat**,
so caster/hybrid builds run dry. HP, by contrast, has both `LIFESTEAL_PCT` (% of damage → HP) and
`LIFE_ON_HIT` (flat HP per hit).

This feature mirrors that sustain for energy:

- **Mana Steal** — restore energy equal to a % of weapon damage dealt.
- **Mana on Kill** — restore a flat amount of energy on each kill.

**Spell rule (clarified during design):** lifesteal stays exactly as it is, and **manasteal behaves
identically to lifesteal**. Concretely: neither steal procs on the **direct-damage** skills
(automatic — those skills call `Combat::applyDamage` and never touch the steal code), and both proc
on weapon melee/hitscan **and** on projectile hits (including skill-spawned projectiles — matching
lifesteal's *current* behavior). We deliberately do **not** add weapon-vs-skill projectile gating.
**Mana on Kill** fires on **any** kill (weapon or spell) — it is an on-kill reward, separate from the
steal rule.

## Existing systems reused (mirror these)

- **Affix pipeline:** `AffixType` enum ([item.h:49](../../../src/game/item.h#L49)) — append-only,
  serialized by integer; `affixTypeFromString` ([item_loader.cpp:158](../../../src/game/item_loader.cpp#L158));
  [affixes.json](../../../assets/config/affixes.json).
- **On-demand affix sum (save-safe):** `Inventory::lifestealPct` via `sumEquippedAffix`
  ([inventory.cpp:70](../../../src/game/inventory.cpp#L70)). No cached `PlayerInventory` field → **no
  `SAVE_VERSION` bump**.
- **Lifesteal application sites** (where mana steal slots in beside the HP heal):
  - melee/hitscan, local: [engine_combat.cpp:658](../../../src/engine/engine_combat.cpp#L658)
  - melee/hitscan, remote NetPlayer: [engine_combat.cpp:1357](../../../src/engine/engine_combat.cpp#L1357)
  - projectile hit (deferred), local-lane + remote routing:
    [engine_init_callbacks.cpp:197](../../../src/engine/engine_init_callbacks.cpp#L197)
- **Kill hook:** `Combat::setOnKill` → fired from `killEntity` with `killerSlot` attribution.
- **Energy storage:** `SkillState.energy` / `.maxEnergy` in `m_skillStates[slot]`.
- **Display:** `affixTypeName` ([hud_inventory.cpp:16](../../../src/renderer/hud_inventory.cpp#L16));
  character sheet "Lifesteal" row ([engine_render_character.cpp:279](../../../src/engine/engine_render_character.cpp#L279)).

## Design

### 1. Two new affixes
Append to `AffixType` **before `COUNT`** (save-safe): `MANASTEAL_PCT`, `MANA_ON_KILL`.
Add to `affixTypeFromString`: `"manasteal_pct"`, `"mana_on_kill"`.
Add to `affixes.json`:
- `manasteal_pct` — name *"of the Mystic"*, slots `[weapon, ring, gloves, helmet]`, min **0.1**, max **1.0**
- `mana_on_kill` — name *"of Harvesting"*, slots `[ring, helmet, gloves, weapon]`, min **1.0**, max **5.0**

On-demand sums (declare in `item.h`, define in `inventory.cpp` next to `lifestealPct`, reusing
`sumEquippedAffix`):
- `f32 Inventory::manastealPct(const PlayerInventory&)`
- `f32 Inventory::manaOnKill(const PlayerInventory&)`

### 2. Mana steal application (mirror lifesteal)
A small helper restores energy to a slot's `SkillState`, clamped to `maxEnergy`:
`ss.energy = fminf(ss.energy + gain, ss.maxEnergy)`. At **each** lifesteal site, right after the HP
heal, compute `gain = damageDealt * Inventory::manastealPct(inv) * 0.01f` and apply it to the **same
player's** energy:
- `engine_combat.cpp:658` (local melee/hitscan) → `m_skillStates[m_localPlayerIndex]`.
- `engine_combat.cpp:1357` (remote NetPlayer) → that remote slot's skill state, mirroring how the
  HP heal there picks the player.
- `engine_init_callbacks.cpp:197` (projectile hit) → restore to `m_skillStates[ownerSlot]`, using the
  same local-lane (`ownerSlot < m_splitPlayerCount`) vs. remote (`m_players[ownerSlot]`) routing the
  HP heal already uses.

`damageDealt` is the same value lifesteal already uses (`wpn.damage` at the melee/hitscan sites; the
callback's `damage` arg for projectiles). Behavior therefore matches lifesteal exactly, including on
skill-spawned projectiles; **no gating is added**.

### 3. Mana on kill (one new hook)
In the engine's `onKill` handler (registered via `Combat::setOnKill`, fired from `killEntity` with
`killerSlot`), add: `energy[killerSlot] += Inventory::manaOnKill(m_inventories[killerSlot])`, clamped
to `maxEnergy`. Routes to the local-lane Player's skill state vs. the remote slot's skill state the
same way the kill is already credited. Fires on every kill regardless of damage source.

### 4. Display
- `affixTypeName`: `MANASTEAL_PCT` → `"Mana Steal %"`, `MANA_ON_KILL` → `"+Mana on Kill"`.
- Character sheet: add a **"Mana Steal"** row (`Inventory::manastealPct`) beside the existing
  "Lifesteal" row.

### 5. Save format
`AffixType` values are appended before `COUNT` (a save stores affix type by integer, so old saves are
unaffected). Both affixes are summed on demand — **no new `PlayerInventory` field, no `SAVE_VERSION`
bump, no legacy-reader changes.**

## Testing

- **Unit** (`tests/game/test_mana_affixes.cpp`, forward-only policy): `manastealPct` and `manaOnKill`
  sum correctly across a synthetic equipped set; empty inventory → 0; multiple slots stack; non-mana
  affixes are ignored. Pure functions over `PlayerInventory`, same shape as any lifesteal test.
- **Manual:** equip a manasteal weapon → melee/hitscan **and** projectile hits raise energy; casting
  a skill does **not** refund energy from the spell's own damage; equip mana_on_kill → kills (by
  weapon **and** by spell) raise energy; energy clamps at max; tooltips show the new labels; the
  character sheet shows "Mana Steal".
- Build `DungeonEngine` + `dungeon_tests` green; deploy to Switch.

## Out of scope (explicit)

- No change to lifesteal's behavior, including its existing proc on skill-spawned projectiles.
- No weapon-vs-skill projectile marking/gating.

## Resolved during implementation

- **Energy storage:** per-lane `m_skillStates[slot].energy/.maxEnergy` (parallels `m_inventories[slot]`).
  Restores use that index at every site, clamped to `maxEnergy`.
- **Energy is NOT in the network snapshot** (it's client-authoritative; only HP is synced). So the
  remote **server** path (`handleWeaponFireForPlayer`) was deliberately *not* given an energy restore
  — it would be dead code. Mana sustain is applied on the **authoritative local lanes** only.
- **Net effect by play mode:**
  - Singleplayer / host / split-screen lanes — **fully works** (steal + on-kill).
  - A remote **guest**: melee/hitscan manasteal works via client prediction (`result.hitEntity`);
    **projectile manasteal + mana-on-kill now reach the guest** via the `SV_ENERGY_GAIN` reliable
    event (see the follow-up design `2026-06-16-...`/plan): the host routes those host-resolved gains
    through `Engine::grantEnergy`, which coalesces a remote guest's gain into one reliable packet per
    tick that the guest adds to its local pool. The only remaining gap is a *mispredicted* melee swing
    (client "hit", server "miss") leaving a tiny un-reconciled over-credit — acceptable under the
    co-op trust model. Full server-authoritative energy (snapshot field + predict/reconcile + cost
    enforcement) is the larger deferred milestone.
- **On-kill hook:** the `Combat::setOnKill` lambda (fired from `killEntity`) reaches the killer's
  inventory + skill state via `killerSlot`; gated `killerSlot < m_splitPlayerCount` (local lanes;
  excludes `0xFF` environmental and remote guests), placed before the SERVER-only broadcast guard so
  singleplayer benefits.
