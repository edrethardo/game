---
name: pick-sfx
description: Use when hand-picking, replacing, or improving DungeonEngine's weapon/reload sound effects — launch the interactive picker (tools/pick_sfx.py) to audition ranked CC0 candidates, dial in pitch/stretch/gain/reverb/lowpass/trim with live preview, and save choices to the git-tracked manifest. Trigger for "pick/replace/improve a weapon or reload sound", "the gunshot/sword/reload sound is bad", "choose a better SFX", "audio sound picker".
---

# Hand-pick weapon / reload SFX

`tools/pick_sfx.py` is a human-in-the-loop picker for the 15 weapon/reload slots. It ranks
candidates from the already-downloaded CC0 packs, lets the user audition them in a browser and
apply an effect chain, and writes the polished WAV into `assets/audio/` plus a git-tracked
manifest. It complements `fetch_audio.py` (which auto-selects everything else). Nothing in the
pipeline pitch-shifts or edits a hand-picked slot — see **Pipeline guarantees** below.

Scope = 15 slots: `sfx_weapon_{sword,dagger,axe,claymore,pistol,smg,carbine,revolver,bow,`
`crossbow,throw,molotov,wand,staff}` + `sfx_reload`. Output is 44100 Hz / 16-bit / mono WAV, the
exact format SDL_mixer loads (see `engine-reference` for the `SfxId` map). No C++ changes.

## Launch

- **Ensure the CC0 pool exists** (first time): `python3 tools/fetch_audio.py --cache-only`
  (downloads packs into `build/audio_cache/`). If missing, the picker offers to fetch it.
- **Localhost:** `python3 tools/pick_sfx.py` → opens `http://localhost:8765`.
- **On the LAN** (audition from a phone/other device): `python3 tools/pick_sfx.py --host 0.0.0.0`
  → it prints the reachable LAN URL. Bound to all interfaces, so only on a trusted network.
- Runs in the foreground; **Ctrl-C** stops it. If backgrounded, stop it with
  `curl -s -X POST http://<host>:<port>/shutdown`.

## Workflow (in the browser)

1. Pick a slot on the left → ranked candidates load (the currently-shipped sound is pinned on top
   with a gold **CURRENT** tag for A/B).
2. **▶ dry** plays a candidate raw; click a row to select it.
3. Adjust the effect sliders — **pitch** (duration-preserving), **stretch** (constant-pitch time),
   **gain**, **reverb**, **low-pass**, **trim** — preview auto-plays (debounced) as you drag.
4. **Save to assets/audio** writes `sfx_<slot>.wav` (peak-normalized to −3 dBFS, same as preview)
   and records `{source, params}` in the manifest.

## Manifest & persistence

- Choices live in **`tools/sound_selection.json`** (git-tracked; `assets/audio/` is gitignored).
  **Commit that file** for picks to persist and ship.
- `source` is a path relative to `build/audio_cache/`; `params` are the slider values. This makes
  every pick reproducible from the raw CC0 source.
- **`python3 tools/pick_sfx.py --apply`** headlessly re-renders every manifest slot into
  `assets/audio/` (no server). Needs only ffmpeg; numpy/scipy are lazy-loaded for ranking only.

## Pipeline guarantees (why picks are never re-pitched)

- **`fetch_audio.py` is manifest-aware**: it skips the hand-picked slots (no keyword-map, no copy,
  no `PITCH_SHIFTS`/reverb/lowpass) and then self-applies the manifest in a final Phase 4 via
  `pick_sfx.py --apply`. So one `fetch_audio.py` run yields correct sounds; CI needs nothing extra.
- **`gen_audio.py --all`** skips manifest slots too (explicit `--type <slot>` still regenerates one).
- The engine plays WAVs verbatim (volume only, no runtime pitch); packaging copies `assets/` as-is.
- Net: the only processing on a pick is the slider effects + the −3 dBFS peak-normalize you heard
  in preview.

## CLI flags

`--port` (8765) · `--host` (127.0.0.1; `0.0.0.0` for LAN) · `--slot <stem>` (focus one) ·
`--apply` (headless re-render, exit) · `--no-open-browser` · `--cache-dir <path>` ·
`--top <N>` (candidates per slot, 30) · `--list-slots`.

## Notes / gotchas

- The engine loads `sfx_<name>.ogg` **before** `.wav`; the tool deletes any shadowing `.ogg` when
  it writes a slot, so a stale OGG can't hide a pick.
- Ranking = keyword class match (gun/melee/bow/magic/throw/reload) + acoustic features
  (duration/brightness/attack/transients) computed with numpy/scipy; the current pick is pinned top.
- To cover more slots than the 15, extend `SLOTS`/`SLOT_CLASS`/`CLASS_PROFILES` in `tools/pick_sfx.py`.
- Related: the audio asset bullet in **`engine-how-to`**; `SfxId`↔filename map in **`engine-reference`**.
