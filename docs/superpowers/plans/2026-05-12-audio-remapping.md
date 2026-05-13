# Audio Remapping Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remap 26 of 48 game sounds to better CC0 source files and replace the global 60% pitch-down with per-sound pitch control in `tools/fetch_audio.py`.

**Architecture:** All changes are in `tools/fetch_audio.py`. A new `PITCH_SHIFTS` dict maps each sound name to a float pitch multiplier. The Phase 3 ffmpeg loop reads from this dict instead of applying a blanket `asetrate=44100*0.4`. No C++ changes needed — pitch is baked into WAV files at build time.

**Tech Stack:** Python 3, ffmpeg (external), WAV processing

**Spec:** `docs/superpowers/specs/2026-05-12-audio-remapping-design.md`

---

### Task 1: Add `PITCH_SHIFTS` dict

**Files:**
- Modify: `tools/fetch_audio.py:210-211` (after `MANUAL_OVERRIDES` closing brace)

- [ ] **Step 1: Add the `PITCH_SHIFTS` dict after `MANUAL_OVERRIDES`**

Insert this block at line 211 (after the closing `}` of `MANUAL_OVERRIDES`):

```python
# Per-sound pitch multiplier for ffmpeg processing.
# 1.0 = no pitch change, <1.0 = pitch down (deeper), >1.0 = pitch up.
# Applied in Phase 3 via: asetrate=44100*pitch, aresample=44100
PITCH_SHIFTS = {
    # Melee weapons — weight hierarchy: dagger(light) > sword > axe=claymore(heavy)
    "sfx_weapon_sword":     0.75,
    "sfx_weapon_dagger":    0.90,
    "sfx_weapon_axe":       0.65,
    "sfx_weapon_claymore":  0.65,
    # Ranged weapons — weight hierarchy: SMG(snappy) > pistol=carbine > revolver(boomy)
    "sfx_weapon_pistol":    0.70,
    "sfx_weapon_smg":       0.80,
    "sfx_weapon_carbine":   0.70,
    "sfx_weapon_revolver":  0.65,
    "sfx_weapon_bow":       0.85,
    "sfx_weapon_crossbow":  0.80,
    "sfx_weapon_throw":     0.90,
    "sfx_weapon_molotov":   0.70,
    "sfx_weapon_wand":      0.90,
    "sfx_weapon_staff":     0.80,
    # Reload
    "sfx_reload":           0.80,
    # Combat hits
    "sfx_hit_melee":        0.70,
    "sfx_hit_hitscan":      0.80,
    "sfx_hit_projectile":   0.75,
    "sfx_enemy_hit":        0.75,
    "sfx_enemy_death":      0.65,
    "sfx_player_hit":       0.75,
    "sfx_player_death":     0.60,
    # Skills — mostly natural pitch, slight darkening for dungeon atmosphere
    "sfx_skill_fire":       0.85,
    "sfx_skill_ice":        0.85,
    "sfx_skill_lightning":  0.90,
    "sfx_skill_blood":      0.80,
    "sfx_skill_dash":       1.0,
    "sfx_skill_heal":       1.0,   # bright chime — no darkening
    "sfx_skill_buff":       0.95,
    "sfx_skill_summon":     0.75,
    "sfx_skill_explosion":  0.70,
    "sfx_skill_stun":       0.75,
    # Items — mostly natural
    "sfx_item_pickup":      0.95,
    "sfx_item_equip":       0.85,
    "sfx_item_drop":        0.90,
    "sfx_potion_use":       0.90,
    # UI — no pitch shift, keep crisp
    "sfx_ui_click":         1.0,
    "sfx_ui_back":          1.0,
    "sfx_ui_confirm":       1.0,
    "sfx_menu_hover":       1.0,
    # Footsteps
    "sfx_footstep_stone":   0.80,
    "sfx_footstep_metal":   0.85,
    "sfx_enemy_footstep":   0.70,
    # Enemies — dark and menacing
    "sfx_enemy_attack":     0.75,
    "sfx_boss_roar":        0.60,
    "sfx_boss_stomp":       0.60,
    # Environment
    "sfx_door_open":        0.80,
    "sfx_level_up":         1.0,   # bright, rewarding — no darkening
}
```

- [ ] **Step 2: Verify the dict has exactly 48 entries**

Run:
```bash
cd /home/aaron/game && python3 -c "
import sys; sys.path.insert(0, 'tools')
exec(open('tools/fetch_audio.py').read().split('# ---')[0]
     + open('tools/fetch_audio.py').read().split('# ---')[1]
     + open('tools/fetch_audio.py').read().split('# ---')[2]
     + open('tools/fetch_audio.py').read().split('# ---')[3])
" 2>&1 | head -5
```

Actually, simpler — just grep and count:
```bash
grep -c '"sfx_' tools/fetch_audio.py | head
```

Verify `PITCH_SHIFTS` has the same 48 keys as `SOUND_MAP`:
```bash
cd /home/aaron/game && python3 -c "
import ast, re
src = open('tools/fetch_audio.py').read()
# Extract dict keys by regex
sound_map_keys = set(re.findall(r'\"(sfx_[a-z_]+)\"', src.split('SOUND_MAP')[1].split('MANUAL_OVERRIDES')[0]))
pitch_keys = set(re.findall(r'\"(sfx_[a-z_]+)\"', src.split('PITCH_SHIFTS')[1].split('}')[0]))
print(f'SOUND_MAP: {len(sound_map_keys)} keys')
print(f'PITCH_SHIFTS: {len(pitch_keys)} keys')
missing = sound_map_keys - pitch_keys
extra = pitch_keys - sound_map_keys
if missing: print(f'Missing from PITCH_SHIFTS: {missing}')
if extra: print(f'Extra in PITCH_SHIFTS: {extra}')
if not missing and not extra: print('All keys match!')
"
```

Expected: `SOUND_MAP: 48 keys`, `PITCH_SHIFTS: 48 keys`, `All keys match!`

- [ ] **Step 3: Commit**

```bash
git add tools/fetch_audio.py
git commit -m "audio: add per-sound PITCH_SHIFTS dict (48 entries)

Replaces the global 60% pitch-down with per-sound control.
UI/heal/level-up stay at 1.0 (crisp), weapons/creatures pitch
down 0.60-0.90 for dungeon weight."
```

---

### Task 2: Update `MANUAL_OVERRIDES` with better source files

**Files:**
- Modify: `tools/fetch_audio.py:150-210` (`MANUAL_OVERRIDES` dict)

- [ ] **Step 1: Replace the `MANUAL_OVERRIDES` dict**

Replace the entire `MANUAL_OVERRIDES` dict (lines 150-210) with:

```python
MANUAL_OVERRIDES = {
    # Melee weapons
    "sfx_weapon_sword":     "oga_80-rpg-sfx/blade_01.ogg",
    "sfx_weapon_dagger":    "kenney_rpg-audio/Audio/knifeSlice.ogg",        # was drawKnife1 (unsheathing, not attacking)
    "sfx_weapon_axe":       "kenney_rpg-audio/Audio/chop.ogg",
    "sfx_weapon_claymore":  "oga_80-rpg-sfx/blade_03.ogg",                  # was blade_02 (too similar to sword's blade_01)
    # Ranged weapons
    "sfx_weapon_pistol":    "oga_100-cc0-sfx/shot_01.ogg",
    "sfx_weapon_smg":       "oga_100-cc0-sfx/shot_02.ogg",
    "sfx_weapon_carbine":   "oga_100-cc0-sfx/hit_03.ogg",                   # was retro synth (too arcade-y)
    "sfx_weapon_revolver":  "oga_100-cc0-sfx/slam_03.ogg",                  # was retro synth (too arcade-y)
    "sfx_weapon_bow":       "oga_swishes/swishes/swish-3.wav",
    "sfx_weapon_crossbow":  "oga_thwack/PCM/thwack-01.wav",
    "sfx_weapon_throw":     "oga_swishes/swishes/swish-9.wav",
    "sfx_weapon_molotov":   "kenney_impact-sounds/Audio/impactGlass_heavy_004.ogg",  # was variant 000
    # Magic weapons
    "sfx_weapon_wand":      "oga_80-rpg-sfx/spell_01.ogg",
    "sfx_weapon_staff":     "oga_80-rpg-sfx/spell_02.ogg",
    # Reload
    "sfx_reload":           "kenney_rpg-audio/Audio/metalLatch.ogg",
    # Combat hits
    "sfx_hit_melee":        "kenney_impact-sounds/Audio/impactPunch_heavy_000.ogg",  # was variant 002
    "sfx_hit_hitscan":      "kenney_impact-sounds/Audio/impactMetal_light_002.ogg",
    "sfx_hit_projectile":   "kenney_impact-sounds/Audio/impactWood_medium_001.ogg",
    "sfx_enemy_hit":        "kenney_impact-sounds/Audio/impactSoft_heavy_001.ogg",
    "sfx_enemy_death":      "oga_80-rpg-sfx/creature_die_01.ogg",
    "sfx_player_hit":       "oga_80-rpg-sfx/creature_hurt_02.ogg",          # was variant 01
    "sfx_player_death":     "oga_80-rpg-sfx/creature_monster_02.ogg",       # was variant 01
    # Skills
    "sfx_skill_fire":       "oga_80-rpg-sfx/spell_fire_03.ogg",
    "sfx_skill_ice":        "kenney_impact-sounds/Audio/impactGlass_light_000.ogg",  # was variant 003
    "sfx_skill_lightning":  "oga_50-retro-synth/synth_laser_05.ogg",        # was variant 03
    "sfx_skill_blood":      "oga_80-rpg-sfx/creature_slime_02.ogg",
    "sfx_skill_dash":       "oga_swishes/swishes/swish-1.wav",              # was swish-5 (swish-1 is quicker)
    "sfx_skill_heal":       "oga_100-cc0-sfx/bell_01.ogg",                  # was spell_fire_04 (fire sound for heal!)
    "sfx_skill_buff":       "oga_50-retro-synth/power_up_04.ogg",           # was variant 03
    "sfx_skill_summon":     "oga_80-rpg-sfx/misc_03.ogg",                   # was variant 02
    "sfx_skill_explosion":  "oga_100-cc0-sfx/explosion.ogg",
    "sfx_skill_stun":       "oga_thwack/PCM/thwack-08.wav",                 # was thwack-05
    # Items
    "sfx_item_pickup":      "oga_80-rpg-sfx/item_coins_02.ogg",             # was variant 01
    "sfx_item_equip":       "oga_80-rpg-sfx/metal_02.ogg",                  # was variant 01
    "sfx_item_drop":        "oga_80-rpg-sfx/item_wood_02.ogg",              # was variant 01
    "sfx_potion_use":       "oga_100-cc0-sfx/plop_02.ogg",                  # was variant 01
    # UI
    "sfx_ui_click":         "kenney_ui-audio/Audio/click3.ogg",
    "sfx_ui_back":          "kenney_ui-audio/Audio/click5.ogg",
    "sfx_ui_confirm":       "kenney_ui-audio/Audio/switch3.ogg",
    "sfx_menu_hover":       "kenney_ui-audio/Audio/rollover1.ogg",
    # Movement
    "sfx_footstep_stone":   "kenney_impact-sounds/Audio/footstep_concrete_000.ogg",  # was variant 001
    "sfx_footstep_metal":   "kenney_rpg-audio/Audio/footstep03.ogg",
    "sfx_enemy_footstep":   "kenney_impact-sounds/Audio/footstep_wood_004.ogg",      # was variant 002
    # Enemies — attack bark is short roar_01, boss gets deep monster_04
    "sfx_enemy_attack":     "oga_80-rpg-sfx/creature_roar_01.ogg",          # was roar_03
    "sfx_boss_roar":        "oga_80-rpg-sfx/creature_monster_04.ogg",       # was monster_03
    "sfx_boss_stomp":       "kenney_impact-sounds/Audio/impactPlate_heavy_000.ogg",  # was variant 002
    # Environment
    "sfx_door_open":        "kenney_rpg-audio/Audio/doorOpen_2.ogg",         # was doorOpen_1
    "sfx_level_up":         "oga_80-rpg-sfx/item_gem_02.ogg",               # was variant 01
}
```

- [ ] **Step 2: Verify all override files exist in cache**

```bash
cd /home/aaron/game && python3 -c "
import os
CACHE = 'build/audio_cache'
overrides = {
    'sfx_weapon_sword':     'oga_80-rpg-sfx/blade_01.ogg',
    'sfx_weapon_dagger':    'kenney_rpg-audio/Audio/knifeSlice.ogg',
    'sfx_weapon_axe':       'kenney_rpg-audio/Audio/chop.ogg',
    'sfx_weapon_claymore':  'oga_80-rpg-sfx/blade_03.ogg',
    'sfx_weapon_pistol':    'oga_100-cc0-sfx/shot_01.ogg',
    'sfx_weapon_smg':       'oga_100-cc0-sfx/shot_02.ogg',
    'sfx_weapon_carbine':   'oga_100-cc0-sfx/hit_03.ogg',
    'sfx_weapon_revolver':  'oga_100-cc0-sfx/slam_03.ogg',
    'sfx_weapon_bow':       'oga_swishes/swishes/swish-3.wav',
    'sfx_weapon_crossbow':  'oga_thwack/PCM/thwack-01.wav',
    'sfx_weapon_throw':     'oga_swishes/swishes/swish-9.wav',
    'sfx_weapon_molotov':   'kenney_impact-sounds/Audio/impactGlass_heavy_004.ogg',
    'sfx_weapon_wand':      'oga_80-rpg-sfx/spell_01.ogg',
    'sfx_weapon_staff':     'oga_80-rpg-sfx/spell_02.ogg',
    'sfx_reload':           'kenney_rpg-audio/Audio/metalLatch.ogg',
    'sfx_hit_melee':        'kenney_impact-sounds/Audio/impactPunch_heavy_000.ogg',
    'sfx_hit_hitscan':      'kenney_impact-sounds/Audio/impactMetal_light_002.ogg',
    'sfx_hit_projectile':   'kenney_impact-sounds/Audio/impactWood_medium_001.ogg',
    'sfx_enemy_hit':        'kenney_impact-sounds/Audio/impactSoft_heavy_001.ogg',
    'sfx_enemy_death':      'oga_80-rpg-sfx/creature_die_01.ogg',
    'sfx_player_hit':       'oga_80-rpg-sfx/creature_hurt_02.ogg',
    'sfx_player_death':     'oga_80-rpg-sfx/creature_monster_02.ogg',
    'sfx_skill_fire':       'oga_80-rpg-sfx/spell_fire_03.ogg',
    'sfx_skill_ice':        'kenney_impact-sounds/Audio/impactGlass_light_000.ogg',
    'sfx_skill_lightning':  'oga_50-retro-synth/synth_laser_05.ogg',
    'sfx_skill_blood':      'oga_80-rpg-sfx/creature_slime_02.ogg',
    'sfx_skill_dash':       'oga_swishes/swishes/swish-1.wav',
    'sfx_skill_heal':       'oga_100-cc0-sfx/bell_01.ogg',
    'sfx_skill_buff':       'oga_50-retro-synth/power_up_04.ogg',
    'sfx_skill_summon':     'oga_80-rpg-sfx/misc_03.ogg',
    'sfx_skill_explosion':  'oga_100-cc0-sfx/explosion.ogg',
    'sfx_skill_stun':       'oga_thwack/PCM/thwack-08.wav',
    'sfx_item_pickup':      'oga_80-rpg-sfx/item_coins_02.ogg',
    'sfx_item_equip':       'oga_80-rpg-sfx/metal_02.ogg',
    'sfx_item_drop':        'oga_80-rpg-sfx/item_wood_02.ogg',
    'sfx_potion_use':       'oga_100-cc0-sfx/plop_02.ogg',
    'sfx_ui_click':         'kenney_ui-audio/Audio/click3.ogg',
    'sfx_ui_back':          'kenney_ui-audio/Audio/click5.ogg',
    'sfx_ui_confirm':       'kenney_ui-audio/Audio/switch3.ogg',
    'sfx_menu_hover':       'kenney_ui-audio/Audio/rollover1.ogg',
    'sfx_footstep_stone':   'kenney_impact-sounds/Audio/footstep_concrete_000.ogg',
    'sfx_footstep_metal':   'kenney_rpg-audio/Audio/footstep03.ogg',
    'sfx_enemy_footstep':   'kenney_impact-sounds/Audio/footstep_wood_004.ogg',
    'sfx_enemy_attack':     'oga_80-rpg-sfx/creature_roar_01.ogg',
    'sfx_boss_roar':        'oga_80-rpg-sfx/creature_monster_04.ogg',
    'sfx_boss_stomp':       'kenney_impact-sounds/Audio/impactPlate_heavy_000.ogg',
    'sfx_door_open':        'kenney_rpg-audio/Audio/doorOpen_2.ogg',
    'sfx_level_up':         'oga_80-rpg-sfx/item_gem_02.ogg',
}
ok = 0
for name, path in sorted(overrides.items()):
    full = os.path.join(CACHE, path)
    if os.path.isfile(full):
        ok += 1
    else:
        print(f'MISSING: {name} -> {path}')
print(f'{ok}/{len(overrides)} files found')
"
```

Expected: `48/48 files found`

- [ ] **Step 3: Commit**

```bash
git add tools/fetch_audio.py
git commit -m "audio: remap 26 sounds to better source files

Key changes:
- sfx_skill_heal: fire sound -> bell chime
- sfx_weapon_dagger: unsheathe -> knife slice
- sfx_weapon_carbine/revolver: retro synth -> grounded shots
- enemy audio hierarchy: short bark vs deep boss roar
- variant swaps across items, footsteps, skills for diversity"
```

---

### Task 3: Replace Phase 3 global pitch loop with per-sound pitch

**Files:**
- Modify: `tools/fetch_audio.py:849-882` (Phase 3 ffmpeg loop)

- [ ] **Step 1: Replace the Phase 3 block**

Replace lines 849-882 (from `# Post-process: pitch down` through `print("\n[NOTE] ffmpeg not found`)`) with:

```python
    # Post-process: apply per-sound pitch shifts using ffmpeg (if available)
    import subprocess
    ffmpeg = shutil.which("ffmpeg")
    if ffmpeg and not args.dry_run:
        print("\n--- Phase 3: Apply per-sound pitch shifts (ffmpeg) ---")
        pitched = 0
        skipped = 0
        for fname in os.listdir(OUTPUT_DIR):
            if not fname.endswith((".wav", ".ogg")):
                continue
            sfx_name = os.path.splitext(fname)[0]
            pitch = PITCH_SHIFTS.get(sfx_name, 0.6)  # default to old behavior if missing

            src = os.path.join(OUTPUT_DIR, fname)
            if pitch == 1.0:
                # No pitch change needed — just ensure it's WAV format
                if fname.endswith(".ogg"):
                    tmp = src + ".tmp.wav"
                    ret = subprocess.run(
                        [ffmpeg, "-y", "-i", src,
                         "-ar", "44100", "-ac", "1", "-sample_fmt", "s16",
                         tmp],
                        capture_output=True)
                    if ret.returncode == 0 and os.path.isfile(tmp):
                        wav_name = sfx_name + ".wav"
                        dst = os.path.join(OUTPUT_DIR, wav_name)
                        os.replace(tmp, dst)
                        if os.path.isfile(src):
                            os.remove(src)
                        skipped += 1
                    else:
                        if os.path.isfile(tmp):
                            os.remove(tmp)
                else:
                    skipped += 1
                continue

            tmp = src + ".tmp.wav"
            # asetrate: treat source as faster -> plays deeper when resampled back
            ret = subprocess.run(
                [ffmpeg, "-y", "-i", src,
                 "-af", f"asetrate=44100*{pitch},aresample=44100",
                 "-ar", "44100", "-ac", "1", "-sample_fmt", "s16",
                 tmp],
                capture_output=True)
            if ret.returncode == 0 and os.path.isfile(tmp):
                wav_name = sfx_name + ".wav"
                dst = os.path.join(OUTPUT_DIR, wav_name)
                os.replace(tmp, dst)
                # Remove OGG original if it was different
                if fname.endswith(".ogg") and os.path.isfile(src):
                    os.remove(src)
                pitched += 1
            else:
                if os.path.isfile(tmp):
                    os.remove(tmp)
        print(f"  Pitched {pitched} files, {skipped} kept at original pitch")
    elif not ffmpeg:
        print("\n[NOTE] ffmpeg not found — skipping pitch processing")
```

- [ ] **Step 2: Commit**

```bash
git add tools/fetch_audio.py
git commit -m "audio: replace global pitch-down with per-sound pitch shifts

Phase 3 now reads from PITCH_SHIFTS dict. Sounds at 1.0 (UI,
heal, level-up) skip pitch processing entirely. Others get their
individual pitch applied via ffmpeg asetrate."
```

---

### Task 4: Run the pipeline and verify output

- [ ] **Step 1: Run fetch_audio.py**

```bash
cd /home/aaron/game && python3 tools/fetch_audio.py
```

Expected output:
- Phase 1: all 8 packs cached
- Phase 2: all 48 sounds mapped from packs (0 fallback, 0 missing)
- Phase 3: ~38 files pitched, ~10 kept at original pitch

- [ ] **Step 2: Verify all 48 WAV files exist in assets/audio/**

```bash
ls -la /home/aaron/game/assets/audio/sfx_*.wav | wc -l
```

Expected: `48`

- [ ] **Step 3: Verify no OGG files remain (all converted to WAV by ffmpeg)**

```bash
ls /home/aaron/game/assets/audio/*.ogg 2>/dev/null && echo "OGG files found!" || echo "All WAV — good"
```

Expected: `All WAV — good`

- [ ] **Step 4: Spot-check file sizes are reasonable (not empty, not huge)**

```bash
cd /home/aaron/game && for f in assets/audio/sfx_skill_heal.wav assets/audio/sfx_ui_click.wav assets/audio/sfx_boss_roar.wav assets/audio/sfx_weapon_sword.wav; do
  size=$(stat -f%z "$f" 2>/dev/null || stat -c%s "$f")
  echo "$f: ${size} bytes"
done
```

Expected: files between 5 KB and 500 KB each (not 0, not multi-MB)

- [ ] **Step 5: Commit the regenerated audio assets**

```bash
git add assets/audio/
git commit -m "audio: regenerate all 48 sounds with new mappings and per-sound pitch"
```

---

### Task 5: Build and smoke test

- [ ] **Step 1: Build the game**

```bash
cd /home/aaron/game && cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build
```

Expected: builds successfully with no errors

- [ ] **Step 2: Verify the binary runs and loads audio**

```bash
cd /home/aaron/game && timeout 5 ./build/dungeon_game 2>&1 | head -30 || true
```

Expected: should see audio init messages, no "failed to load" errors for any sfx_ file

- [ ] **Step 3: Commit any build-related fixes if needed**

Only if the build or audio loading revealed issues. Otherwise skip.
