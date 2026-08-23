---
name: create-cinematic-trailer
description: Produce or update the CINEMATIC trailer — HUD-free camera-path takes (orbit/glide/follow), the biome tour on deep floors, the seven-biome weapon montage, loot shower, chakram follow-kill and storm orbit, word-by-word title intro and logo-walk outro; tools/cut_cinematic.sh is the EDL. Trigger for "Cinematic-Trailer", "cinematic trailer neu", "Kamerafahrt", "Biome-Tour".
---

# Create / update the CINEMATIC trailer

**Load the `create-trailer` skill first** — it owns the shared pipeline (shoot → encode →
cards → cut → QC → deliver) and the standing hard rules. This skill is the cinematic-specific
layer on top.

## What makes a take a CINEMATIC take

- **HUD OFF**, two routes: `--camera "<orbit|glide|follow spec>"` (replaces the player camera,
  implies HUD-off) for staged shots, or `--hidehud` for bot-played runs where the player camera
  IS the dolly (the biome tour, the weapon montage).
- The cinematic carries the **biome tour and the overworld/town material** — the gameplay
  trailer is dungeon-only, this one is where the world gets shown.
- **One visual language**: the word-by-word CURSE/OF/THE/DUNGEON-ENGINE intro frames
  (`store/trailer/frame_1..4`), gold-wordmark mid-cards, the animated `logo_walk.mp4` outro,
  the OUT NOW end card. Never drawtext-on-black ("die waren cooler").
- **Don't advertise on cards what can be SHOWN in action** — the chakrams get a thrown disc, a
  wall bounce, a kill; never a hype card.

## The beat structure (v6 EDL, `tools/cut_cinematic.sh`)

Word intro → town glide → **biome tour** (dungeon → catacombs → caverns → descent → lava
flyover → void) → **weapon montage** (7 legendaries, ~2.2 s each, EACH IN ITS OWN BIOME — the standing rule since v7) → loot shower → stage beat →
**chakram follow-kill** → storm orbit → logo walk → OUT NOW. ~77 s.

## Per-beat recipes

- **Biome tour**: bot-played `--hidehud --endgame --autoplay` runs, one floor per THEME — and
  on the DEEP end of each tier (7 / 18 / 25 / 9-fourstory / 31-lava / 47), because an endgame
  hero on floor 3 races empty corridors and an empty corridor is not a spectacle. Trims from
  `scan_take.py` clean windows, then eyeball: composition varies wildly on bot camera.
- **Lava flyover**: `--floor 31 --lava --endgame --autoplay --camera "glide:4,6,4:40,5,36:12:52,1,48"`
  — the gaze point sits at the glide's FAR END or the mid-flight view tips vertical; the bot
  defends the cameraman.
- **Town reveal**: `--new warrior --town --camera "glide:22,4.5,40:22,2.2,6:11:22,1.5,2"`,
  trim the early high sweep (~2.0–7.0) before the camera sinks into empty grass.
- **Weapon montage**: every weapon fights in its OWN biome (v7, Aaron: "abwechslungsreicher —
  mehr Biome"): stone 3, catacombs 18, caverns 25, `--vhall` 8, `--lava` 33, void 47,
  `--fourstory` 9. `--new warrior --floor <n> [style] --autoplay --hidehud --stage
  stages/eq3_<w>.txt` — the eq3 stages carry the weapon PLUS the legendary armor kit (the orb
  recipe's survivability trick; a fresh unarmored hero dies in seconds past floor ~12; one
  death per deep take is fine — trim inside the clean windows). Fresh hero so nothing in the
  bag displaces the staged gear. Seeds differ per launch — a take whose best frame is the
  inside of a loot crate gets RE-ROLLED, not re-trimmed. Note: in the sealed chakram room the
  bot does NOT fight (stage worlds give the brain no floor), so weapon demos must run on real
  floors.
- **Loot shower**: `--new warrior --town --stage stages/stage_loot.txt --camera
  "orbit:22,20,6,12,2.6,0.6"` — 56 drops (near the 64-slot world-item cap), every 6th roll
  forced legendary.
- **Chakram follow-kill**: `--new rogue --endgame --chakram-room 1 --stage
  stages/stage_followkill.txt --camera "follow:2.4,0.5"`. `--chakram-room 1` pins the lone
  disc to torso height; the extras stand ON the measured path (probe run + `[FOLLOW]`
  telemetry charted it; the room is deterministic, so the kill reproduces — verify it did:
  the disc despawns and telemetry stops at the hit, ~2.3 s).
- **Storm orbit**: `--new warrior --endgame --chakram-room 55 --camera
  "orbit:14,14,11.5,13,3.2,1.3"` — single frames undersell it, the motion carries; judge in
  video, not stills.

## Trims and QC

Camera-path takes are deterministic from frame 0, so their trims survive re-shoots; bot-played
takes need fresh `scan_take.py` windows every time. Sheet the finished master once
(`fps=0.8,tile=8x8`) before delivering; trailing black tiles are sheet padding, not a defect.
