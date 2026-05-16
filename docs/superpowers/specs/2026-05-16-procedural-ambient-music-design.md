# Procedural Ambient Music Design

## Context

The game has no background music. Adding dark ambient tracks per floor tier would massively improve atmosphere. The audio system already supports `AudioSystem::playMusic(path)` for looping background music. All tracks are procedurally generated using the existing `gen_audio.py` synthesizer — zero licensing concerns.

## Tracks

5 tracks, one per floor tier, progressively darker:

| Tier | Floors | Theme | Layers |
|------|--------|-------|--------|
| 1 | 1-10 | Stone Dungeon | Low sine drone (80Hz) + filtered noise wind. Sparse, eerie. |
| 2 | 11-20 | Catacombs | Deeper drone (50Hz) + detuned sine harmonics + slow pulse. Ominous. |
| 3 | 21-30 | Caverns | Heavy sub-bass (40Hz) + noise drip bursts + heavy low-pass. Oppressive. |
| 4 | 31-40 | Hellforge | Sawtooth drones + distorted noise crackle + fast sub-pulse. Infernal. |
| 5 | 41-50 | Void | Ultra-low sine (30Hz) + dissonant intervals + alien modulation. Otherworldly. |

Each track:
- ~60 seconds, seamlessly loopable
- 3-5 mixed layers (bass drone + mid pad + texture + optional percussion)
- 44100Hz, 16-bit mono WAV
- ~5.3MB per track, ~26MB total

## Generation

Add 5 new functions to `tools/gen_audio.py`:
- `music_tier1_dungeon()` through `music_tier5_void()`
- Each returns a sample list built from `synthesize_mix()` of 3-5 layers
- Layers use existing synth primitives: sine, sawtooth, noise + ADSR + low-pass + vibrato
- Fade-in first 2s and fade-out last 2s for seamless looping
- Register in a `MUSIC_PRESETS` dict similar to `SOUND_PRESETS`

Output files:
- `assets/audio/music_tier1.wav` through `assets/audio/music_tier5.wav`

## Integration

### Music selection (`src/engine/engine_startgame.cpp`)

At the end of `startGame()`, after all level setup:

```cpp
const char* musicFile = "assets/audio/music_tier1.wav";
if (m_level.currentFloor >= 41) musicFile = "assets/audio/music_tier5.wav";
else if (m_level.currentFloor >= 31) musicFile = "assets/audio/music_tier4.wav";
else if (m_level.currentFloor >= 21) musicFile = "assets/audio/music_tier3.wav";
else if (m_level.currentFloor >= 11) musicFile = "assets/audio/music_tier2.wav";
AudioSystem::playMusic(ASSET_PATH(musicFile));
```

On Switch, `ASSET_PATH` maps to `romfs:/audio/music_tierN.wav`.

### Menu / death silence

Stop music when returning to menu or game over:
```cpp
AudioSystem::stopMusic();
```

### Volume

Music volume should be lower than SFX (atmospheric, not dominant). Use `AudioSystem::setMusicVolume(0.3f)` at init.

## Files to Modify

| File | Change |
|------|--------|
| `tools/gen_audio.py` | Add 5 `music_tierN()` functions + `MUSIC_PRESETS` dict + CLI support |
| `src/engine/engine_startgame.cpp` | Play tier-appropriate music after level setup |
| `src/engine/engine_menu.cpp` | Stop music on return to menu |
| `src/engine/engine_init.cpp` | Set default music volume to 0.3 |

## Generation Command

```bash
python3 tools/gen_audio.py --music-all
```

Or individual:
```bash
python3 tools/gen_audio.py --music tier1
```

## Verification

1. Start new game — tier 1 music plays (low drone, eerie)
2. Descend to floor 11 — tier 2 music starts (deeper, ominous)
3. Continue through tiers — each progressively darker
4. Return to menu — music stops
5. Die — music stops
6. Music loops seamlessly after 60s
7. SFX clearly audible over music (music at 30% volume)
8. Build PC + Switch
