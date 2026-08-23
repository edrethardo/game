---
name: create-trailer
description: Produce or update a DungeonEngine trailer — shoot deterministic takes with the --record/--camera/--stage pipeline, generate the gold/red title cards, cut via the EDL scripts, QC frames, and export the Steam master. Trigger for "neuer Trailer", "Trailer ändern", "Take drehen", "trailer schneiden", "capture footage".
---

# Create / update a trailer

**Genre layers:** for the actual trailer WORKFLOWS load the dedicated skill on top of this
one — `create-gameplay-trailer` (HUD-on, dungeon-only, the beat recipes) or
`create-cinematic-trailer` (HUD-free camera paths, biome tour, montage staging). This skill
owns what both share: the shoot/encode/cards/cut/QC/deliver pipeline and the hard rules.

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

**Native 1080p:** add `--res 1920x1080` (goes BORDERLESS — a decorated window is WM-clamped
below the desktop size). PNG writes are fast-lossless (level 1, no filter) since v6; even so,
1080p locksteps at ~4-7 fps wall. Frames are ~2.5 MB each: encode each take right after the
shoot and DELETE the PNGs (`take.mp4` at CRF 18 is the keeper; the command re-creates the rest).
Tutorial hints (Attack/Skill, Block, Dodge Roll) are auto-suppressed while `--record` is armed —
every dev-door hero is fresh, and the bot may never perform the action that dismisses a hint.

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

- BOTH trailers end on the OUT NOW card followed by the CREDITS card (user, 2026-08-23:
  direction & editing = Ed Rethardo & Claude; music = "CLAUDE — ORIGINAL SCORE"). The score
  is the SYNTHESIZED bed — seed-fixed and bit-reproducible from tools/gen_placeholder_music.py
  (provenance: game_takes/SCORE.md); there is NO external CC author to credit. Never render
  the two cut scripts in parallel — they share .cut_work and clobber each other's segments.
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

- `tools/scan_take.py <take.mp4>` finds trim windows on the ENCODED take (no PNGs left):
  `clean` = no slates/death screens (adaptive threshold), `--busy D` ranks D-second windows by
  action (busier frames compress worse), `--blue` ranks frozen-orb frost (pale TEAL, g~=b —
  a b>g test finds nothing). `tools/log_time.py <take.log> <regex>` maps any log line to the
  exact sim second — each recorded frame logs its own "screenshot saved" line, so the
  interleave is a frame-accurate clock (wall timestamps are useless at 4-7 fps capture).
- Contact sheets for window hunting: `ffmpeg -ss A -to B -vf "fps=1,scale=480:270,tile=4x3"`.

- Extract frames at every cut boundary (`ffmpeg -ss T -frames:v 1`), READ them. Montage sheets
  mislead (glob order) — judge single frames.
- Known traps, all shipped once: **floor-transition slates** in trim windows (endgame bots clear
  floor 1 in ~4 s — check couch/biome takes); a fresh hero DIES in seconds on deep floors
  (death screen mid-take — `--autoplay` or `--endgame` to survive, or use it as the death beat:
  find its frames by SIZE, the near-black screen is ~30 KB vs ~500 KB); segment counters in
  subshells (`$(seg_out)` didn't increment — the first render was the end card ten times).
- Verify audio (`volumedetect`), duration, and that the takes named in the EDL have a take.mp4.
- **Boss beats:** the bot + endgame gear melts bosses in seconds — find the fight via the NEXT
  floor's build line in the log, then sheet the seconds before it. A named boss barely frames
  itself; pick the moment the nameplate + body + numbers coincide.
- **Frozen-orb beat recipe:** fresh sorcerer + staged FULL legendary kit incl. `equip Frost
  Staff` (grants frozen_orb; the granted-rail spam becomes orb spam). `--endgame` is WRONG for
  this shot: the bag holds a better wand and auto-equip swaps the staff back out mid-take
  (measured twice), and optimal gear melts rooms so fast no cast ever shows.
- **Never rewrite a shoot script while its bash instances run** — bash reads script files
  lazily, and a rewrite underneath a running instance re-seeks mid-token ("syntax error near
  `;;`", two takes killed mid-batch). Copy the loop into a new driver file instead.
- **`--autoplay-couch` needs `--new <class>` beside it** or the parser falls back to the menu
  and the "recording" is zero frames with the warning buried in a block-buffered log.

## 6. Deliver

Phone previews: 640x360, `-profile:v baseline`, CRF 28, faststart (~5 MB) → SendUserFile one at
a time. Big files (>15 MB) time out. Update `takes.md`, report the Werkbank ticket (epics
WB-266/267), commit tools changes. Final cut approval is ALWAYS Aaron's.
