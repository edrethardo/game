---
name: create-gameplay-trailer
description: Produce or update the GAMEPLAY trailer — HUD-on, dungeon-only, honest footage; bot-played takes per beat (frozen-orb opener, class beats, boss trio, chakram storm, couch split, death beat), scan-derived trims, tools/cut_gameplay.sh as the EDL. Trigger for "Gameplay-Trailer", "gameplay trailer neu", "Gameplay-Take".
---

# Create / update the GAMEPLAY trailer

**Load the `create-trailer` skill first** — it owns the shared pipeline (shoot → encode →
cards → cut → QC → deliver) and the standing hard rules (never drive input into the game,
artifacts to `/home/aaron/game_takes/`, run from repo root, kill by PID). This skill is the
gameplay-specific layer on top.

## What makes a take a GAMEPLAY take

- **HUD ON** (no `--hidehud`, no `--camera` — the player camera IS the shot). Damage numbers,
  boss bars, the skill bar and the minimap are the point: this trailer answers "what does
  playing it actually look like".
- **DUNGEON-ONLY** (Aaron's standing call): floors, forced styles (`--vhall`, `--fourstory`,
  `--lava`), `--source`, `--chakram-room`, couch. No overworld/zone clips here — those belong
  to the cinematic.
- **Honest**: bot-played runs (`--autoplay`), real drops, real deaths. The death beat is a
  feature, not an accident.
- Tutorial hints (Attack/Skill, Block, Dodge Roll) auto-suppress while `--record` is armed —
  every dev-door hero is fresh, and the bot may never do the action that dismisses a hint.

## The beat structure (v6 EDL, `tools/cut_gameplay.sh`)

card ACTUAL GAMEPLAY → **orb opener** → lava → VHALL brawl → card 9 CLASSES → tinkerer swarm →
engineer tesla → descent crits → card 11 BOSSES → three bosses → chakram storm → card COUCH
CO-OP → splitscreen → death beat → card OUT NOW. ~48 s. Every beat is dense combat; a quiet
beat (the v5 menu pan) is the first thing to cut.

## Per-beat recipes (each earned by a measured failure)

- **Frozen-orb opener**: FRESH sorcerer + staged FULL legendary kit incl. `equip Frost Staff`
  (grants frozen_orb → the bot's granted-skill spam IS orb spam), on `--floor 12 --vhall` for
  density. `--endgame` is WRONG twice over: the bag holds a better wand so auto-equip swaps
  the staff back out mid-take (measured), and optimal gear melts rooms before a cast shows.
  Stage: `game_takes/stages/eq_orb2.txt`. Find the trim with `scan_take.py --blue` (frost is
  pale TEAL, g≈b — a b>g test finds nothing).
- **Class beats** (tinkerer/engineer): `--new <class> --floor 15 --endgame --autoplay`. The
  build-cell re-pick in engine_launch.cpp keeps casters off swords — if a class carries the
  wrong weapon family, that fix regressed. Find tesla casts etc. with
  `log_time.py <log> "Tesla Coil hit"` — the frame-line interleave is a framegenaue sim clock.
- **Boss beats**: milestone floors (`--floor 45/40/10 --difficulty 2 --endgame --autoplay`,
  class marksman survives where wanderer died on 45). The bot + endgame gear melts a boss in
  seconds: find the kill via the NEXT floor's `"Floor N+1 exit portal"` build line in the log,
  then contact-sheet the seconds before it — pick the moment nameplate + body + numbers
  coincide. Never the Dungeon Engine (Aaron: show OTHER bosses, e.g. the teleporter Nyx).
- **Chakram storm**: `--new rogue --endgame --chakram-room 55 --stage stages/stage_storm3.txt`
  — docile extras CLOSE to the player spawn at (14,22) so the discs shred them on camera.
  AGGRO tier-5 packs one-shot even the endgame rogue (a whole retake was one long YOU DIED);
  docile-but-distant extras read as a static room.
- **Couch beat**: `--new warrior --autoplay-couch --endgame` — `--new` is REQUIRED beside
  `--autoplay-couch` or the parser falls back to the menu and the "recording" is zero frames
  (the warning hides in a block-buffered log). Trim past the early floor-transition slates.
- **Death beat**: `--new warrior --floor 40 --autoplay` — the fresh hero dies in seconds,
  revives, dies again. Death screens sit in the scan gaps (they compress tiny); trim the last
  alive second into the screen's onset.

## Trims and QC

`tools/scan_take.py <take.mp4>` for clean/busy windows (adaptive threshold),
`tools/log_time.py` to pin log events to sim seconds, contact sheets
(`ffmpeg -ss A -to B -vf "fps=1,scale=480:270,tile=4x3"`) for window hunting, then READ the
frames — a boss fight's chosen 3 s must show the boss, not the trash beside it. Render via
`tools/cut_gameplay.sh` (trims are literals in the EDL), sheet the FINISHED master once
(`fps=1,tile=7x7`) before delivering.
