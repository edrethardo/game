# Audio Remapping: Better Sound Picks + Per-Sound Pitch Control

## Context

The game's 48 sound effects are sourced from 8 CC0 sound packs (~500 files) via `tools/fetch_audio.py`. The current `MANUAL_OVERRIDES` dict maps each game sound to a specific source file, then a global ffmpeg pass pitches everything down by 60%. Several sounds have poor semantic matches (e.g., a fire sound for heal), some ranged weapon sounds are too arcade-y for the Barony-style aesthetic, and the global pitch-down hurts UI crispness and positive-feedback sounds (level up, heal).

This spec remaps 26 of 48 sounds to better-fitting source files and replaces the global pitch-down with per-sound pitch control.

## Changes to `tools/fetch_audio.py`

### 1. Add `PITCH_SHIFTS` dict (after `MANUAL_OVERRIDES`, ~line 211)

Maps each sound name to a float pitch multiplier. `1.0` = no change, `<1.0` = pitch down. The ffmpeg command becomes `asetrate=44100*{pitch}` per file instead of a blanket `asetrate=44100*0.4`.

### 2. Update `MANUAL_OVERRIDES` dict (lines 150-210)

Replace 26 entries with better source files. Keep 22 entries unchanged.

### 3. Replace Phase 3 global pitch loop (lines ~850-880)

Instead of iterating all WAVs with a single pitch factor, look up each file in `PITCH_SHIFTS`. For `1.0` entries, skip ffmpeg entirely (just copy). For others, use the per-sound pitch value.

### No C++ changes needed

Pitch is baked into WAV files at build time. The runtime audio system (`src/audio/audio.h`, `src/audio/audio.cpp`) loads and plays them unchanged.

---

## Complete Remapping Table

### Melee Weapons

| Sound | Source File | Pitch | Change? | Rationale |
|---|---|---|---|---|
| `sfx_weapon_sword` | `oga_80-rpg-sfx/blade_01.ogg` | 0.75 | Keep | Clean sword slash, pitched down for weight |
| `sfx_weapon_dagger` | `kenney_rpg-audio/Audio/knifeSlice.ogg` | 0.90 | **Changed** | Was drawKnife1 (unsheathing sound). knifeSlice is an actual cutting attack. Higher pitch = lighter/faster feel |
| `sfx_weapon_axe` | `kenney_rpg-audio/Audio/chop.ogg` | 0.65 | Keep | Perfect axe chop. Deep pitch for brutal impact |
| `sfx_weapon_claymore` | `oga_80-rpg-sfx/blade_03.ogg` | 0.65 | **Changed** | Was blade_02 (too similar to sword's blade_01). blade_03 is more distinct. Lowest pitch = heaviest melee weapon |

### Ranged Weapons

| Sound | Source File | Pitch | Change? | Rationale |
|---|---|---|---|---|
| `sfx_weapon_pistol` | `oga_100-cc0-sfx/shot_01.ogg` | 0.70 | Keep | Grounded gunshot, pitched down for dungeon weight |
| `sfx_weapon_smg` | `oga_100-cc0-sfx/shot_02.ogg` | 0.80 | Keep | Snappier than pistol (higher pitch = lighter rounds) |
| `sfx_weapon_carbine` | `oga_100-cc0-sfx/hit_03.ogg` | 0.70 | **Changed** | Was retro synth shot_02 (too arcade-y). hit_03 has a sharp crack for rifle report |
| `sfx_weapon_revolver` | `oga_100-cc0-sfx/slam_03.ogg` | 0.65 | **Changed** | Was retro synth shot_01 (too arcade-y). slam_03 = booming single shot. Deepest pitch = most powerful gun |
| `sfx_weapon_bow` | `oga_swishes/swishes/swish-3.wav` | 0.85 | Keep | Good twang+whoosh for bow release |
| `sfx_weapon_crossbow` | `oga_thwack/PCM/thwack-01.wav` | 0.80 | Keep | Mechanical snap/bolt release |
| `sfx_weapon_throw` | `oga_swishes/swishes/swish-9.wav` | 0.90 | Keep | Fast whoosh, relatively natural pitch |
| `sfx_weapon_molotov` | `kenney_impact-sounds/Audio/impactGlass_heavy_004.ogg` | 0.70 | **Changed** | Was impactGlass_heavy_000. Variant 004 for diversity |

### Magic Weapons

| Sound | Source File | Pitch | Change? | Rationale |
|---|---|---|---|---|
| `sfx_weapon_wand` | `oga_80-rpg-sfx/spell_01.ogg` | 0.90 | Keep | Purpose-made magic sound |
| `sfx_weapon_staff` | `oga_80-rpg-sfx/spell_02.ogg` | 0.80 | Keep | Pitched lower than wand (heavier cast) |

### Reload

| Sound | Source File | Pitch | Change? | Rationale |
|---|---|---|---|---|
| `sfx_reload` | `kenney_rpg-audio/Audio/metalLatch.ogg` | 0.80 | Keep | Good mechanical reload |

### Combat Hits

| Sound | Source File | Pitch | Change? | Rationale |
|---|---|---|---|---|
| `sfx_hit_melee` | `kenney_impact-sounds/Audio/impactPunch_heavy_000.ogg` | 0.70 | **Changed** | Was punch_002. Variant 000 for meatier punch |
| `sfx_hit_hitscan` | `kenney_impact-sounds/Audio/impactMetal_light_002.ogg` | 0.80 | Keep | Metal ping for bullet impact |
| `sfx_hit_projectile` | `kenney_impact-sounds/Audio/impactWood_medium_001.ogg` | 0.75 | Keep | Wood thud for arrow/bolt |
| `sfx_enemy_hit` | `kenney_impact-sounds/Audio/impactSoft_heavy_001.ogg` | 0.75 | Keep | Soft impact = hitting flesh |
| `sfx_enemy_death` | `oga_80-rpg-sfx/creature_die_01.ogg` | 0.65 | Keep | Deep pitch for satisfying kill |
| `sfx_player_hit` | `oga_80-rpg-sfx/creature_hurt_02.ogg` | 0.75 | **Changed** | Was hurt_01. Variant 02 for different pain sound |
| `sfx_player_death` | `oga_80-rpg-sfx/creature_monster_02.ogg` | 0.60 | **Changed** | Was monster_01. Variant 02, very low pitch for dread |

### Skills

| Sound | Source File | Pitch | Change? | Rationale |
|---|---|---|---|---|
| `sfx_skill_fire` | `oga_80-rpg-sfx/spell_fire_03.ogg` | 0.85 | Keep | Purpose-made fire spell |
| `sfx_skill_ice` | `kenney_impact-sounds/Audio/impactGlass_light_000.ogg` | 0.85 | **Changed** | Was glass_003. Variant 000 for crisper crystalline crack |
| `sfx_skill_lightning` | `oga_50-retro-synth/synth_laser_05.ogg` | 0.90 | **Changed** | Was laser_03. Variant 05 for more variety. Synth lasers work as electric zaps |
| `sfx_skill_blood` | `oga_80-rpg-sfx/creature_slime_02.ogg` | 0.80 | Keep | Best available wet/blood squelch |
| `sfx_skill_dash` | `oga_swishes/swishes/swish-1.wav` | 1.0 | **Changed** | Was swish-5. swish-1 is quicker/shorter for instantaneous dash feel. No pitch shift = fast and sharp |
| `sfx_skill_heal` | `oga_100-cc0-sfx/bell_01.ogg` | 1.0 | **Changed** | Was spell_fire_04 (fire sound for heal!). bell_01 = bright chime that reads as restorative. No pitch shift = positive contrast |
| `sfx_skill_buff` | `oga_50-retro-synth/power_up_04.ogg` | 0.95 | **Changed** | Was power_up_03. Different variant, nearly natural pitch |
| `sfx_skill_summon` | `oga_80-rpg-sfx/misc_03.ogg` | 0.75 | **Changed** | Was misc_02. Variant 03 for more mystical quality, pitched down for otherworldly feel |
| `sfx_skill_explosion` | `oga_100-cc0-sfx/explosion.ogg` | 0.70 | Keep | Only real explosion available. Pitched down for deeper boom |
| `sfx_skill_stun` | `oga_thwack/PCM/thwack-08.wav` | 0.75 | **Changed** | Was thwack-05. Variant 08 for heavier concussive hit |

### Items

| Sound | Source File | Pitch | Change? | Rationale |
|---|---|---|---|---|
| `sfx_item_pickup` | `oga_80-rpg-sfx/item_coins_02.ogg` | 0.95 | **Changed** | Was coins_01. Different jingle variant |
| `sfx_item_equip` | `oga_80-rpg-sfx/metal_02.ogg` | 0.85 | **Changed** | Was metal_01. Different clank variant |
| `sfx_item_drop` | `oga_80-rpg-sfx/item_wood_02.ogg` | 0.90 | **Changed** | Was wood_01. Different thud variant |
| `sfx_potion_use` | `oga_100-cc0-sfx/plop_02.ogg` | 0.90 | **Changed** | Was plop_01. Different liquid plop variant |

### UI

| Sound | Source File | Pitch | Change? | Rationale |
|---|---|---|---|---|
| `sfx_ui_click` | `kenney_ui-audio/Audio/click3.ogg` | 1.0 | Keep | Clean click, no pitch shift |
| `sfx_ui_back` | `kenney_ui-audio/Audio/click5.ogg` | 1.0 | Keep | Distinct from click3 |
| `sfx_ui_confirm` | `kenney_ui-audio/Audio/switch3.ogg` | 1.0 | Keep | Good confirmation switch |
| `sfx_menu_hover` | `kenney_ui-audio/Audio/rollover1.ogg` | 1.0 | Keep | Subtle hover tick |

### Footsteps

| Sound | Source File | Pitch | Change? | Rationale |
|---|---|---|---|---|
| `sfx_footstep_stone` | `kenney_impact-sounds/Audio/footstep_concrete_000.ogg` | 0.80 | **Changed** | Was concrete_001. Variant 000 (cleanest sample), pitched down for boots on dungeon stone |
| `sfx_footstep_metal` | `kenney_rpg-audio/Audio/footstep03.ogg` | 0.85 | Keep | Good metallic footstep |
| `sfx_enemy_footstep` | `kenney_impact-sounds/Audio/footstep_wood_004.ogg` | 0.70 | **Changed** | Was wood_002. Variant 004, pitched significantly lower = heavier/more threatening |

### Enemies

| Sound | Source File | Pitch | Change? | Rationale |
|---|---|---|---|---|
| `sfx_enemy_attack` | `oga_80-rpg-sfx/creature_roar_01.ogg` | 0.75 | **Changed** | Was roar_03. Using roar_01 as shorter attack bark. Reserves deeper sounds for boss |
| `sfx_boss_roar` | `oga_80-rpg-sfx/creature_monster_04.ogg` | 0.60 | **Changed** | Was monster_03. Variant 04, pitched very low = most intimidating sound in the game |
| `sfx_boss_stomp` | `kenney_impact-sounds/Audio/impactPlate_heavy_000.ogg` | 0.60 | **Changed** | Was plate_002. Variant 000, pitched very low = ground-shaking stomp |

### Environment

| Sound | Source File | Pitch | Change? | Rationale |
|---|---|---|---|---|
| `sfx_door_open` | `kenney_rpg-audio/Audio/doorOpen_2.ogg` | 0.80 | **Changed** | Was doorOpen_1. doorOpen_2 has more creak character for heavy dungeon doors |
| `sfx_level_up` | `oga_80-rpg-sfx/item_gem_02.ogg` | 1.0 | **Changed** | Was gem_01. Different chime variant. No pitch shift = bright, rewarding contrast |

---

## Pitch Philosophy

| Category | Range | Design Intent |
|---|---|---|
| UI | 1.0 | Crisp, functional, no mood coloring |
| Heal, level up | 1.0 | Bright, rewarding — positive contrast against dark dungeon |
| Items, buff, dash | 0.90-1.0 | Mostly natural, slight warmth |
| Skills/magic | 0.75-0.90 | Dungeon atmosphere darkening |
| Footsteps | 0.70-0.85 | Weighted, grounded |
| Melee weapons | 0.65-0.90 | Weight hierarchy: dagger > sword > axe = claymore |
| Ranged weapons | 0.65-0.90 | Weight hierarchy: SMG > pistol = carbine > revolver |
| Creatures/death | 0.60-0.75 | Dark, menacing |
| Boss sounds | 0.60 | Maximum darkness and weight |

---

## Files to Modify

- `tools/fetch_audio.py` — Update `MANUAL_OVERRIDES` (26 entries), add `PITCH_SHIFTS` dict (48 entries), replace Phase 3 global pitch loop with per-sound pitch

## Verification

1. Run `python3 tools/fetch_audio.py` and confirm all 48 WAVs are generated in `assets/audio/`
2. Build and launch the game: `cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build && ./build/dungeon_game`
3. Playtest each category:
   - Swing sword/dagger/axe/claymore — verify distinct sounds and weight hierarchy
   - Fire pistol/smg/carbine/revolver — verify no arcade-y synth sounds remain
   - Use skills (heal, fire, ice, lightning) — verify heal is a chime not fire
   - Pick up items, equip, use potions — verify crisp feedback
   - Navigate menus — verify UI sounds are not pitched down
   - Encounter enemies and boss — verify audio hierarchy (normal attack vs boss roar)
   - Level up — verify bright, rewarding sound
4. If any sound feels wrong, swap the source variant in `MANUAL_OVERRIDES` or adjust pitch in `PITCH_SHIFTS` and re-run `fetch_audio.py`
