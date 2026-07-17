# Arena Mode (PvP) — Design

**Date:** 2026-07-17
**Status:** Approved (menu entry, PvP wiring, map, deathmatch loop all locked with the user)

## Summary

A player-vs-player deathmatch mode, entered from a new top-level main-menu row **"Arena Mode"**.
Players bring their **saved characters** (real level, gear, skills) into a dedicated, deterministic
**arena map with cover** on sentinel floor **97** and fight free-for-all to **10 kills**. Supported
setups: **online** (host + join, up to `MAX_PLAYERS` = 4, including host-couch/client-couch lanes)
and **local split-screen 1v1** ("Local Versus"). The **full kit** deals PvP damage — weapons *and*
class/legendary skills — resolved server-authoritatively through the existing
`Combat::applyDamageToPlayer` path so blocking, perfect blocks, armor and invuln frames work
unchanged. Arena is a **progression firewall**: no XP, no loot, no saves touched.

### Locked decisions (from the user)

1. Connectivity: **online + couch** (both).
2. Gear: **saved characters** — unbalanced by design ("let them exploit it" philosophy).
3. Format: **FFA deathmatch, first to 10 kills**, 3 s auto-respawn.
4. PvP damage surface: **full kit** (weapons + skills), not weapons-only.
5. The arena must **provide cover** (explicit user requirement).

## Mode representation

- `Game::ARENA_SENTINEL_FLOOR = 97u` in `game/game_constants.h`, beside TOWN (98) / SOURCE (99).
- The floor sentinel **is** the mode flag: `Engine::inArena()` ⇒ `m_floor == ARENA_SENTINEL_FLOOR`.
  No separate boolean to drift out of sync, and the join-accept packet already carries the floor —
  the existing sentinel-floor routing (added for join-into-town) grows one case:
  97 → `enterArenaClient()`.
- `PROTOCOL_VERSION` **19 → 20** (new SV_EVENTs + arena semantics; old clients get a clean
  `SV_JOIN_REJECT`).

## Menu & flow

New row **"Arena Mode"** in `fullLabels[]` (`engine_render_menus.cpp:1053`) between "Join Game" and
"Options". (Demo builds: not added to `demoLabels[]`.)

Submenu (new menu subStates, following the couch-lobby pattern):

- **Host Arena** — existing save-slot chooser (New *or* Continue; a fresh character may enter, that
  is the player's choice) → existing LAN/Online host chooser → existing lobby screen rendered with
  an "ARENA" tag so joiners know what they are entering → start ⇒ `startArena()` (hosts, builds
  floor 97).
- **Local Versus** — clones the couch flow: P1 New/Continue + slot, P2 New/Continue + slot (own
  pads, can't pick P1's slot), then start ⇒ `startArenaCouch()` (`m_splitPlayerCount = 2`, no net).
- **No "Join Arena" entry.** A client uses the normal **Join Game** flow; the join-accept's floor
  routes them into the arena. Steam invites / lobby browser / quickmatch work for free.

Menu `MENU_BACK`/ESC behavior follows the existing chooser screens (every new subState must back
out cleanly — this was a whole bug-fix pass, don't regress it).

## The arena map

`src/engine/engine_arena.cpp` — `buildArenaLevel()` + `spawnArenaContents()` +
`enterArena()` / `enterArenaClient()`, modeled line-for-line on `engine_town.cpp` (199 lines):

- Deterministic build (fixed seed constant), **~36×36 cells**, symmetric across both diagonals so
  no spawn corner is favored.
- **Open sky** — no `CELL_CEILING`, `ceilingHeight` 12, reusing the town's daylight lighting branch
  in `engine_render.cpp` (keyed on the arena sentinel exactly as it keys on town).
- High brick **perimeter wall**; sand/gravel floor texture via `tools/gen_texture.py` (new palette,
  registered in `tools/build_assets.py`; any new mesh goes in **both** `asset_manifest.h` and
  `build_assets.py`).
- **Cover, all as solid cells** (existing collision, combat-query LOS, raycast and projectile DDA
  respect them with zero new systems):
  - a **4-pillar rotunda** at center (the contested middle),
  - **four crate clusters** at mid-field (one per quadrant),
  - **short wall segments** between rotunda and corners (sightline breakers).
- **Four spawn pads** in the corners, each tucked behind cover, spawn yaw facing the plaza.
- **No enemies, no loot spawns, no shrines, no portals.** The exit is the pause menu.

## PvP combat wiring (the core work)

Player attacks currently test **only the entity pool**; enemy projectiles are the only thing that
collides with players. Every player-sourced damage path gains a players-as-targets branch, **gated
on `inArena()`** and always resolved where combat is already authoritative (SP/couch shared tick,
or the server tick — never on a client).

**Target enumeration.** One helper builds the per-tick combatant list: for each living player
except the attacker, a `Player*` view + net slot. On the SERVER that is the remote player views
(`seedRemoteView` mirrors) plus host-local lanes; in couch it is the other lane's
`m_localPlayers[]` **array entry, not the swapped-in alias** (an alias write outside the swap is
erased — the known trap). All PvP resolution for couch runs in `tickSharedSystems`, after the
per-player loop, against the lane arrays.

**Per-path branches** (each lands through `Combat::applyDamageToPlayer(victim, damage,
&attackerPos, attackerSlot…)`, so **blocking, perfect-block windows, armor and invuln frames work
in PvP unchanged**):

1. **Melee** — after the entity cone test in the fire paths (`engine_combat.cpp:424` local,
   `:1375` in `handleWeaponFireForPlayer`), run the same cone test against combatant AABBs.
2. **Hitscan** — ray vs combatant AABBs (closest hit wins between wall, entity, player), beside
   `:521` / `:1409`.
3. **Projectiles** — `fromPlayer` projectiles additionally collide with combatants other than
   `ownerSlot` (today `fromPlayer` skips players entirely). Mirror Aegis' `reflectAsParry`
   projectiles keep their new owner and can hit the original shooter.
4. **AoE skills** (meteor, novas, orb shards, Divine pillar, splash) — one shared helper
   `Engine::applyArenaAoeToPlayers(center, radius, damage, attackerSlot)` called beside each
   existing entity-AoE site in `engine_update_skills.cpp`; the plan enumerates every site.
5. **Chain/direct-target skills** (chain lightning, discharge) — chain hops may target combatants
   when no entity is nearer.

**Damage routing.** Damage to a remote victim reuses **`SV_DAMAGE_TO_ME`** (the M10.3 path built
for enemy projectiles); the victim's client applies it to the predicted local player exactly as it
does for monster hits. Damage numbers, hit sounds, vignette and rumble all fire from the existing
`applyDamageToPlayer` / impact-tier plumbing.

**Deliberate v1 exclusions:**

- **No self-damage** from your own splash/novas.
- **No teams** — pure FFA.
- **Pets/turrets do not attack players** (needs new AI targeting; they may be summoned but are
  cosmetic in arena). Parked.
- **Perfect-block retaliation procs** (Thunderwall discharge, chain retaliation) resolve against
  the entity pool as today — in an empty arena they find no target and fizzle. Blocking itself
  (negation + the perfect window) fully works. Player-targeted retaliation is future work.

## Deathmatch loop

- **Score:** `m_arenaKills[MAX_PLAYERS]` on the authority. A player death whose damage carried an
  attacker slot credits that slot (environmental deaths credit no one).
- **`NetEventType::ARENA_KILL = 0x0A`** — payload: killer slot, victim slot, killer's new score.
  Drives a kill-feed line ("Ed ⚔ Aaron") and the always-on **score strip** (top-center, per-player
  kill counts). Clients keep a score mirror from these events (mid-join gets a full score refresh
  in the join-accept extension).
- **Auto-respawn:** arena deaths skip the death screen. The authority respawns the victim **3 s**
  after death at the spawn pad **farthest from living enemies**, reusing the
  `handleRespawnRequest` field-set (full HP, 1.5 s invuln). The victim's screen shows
  "Respawning in 3…" over the death camera. No `CL_RESPAWN` needed — server-driven, propagates by
  snapshot; couch respawns the lane directly.
- **Match end:** first to **10 kills** (fixed in v1) triggers **`NetEventType::ARENA_OVER = 0x0B`**
  (winner slot + final scores) — broadcast **before** any host-local state flip (the credits-hang
  lesson: run-wide state must broadcast). All peers show a winner banner over the final scoreboard
  for ~8 s, then tear down to the main menu via the existing VICTORY→MENU teardown.

## Progression firewall

In arena, all of the following are inert — the character leaves exactly as they entered:

- **No XP / kill credit** (lifetime-kills stat untouched by PvP kills).
- **No loot:** the death callback's loot phases are already gated; arena adds the gate for player
  deaths (nothing to roll anyway — no entities die).
- **Item drop requests rejected** (`CL_DROP_ITEM` and the local drop path no-op in arena) so gear
  cannot be lost or duped in a mode that never saves.
- **No autosave, no floor transition, no descend.** Pause menu swaps "Save & Quit" / "Go to Town"
  for **"Leave Arena"** (returns to main menu; net teardown if online — same broadcast-first rule
  if the *host* leaves: remaining peers get kicked to menu cleanly, the existing host-quit path).
- Stash, town portal, difficulty unlocks: unreachable/untouched.

## Testing

- **Pure logic in `src/game/arena.h`** (the `stash.h` pattern): score tally + win check +
  farthest-spawn-pad selection as free functions on plain state; doctest coverage in
  `tests/game/test_arena.cpp` (registered in `tests/CMakeLists.txt`).
- **`--arena` launch flag** (with `--load <slot>`) drops straight into a hosted arena for
  iteration; `--arena-couch` starts a local-versus with two fresh lanes.
- **Co-op soak:** host + client under `--net-loss 15 --net-latency 100` — melee, hitscan,
  projectile and AoE kills all credit correctly; respawns don't rubber-band; F9 net-graph stays in
  the known envelope.
- **Regression:** full PvE co-op smoke (nothing PvP-gated may fire outside floor 97), save
  round-trip byte-identical after an arena session.

## Future work (explicitly parked)

- Host-configurable kill target (5/10/20) in the arena lobby.
- Shrines as contested map pickups.
- Pet/turret player-targeting; retaliation procs vs players.
- Round-based duel format; teams; more arena layouts.
