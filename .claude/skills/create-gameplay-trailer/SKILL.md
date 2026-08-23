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

## The beat structure (v8 EDL, `tools/cut_gameplay.sh`)

**NO text cards mid-stream** (v7) and **the cut walks the EQUIPMENT spectrum** (v8, Aaron:
"fast nur melee — ich brauche die volle Equipment variety" / "mehr skills"): staff → bow →
revolver → launcher → meteor wand → drones → turrets → paladin hammer → flask → hitscan →
sword duel → discs, and every class beat is a SKILL showcase with its cast window pinned by
log_time.py. Orb opener → ranger-over-lava (Volley/Barrage) → gw weapon beats → meteor storm
→ tinkerer/engineer/paladin → flask → descent → six-boss reel → chakram storm → couch →
death → OUT NOW. ~50 s. A new beat that reads as one more melee swing is the wrong beat.

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
- **Boss beats**: milestone floors (`--floor N --difficulty 2 --endgame --autoplay`,
  class marksman survives where wanderer died on 45). The bot + endgame gear melts a boss in
  seconds: find the kill via the NEXT floor's `"Floor N+1 exit portal"` build line in the log,
  then contact-sheet the seconds before it — pick the moment nameplate + body + numbers
  coincide. Never the Dungeon Engine (Aaron: show OTHER bosses, e.g. the teleporter Nyx).
  TWO measured boundaries: an EARLY boss (the Butcher, floor 5) dies OFFSCREEN in ~4 s to
  endgame gear — shoot him as a DUEL instead (`--new warrior --floor 5 --autoplay --stage
  stages/eq_armor.txt`: armor-only kit + starter sword keeps the DPS low, so he stays alive
  and on camera ~10 s). And Inferno floor 50 (Grim Reaper) is UNREACHABLE — the marksman died
  three times and the boss never entered the frame; Azhar (35) is the deep-end substitute.
- **Class-skill showcases** (Aaron: turret squad / drone army / Divine Judgment / frozen
  orbs): `log_time.py` finds every cast ("Turret requested", "Divine Judgment: ... pillars",
  "Swarm ..."). Paladin needs floor 30+ so Divine Judgment (unlock {1,10,20,30}) is the
  biggest slot the dump picks — measured 3 casts in 3.5 s at floor 31. The tinkerer's swarm
  tag reaches x28 by ~19 s on floor 15; floor 15 is a BOSS floor, so the drone army fights
  Sethrak for free. **Grep the CLASS's REAL kit** (class_defs.cpp) before hunting casts: the
  ranger's kit is Volley/Piercing Shot/Barrage/Mark Prey — a log_time on "Multi Shot|Rain of
  Arrows" (legacy skill names) returns silence and reads as "the ranger never casts".
- **Ranger over the lava sea**: `--new ranger --floor 33 --lava --autoplay --stage
  stages/eq3_bow.txt` — staged Void Bow + armor kit, NO --endgame: the endgame roll handed
  the ranger a WAND (the ranged column holds guns and bows, but the roll can score a caster
  weapon higher), and no bow grants a legendary skill, so the class kit gets the whole
  energy pool: Volley (80 arrows), Piercing Shot and Barrage land back to back.
- **Meteor storm**: `--new sorcerer --floor 45 --endgame --autoplay` — the endgame wand's
  granted meteor_strike IS the beat here (the same spam that ruins the orb take makes this
  one); impacts rain from ~3 s ("Meteor/pillar struck" lines, ~2 per second).
- **gw_* weapon beats**: HUD-ON twins of the cinematic's biome-weapon takes (same eq3
  stages, damage numbers visible) — revolver on the two-story hall, launcher over the lava,
  flask in the void.
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
