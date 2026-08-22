---
name: create-trailer
description: Produce or update a DungeonEngine trailer — shoot deterministic takes with the --record/--camera/--stage pipeline, generate the gold/red title cards, cut via the EDL scripts, QC frames, and export the Steam master. Trigger for "neuer Trailer", "Trailer ändern", "Take drehen", "trailer schneiden", "capture footage".
---

# Create / update a trailer

The whole pipeline is deterministic: **the command is the take, the script is the edit.**
Masters, takes, stages, proofs live in `/home/aaron/game_takes/` (PERMANENT — never only /tmp).

## Hard rules (standing, from Aaron)

- **Never drive input into the running game** (no xdotool anything); **launching opens a window
  on :1 and grabs pointer/focus — only with Aaron's go-ahead.** Kill by explicit PID.
- Artifacts worth keeping go to `/home/aaron/game_takes/` (subdirs `stages/`, `proofs/`,
  `previews/`; `takes.md` is the index — keep it current).
- **Don't advertise features on cards that can be SHOWN in action** (his call on the chakrams:
  no hype card — a thrown disc, a wall bounce, a kill).
- Run from the **repo root** or shaders fail and frames are a flat clear colour.

## 1. Shoot a take

```
./build/src/DungeonEngine <world> <actors> <camera> --record /home/aaron/game_takes/<take>
```

- `--record <dir>` LOCKSTEPS the sim (1 tick = 1 frame; wall speed irrelevant — PNG writing runs
  ~6-8 fps wall, the encode is still exact real time). `manifest.txt` on clean exit.
- **Wall ≠ sim time:** ~110 s wall ≈ 12-15 s footage. Budget sleep accordingly; kill by PID.
- Worlds: `--floor N` (+`--vhall`/`--fourstory`/`--lava`), `--zone N`, `--town`, `--source`
  (spawns INSIDE the superboss fight — the boss shot), `--chakram-room [n]` (the storm stage;
  n=1 pins the disc to torso height for the follow-kill).
- Actors: `--autoplay` (bot plays; also DEFENDS the cameraman on hostile floors), `--endgame`
  (ladder-end gear), `--stage <file>` (spawn/loot/equip lines — see `engine-reference`;
  `docile` zeroes detection AND legs, or extras wander off their marks).
- Camera: `--camera "orbit:cx,cz,r,lapSec[,h[,lookY]]"` · `"glide:ax,ay,az:bx,by,bz:secs[:look]"`
  (put the gaze point at the FAR end or the mid-glide view tips vertical) ·
  `"follow[:dist,height]"` (trails the first live player projectile, holds after the kill;
  1 Hz `[FOLLOW]` telemetry charts the deterministic path — MEASURE the path with a real-time
  probe run first, then place extras ON it; two guessed takes missed entirely).
  `--camera` implies HUD-off; `--hidehud` for bot-played HUD-free takes without a camera path.

## 2. Encode

`tools/encode_trailer.sh <take-dir> [out.mp4] [--scale 1920x1080]` — encodes at the manifest fps
(CRF 18 master). Every take dir gets its `take.mp4` before cutting.

## 3. Title cards & music

- Cards are the OLD title art (gold/red wordmark over the stairs scene) — **never drawtext**:
  `store/trailer/frame_1..5` (word-by-word intro), `logo_walk.mp4` (animated outro),
  `tools/gen_trailer_cards.py` regenerates mid-cards (reuses `gen_trailer_frames.py`'s
  `render_scene_master`/`draw_word`). New card = add to CARDS list there, rerun.
- Music: `tools/gen_placeholder_music.py` synthesizes the licence-free bed
  (`game_takes/PLACEHOLDER_music_bed.wav`). Aaron LIKED it — but track choice is HIS
  (pick_sfx-style flow, WB-274); never auto-pick.

## 4. Cut

`tools/cut_cinematic.sh` / `tools/cut_gameplay.sh` — the EDL IS the script (clip/pngcard/mp4seg
lines via `tools/cut_common.sh`); every change is a re-run, output is the 1080p60 Steam master
with the music bed mixed in. Cinematic = HUD-free takes only; gameplay = HUD-on, honest,
DUNGEON-only (Aaron's call — no overworld clips there).

## 5. QC — sight it before calling it done

- Extract frames at every cut boundary (`ffmpeg -ss T -frames:v 1`), READ them. Montage sheets
  mislead (glob order) — judge single frames.
- Known traps, all shipped once: **floor-transition slates** in trim windows (endgame bots clear
  floor 1 in ~4 s — check couch/biome takes); a fresh hero DIES in seconds on deep floors
  (death screen mid-take — `--autoplay` or `--endgame` to survive, or use it as the death beat:
  find its frames by SIZE, the near-black screen is ~30 KB vs ~500 KB); segment counters in
  subshells (`$(seg_out)` didn't increment — the first render was the end card ten times).
- Verify audio (`volumedetect`), duration, and that the takes named in the EDL have a take.mp4.

## 6. Deliver

Phone previews: 640x360, `-profile:v baseline`, CRF 28, faststart (~5 MB) → SendUserFile one at
a time. Big files (>15 MB) time out. Update `takes.md`, report the Werkbank ticket (epics
WB-266/267), commit tools changes. Final cut approval is ALWAYS Aaron's.
