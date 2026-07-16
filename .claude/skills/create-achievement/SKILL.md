---
name: create-achievement
description: Use when adding a Steam achievement to DungeonEngine — the engine trigger (event-driven or polled), the generated 64x64 achieved/locked icon pair, the DEPLOYMENT.md runbook row, the build-steam compile check, and the Steamworks partner-site definition/publish steps. Trigger for "add an achievement", "new Steam achievement", "achievement for doing X".
---

# Create a Steam achievement (end to end)

One achievement = five artifacts: an engine trigger, an icon pair, a runbook row, a compile
check, and a partner-site definition. Miss the last one and everything else silently no-ops.
Worked examples: `ACH_FIRST_ITEM`, `ACH_FULLY_EQUIPPED`, `ACH_BUTCHERED` (2026-07-16).

## Step 1 — Design the trigger around what each machine KNOWS

The unlock must run on the machine of the player who earned it (their Steam account), and
that machine only knows what the sim or the wire tells it:

- **SP / host / split lanes** run the authoritative sim — they know everything (who dealt the
  killing blow, what spawned, what died). `ACH_BUTCHERED` reads the never-cleared
  `Player::lastAttackerEntity` (combat.cpp stamps it wherever health is actually subtracted)
  and matches the boss `nameTag` at the death site.
- **Network guests** know only replicated state + server-confirmed events. A guest does NOT
  know who killed it (damage arrives as HP adoption). Prefer server-confirmed packets the
  client already receives: `ACH_FIRST_ITEM` unlocks for guests in `onPickupResult` on
  accept — gated on `slot >= 0` because shrine activations ride the same packet but never
  enter the pending-pickup ring. If the guest can't know, scope to SP/host and say so in a
  comment (never guess).
- Exclude the cheats and freebies explicitly: starting-loadout grants and the F7 debug give
  do NOT count as "collecting" an item.

## Step 2 — Engine trigger

`Steam::unlockAchievement("ACH_...")` (`platform/steam.h`) is safe to call unconditionally:
no-op on itch builds and without a running Steam client, and it dedups an already-unlocked
achievement before the `StoreStats` round-trip (which it otherwise calls immediately, so the
overlay toast fires at the deed, not at quit).

Two shapes — pick by trigger type:
- **Event-driven** for rare, discrete moments: call it inline at the site (a pickup, a death,
  a boss kill). No guard bool needed.
- **Polled** for STATE conditions ("all 7 slots filled"): add the check to
  `Engine::checkAchievements()` (engine.cpp, called at 1 Hz from run()'s stats block) with a
  `m_ach*` guard bool. Polling is deliberate — equips happen via A-button, double-click,
  drag, quickbar, auto-equip AND save-load; a lazy check catches every path and cannot rot
  when a new path is added.

## Step 3 — Icons (generated, committed)

Add an entry to `ACHIEVEMENTS` in `tools/gen_achievement_icons.py`: a palette + a 16x16 ASCII
grid ('.' = background plate, letters = palette colors — same idiom as gen_status_icons.py;
bad rows/chars/empty art fail loudly). Run it:

```bash
python3 tools/gen_achievement_icons.py
```

It emits `store/steam/achievements/<API_NAME>.png` + `<API_NAME>_locked.png` — 64x64 (Steam's
mandatory size), OPAQUE (the client composites on dark UI), locked = auto-desaturated (Steam
does NOT gray icons for you). `store/` PNGs are committed (marketing assets, unlike meshes).
EYEBALL the art before shipping: render a contact sheet and look at it — pixel art that
"should" read often doesn't on the first pass (the armor bust read as a scarecrow in v1).

## Step 4 — Docs + compile check

- Add a row to the Achievements table in `docs/DEPLOYMENT.md` (API name, display name,
  trigger description).
- Build ALL THREE variants, especially **`cmake --build build-steam`** — the itch build
  compiles `steam.cpp` to no-ops and cannot catch Steamworks API drift. This is how a
  removed-in-SDK-1.61 `RequestCurrentStats()` call almost shipped: only the Steam variant
  failed. (Related: never call `RequestCurrentStats` — this SDK auto-loads user stats.)
- Full test suite green as usual.

## Step 5 — Partner site (only the user can do this)

Steamworks → App 4819550 → Edit Steamworks Settings → Stats & Achievements → Achievements:
1. New achievement with EXACTLY the API name from the code — a mismatch or an unpublished
   definition makes `SetAchievement` silently no-op.
2. Display name + description (player-facing).
3. Upload both icons (Achieved + Unachieved slots).
4. Save, then **Publish** the change set — definitions AND icons stay invisible unpublished.

## Step 6 — Verify

Run a `build-steam` binary with the dev `tools/steam_appid.txt` next to it and a logged-in
Steam client; perform the deed; the overlay toast should fire immediately (StoreStats is
called at unlock). Reset between runs via the partner site's per-account "Clear all stats".
Check the log line `Steam: achievement unlocked: ACH_...`.
