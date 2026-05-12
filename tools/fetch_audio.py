#!/usr/bin/env python3
"""Download CC0 sound effect packs, extract, and map to DungeonEngine's SfxId naming.

Downloads four CC0 audio packs (Kenney RPG Audio, Kenney Impact Sounds,
Kenney UI Audio, OpenGameArt 80 CC0 RPG SFX), extracts them to a local cache,
then searches extracted WAV files by keyword and copies the best matches into
assets/audio/ using our sfx_*.wav naming convention. Files are normalized to
44100 Hz, 16-bit, mono WAV with peak at -3 dB and silence trimmed.

Falls back to gen_audio.py procedural synthesis for any sounds that can't be
matched from the downloaded packs.

Usage:
    python3 tools/fetch_audio.py              # download + map all
    python3 tools/fetch_audio.py --list-packs # show available sounds in packs
    python3 tools/fetch_audio.py --cache-only # download but don't map
"""

import argparse
import math
import os
import shutil
import struct
import sys
import tempfile
import urllib.error
import urllib.request
import wave
import zipfile

# ---------------------------------------------------------------------------
# Project paths
# ---------------------------------------------------------------------------

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
GAME_ROOT = os.path.dirname(SCRIPT_DIR)
CACHE_DIR = os.path.join(GAME_ROOT, "build", "audio_cache")
OUTPUT_DIR = os.path.join(GAME_ROOT, "assets", "audio")

# ---------------------------------------------------------------------------
# CC0 sound packs to download
# ---------------------------------------------------------------------------

PACKS = [
    (
        "kenney_rpg-audio",
        "https://kenney.nl/media/pages/assets/rpg-audio/706161bc16-1677590336/kenney_rpg-audio.zip",
    ),
    (
        "kenney_impact-sounds",
        "https://kenney.nl/media/pages/assets/impact-sounds/8aa7b545c9-1677589768/kenney_impact-sounds.zip",
    ),
    (
        "kenney_ui-audio",
        "https://kenney.nl/media/pages/assets/ui-audio/e19c9b1814-1677590494/kenney_ui-audio.zip",
    ),
    (
        "oga_80-rpg-sfx",
        "https://opengameart.org/sites/default/files/80-CC0-RPG-SFX_0.zip",
    ),
    (
        "oga_100-cc0-sfx",
        "https://opengameart.org/sites/default/files/100-CC0-SFX_0.zip",
    ),
    (
        "oga_swishes",
        "https://opengameart.org/sites/default/files/swishes.zip",
    ),
    (
        "oga_thwack",
        "https://opengameart.org/sites/default/files/thwack-1.0.zip",
    ),
    (
        "oga_50-retro-synth",
        "https://opengameart.org/sites/default/files/50-CC0-retro-synth-SFX.zip",
    ),
]

# ---------------------------------------------------------------------------
# Keyword mapping: our sfx name -> search keywords + preferences
#
# Each entry is (keywords, preference) where preference is one of:
#   "short"  — prefer shorter files (UI clicks, impacts)
#   "long"   — prefer longer files (boss roars, death cries)
#   "medium" — no strong preference
# ---------------------------------------------------------------------------

SOUND_MAP = {
    # Weapons — melee
    "sfx_weapon_sword":     (["sword", "blade", "slash", "slice"], "short"),
    "sfx_weapon_dagger":    (["dagger", "knife", "stab", "slice"], "short"),
    "sfx_weapon_axe":       (["axe", "chop", "cleave"], "short"),
    "sfx_weapon_claymore":  (["sword", "heavy", "slash", "blade"], "short"),
    # Weapons — ranged
    "sfx_weapon_pistol":    (["pistol", "gun", "shot", "bang", "shoot"], "short"),
    "sfx_weapon_smg":       (["gun", "shot", "pop", "bang", "shoot"], "short"),
    "sfx_weapon_carbine":   (["rifle", "gun", "shot", "bang", "crack", "shoot"], "short"),
    "sfx_weapon_revolver":  (["gun", "shot", "heavy", "bang", "shoot"], "short"),
    "sfx_weapon_bow":       (["bow", "arrow", "twang", "shoot"], "short"),
    "sfx_weapon_crossbow":  (["crossbow", "bolt", "thwack", "crack", "snap"], "short"),
    "sfx_weapon_throw":     (["swish", "whoosh", "throw", "wind"], "short"),
    "sfx_weapon_molotov":   (["glass", "break", "fire", "shatter"], "short"),
    "sfx_weapon_wand":      (["magic", "spell", "zap", "magical"], "short"),
    "sfx_weapon_staff":     (["magic", "spell", "cast", "magical"], "medium"),
    "sfx_reload":           (["reload", "cock", "click", "metal", "latch"], "short"),
    # Combat hits
    "sfx_hit_melee":        (["hit", "punch", "impact", "thud", "melee"], "short"),
    "sfx_hit_hitscan":      (["hit", "impact", "bullet", "soft"], "short"),
    "sfx_hit_projectile":   (["hit", "arrow", "impact", "plate"], "short"),
    "sfx_enemy_hit":        (["hit", "impact", "flesh", "tin"], "short"),
    "sfx_enemy_death":      (["death", "die", "creature", "monster"], "medium"),
    "sfx_player_hit":       (["hurt", "pain", "hit", "creature_hurt"], "short"),
    "sfx_player_death":     (["death", "die", "fall", "creature_die"], "medium"),
    # Skills — magic
    "sfx_skill_fire":       (["fire", "flame", "burn", "spell_fire"], "medium"),
    "sfx_skill_ice":        (["ice", "freeze", "crystal", "crack", "glass", "break"], "medium"),
    "sfx_skill_lightning":  (["lightning", "electric", "shock", "zap", "spark", "laser"], "medium"),
    "sfx_skill_blood":      (["blood", "splat", "gore", "squelch", "wet", "creature"], "short"),
    "sfx_skill_dash":       (["swish", "whoosh", "dash", "wind", "air"], "short"),
    "sfx_skill_heal":       (["heal", "chime", "magical", "spell", "restore", "powerup"], "medium"),
    "sfx_skill_buff":       (["buff", "powerup", "enhance", "boost", "power"], "medium"),
    "sfx_skill_summon":     (["summon", "portal", "magical", "spell", "warp"], "medium"),
    "sfx_skill_explosion":  (["explosion", "boom", "blast", "explode", "bang"], "medium"),
    "sfx_skill_stun":       (["stun", "thwack", "thud", "bang", "concuss", "crack"], "short"),
    # Items
    "sfx_item_pickup":      (["pickup", "coin", "collect", "loot", "coins"], "short"),
    "sfx_item_equip":       (["equip", "armor", "metal", "clank", "metalPot"], "short"),
    "sfx_item_drop":        (["drop", "thud", "place", "book"], "short"),
    "sfx_potion_use":       (["potion", "drink", "gulp", "liquid", "bottle", "swallow"], "short"),
    # UI
    "sfx_ui_click":         (["click", "select", "button"], "short"),
    "sfx_ui_back":          (["back", "cancel", "close", "doorClose"], "short"),
    "sfx_ui_confirm":       (["confirm", "accept", "start", "switch", "rollover"], "short"),
    "sfx_menu_hover":       (["hover", "tick", "move", "rollover"], "short"),
    # Movement
    "sfx_footstep_stone":   (["step", "foot", "stone", "walk", "concrete"], "short"),
    "sfx_footstep_metal":   (["step", "foot", "metal"], "short"),
    "sfx_enemy_footstep":   (["step", "foot", "heavy", "wood"], "short"),
    # Enemies
    "sfx_enemy_attack":     (["attack", "swipe", "claw", "creature", "roar"], "short"),
    "sfx_boss_roar":        (["roar", "growl", "monster", "boss", "creature_roar"], "long"),
    "sfx_boss_stomp":       (["stomp", "quake", "heavy", "slam", "bell"], "medium"),
    # Environment
    "sfx_door_open":        (["door", "open", "creak", "doorOpen"], "medium"),
    "sfx_level_up":         (["level", "fanfare", "victory", "win", "jingle", "success"], "long"),
}

# Manual overrides — bypass keyword matching for sounds that need specific files.
# Paths are relative to the cache directory.
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
    "sfx_weapon_wand":      "kenney_impact-sounds/Audio/impactSoft_medium_002.ogg",  # soft whomp — pitched up becomes a punchy magic bolt
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
    "sfx_weapon_wand":      1.15,   # pitched UP — makes slam sound like a punchy magic bolt
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

# Per-sound reverb: ffmpeg aecho params "in_gain:out_gain:delays:decays"
# Only sounds that need extra tail/space. Most sounds stay dry.
REVERB = {
    "sfx_skill_stun":       "0.8:0.7:40|80:0.4|0.2",    # holy smite uses this — needs divine reverb
    "sfx_skill_heal":       "0.8:0.6:60|120:0.3|0.15",   # healing chime benefits from space
    "sfx_boss_roar":        "0.8:0.7:50|100:0.5|0.25",   # big creature in a dungeon
}

# ---------------------------------------------------------------------------
# Download helpers
# ---------------------------------------------------------------------------

def _download_with_progress(url, dest_path):
    """Download a URL to dest_path, printing a progress bar to stdout."""
    try:
        req = urllib.request.Request(url, headers={"User-Agent": "DungeonEngine/1.0"})
        resp = urllib.request.urlopen(req, timeout=60)
    except (urllib.error.URLError, urllib.error.HTTPError, OSError) as e:
        print(f"  ERROR: failed to connect: {e}")
        return False

    total = resp.headers.get("Content-Length")
    total = int(total) if total else None
    downloaded = 0
    block_size = 64 * 1024

    try:
        with open(dest_path, "wb") as f:
            while True:
                chunk = resp.read(block_size)
                if not chunk:
                    break
                f.write(chunk)
                downloaded += len(chunk)
                if total:
                    pct = downloaded * 100 // total
                    bar_len = 40
                    filled = bar_len * downloaded // total
                    bar = "#" * filled + "-" * (bar_len - filled)
                    mb_done = downloaded / (1024 * 1024)
                    mb_total = total / (1024 * 1024)
                    print(f"\r  [{bar}] {pct:3d}% ({mb_done:.1f}/{mb_total:.1f} MB)", end="", flush=True)
                else:
                    mb_done = downloaded / (1024 * 1024)
                    print(f"\r  Downloaded {mb_done:.1f} MB...", end="", flush=True)
    except (urllib.error.URLError, OSError) as e:
        print(f"\n  ERROR: download interrupted: {e}")
        if os.path.exists(dest_path):
            os.remove(dest_path)
        return False

    print()  # newline after progress bar
    return True


def _extract_zip(zip_path, dest_dir):
    """Extract a ZIP file, verifying integrity. Returns True on success."""
    try:
        with zipfile.ZipFile(zip_path, "r") as zf:
            # Verify CRC integrity
            bad = zf.testzip()
            if bad is not None:
                print(f"  WARNING: corrupt entry in ZIP: {bad}")
            zf.extractall(dest_dir)
        return True
    except (zipfile.BadZipFile, OSError) as e:
        print(f"  ERROR: failed to extract ZIP: {e}")
        return False


# ---------------------------------------------------------------------------
# Audio normalization
# ---------------------------------------------------------------------------

def _read_wav_raw(path):
    """Read a WAV file and return (samples_float_list, sample_rate, n_channels, sampwidth).

    Returns float samples in [-1.0, 1.0] range. Handles 8-bit, 16-bit, and
    24-bit PCM. Returns None on failure.
    """
    try:
        with wave.open(path, "r") as wf:
            n_channels = wf.getnchannels()
            sampwidth = wf.getsampwidth()
            sample_rate = wf.getframerate()
            n_frames = wf.getnframes()
            raw = wf.readframes(n_frames)
    except (wave.Error, EOFError, OSError) as e:
        print(f"  WARNING: cannot read WAV '{path}': {e}")
        return None

    samples = []
    if sampwidth == 1:
        # 8-bit unsigned PCM
        for b in raw:
            samples.append((b - 128) / 128.0)
    elif sampwidth == 2:
        # 16-bit signed PCM
        count = len(raw) // 2
        unpacked = struct.unpack(f"<{count}h", raw)
        samples = [s / 32768.0 for s in unpacked]
    elif sampwidth == 3:
        # 24-bit signed PCM — unpack 3 bytes at a time
        for i in range(0, len(raw), 3):
            if i + 3 > len(raw):
                break
            # Little-endian 24-bit: pad to 32-bit signed
            val = raw[i] | (raw[i + 1] << 8) | (raw[i + 2] << 16)
            if val & 0x800000:
                val -= 0x1000000
            samples.append(val / 8388608.0)
    elif sampwidth == 4:
        # 32-bit signed PCM
        count = len(raw) // 4
        unpacked = struct.unpack(f"<{count}i", raw)
        samples = [s / 2147483648.0 for s in unpacked]
    else:
        print(f"  WARNING: unsupported sample width {sampwidth} in '{path}'")
        return None

    return samples, sample_rate, n_channels, sampwidth


def _stereo_to_mono(samples, n_channels):
    """Average multi-channel samples down to mono."""
    if n_channels == 1:
        return samples
    mono = []
    for i in range(0, len(samples), n_channels):
        total = 0.0
        count = 0
        for ch in range(n_channels):
            idx = i + ch
            if idx < len(samples):
                total += samples[idx]
                count += 1
        mono.append(total / count if count > 0 else 0.0)
    return mono


def _resample_linear(samples, src_rate, dst_rate):
    """Basic linear-interpolation resampler. Good enough for SFX."""
    if src_rate == dst_rate:
        return samples
    if not samples:
        return samples

    ratio = src_rate / dst_rate
    out_len = int(len(samples) / ratio)
    if out_len < 1:
        return samples

    result = []
    for i in range(out_len):
        src_pos = i * ratio
        idx = int(src_pos)
        frac = src_pos - idx
        s0 = samples[min(idx, len(samples) - 1)]
        s1 = samples[min(idx + 1, len(samples) - 1)]
        result.append(s0 + (s1 - s0) * frac)
    return result


def _trim_silence(samples, threshold=0.005):
    """Trim leading and trailing silence below the given amplitude threshold."""
    if not samples:
        return samples

    # Find first sample above threshold
    start = 0
    for i, s in enumerate(samples):
        if abs(s) > threshold:
            start = max(0, i - 64)  # keep a tiny bit of lead-in (64 samples ~1.5ms)
            break
    else:
        return samples  # all silent, return as-is

    # Find last sample above threshold
    end = len(samples)
    for i in range(len(samples) - 1, -1, -1):
        if abs(samples[i]) > threshold:
            end = min(len(samples), i + 64)  # keep a tiny tail
            break

    return samples[start:end]


def _normalize_peak(samples, target_db=-3.0):
    """Normalize peak amplitude to target_db (default -3 dB)."""
    if not samples:
        return samples

    peak = max(abs(s) for s in samples)
    if peak < 0.0001:
        return samples  # effectively silent

    # target_db in linear: 10^(db/20)
    target_linear = 10.0 ** (target_db / 20.0)
    scale = target_linear / peak
    return [s * scale for s in samples]


def normalize_wav(src_path, dst_path):
    """Read a WAV, normalize to 44100 Hz / 16-bit / mono, write to dst_path.

    Performs: stereo->mono, resample, trim silence, peak normalize to -3 dB.
    Returns True on success.
    """
    result = _read_wav_raw(src_path)
    if result is None:
        return False

    samples, src_rate, n_channels, _ = result

    # Stereo to mono
    samples = _stereo_to_mono(samples, n_channels)

    # Resample to 44100 Hz
    samples = _resample_linear(samples, src_rate, 44100)

    # Trim silence
    samples = _trim_silence(samples)

    # Normalize peak to -3 dB
    samples = _normalize_peak(samples, target_db=-3.0)

    # Convert to 16-bit integers
    int_samples = []
    for s in samples:
        clamped = max(-1.0, min(1.0, s))
        int_samples.append(int(clamped * 32767))

    # Write output
    os.makedirs(os.path.dirname(dst_path) or ".", exist_ok=True)
    with wave.open(dst_path, "w") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(44100)
        raw = struct.pack(f"<{len(int_samples)}h", *int_samples)
        wf.writeframes(raw)

    return True


# ---------------------------------------------------------------------------
# Pack management: download + extract
# ---------------------------------------------------------------------------

def download_packs(force=False):
    """Download and extract all packs to CACHE_DIR. Skips if already cached.

    Returns list of (pack_name, pack_dir) for successfully cached packs.
    """
    os.makedirs(CACHE_DIR, exist_ok=True)
    cached = []

    for pack_name, url in PACKS:
        pack_dir = os.path.join(CACHE_DIR, pack_name)
        marker = os.path.join(pack_dir, ".extracted")

        if os.path.exists(marker) and not force:
            print(f"[CACHED] {pack_name}")
            cached.append((pack_name, pack_dir))
            continue

        print(f"[DOWNLOAD] {pack_name}")
        print(f"  URL: {url}")

        # Download to a temp file first, then move on success
        zip_path = os.path.join(CACHE_DIR, f"{pack_name}.zip")
        if not _download_with_progress(url, zip_path):
            print(f"  SKIPPED: download failed for {pack_name}")
            continue

        # Verify it's a valid ZIP
        if not zipfile.is_zipfile(zip_path):
            print(f"  SKIPPED: downloaded file is not a valid ZIP")
            os.remove(zip_path)
            continue

        # Extract
        print(f"  Extracting...")
        if os.path.exists(pack_dir):
            shutil.rmtree(pack_dir)
        os.makedirs(pack_dir, exist_ok=True)

        if not _extract_zip(zip_path, pack_dir):
            print(f"  SKIPPED: extraction failed")
            continue

        # Write marker so we know extraction succeeded
        with open(marker, "w") as f:
            f.write("ok\n")

        # Clean up ZIP to save disk space
        os.remove(zip_path)

        cached.append((pack_name, pack_dir))
        print(f"  OK")

    return cached


# ---------------------------------------------------------------------------
# File discovery: find all WAV files in the cache
# ---------------------------------------------------------------------------

def _find_wav_files(pack_dirs):
    """Walk all pack directories and return a list of absolute WAV file paths."""
    wav_files = []
    for _, pack_dir in pack_dirs:
        for root, _dirs, files in os.walk(pack_dir):
            for fname in files:
                if fname.lower().endswith((".wav", ".ogg")):
                    full_path = os.path.join(root, fname)
                    # Skip macOS resource fork junk
                    if "__MACOSX" in full_path or fname.startswith("._"):
                        continue
                    wav_files.append(full_path)
    return wav_files


def _wav_duration_seconds(path):
    """Return the duration of a WAV file in seconds, or None on error."""
    try:
        with wave.open(path, "r") as wf:
            return wf.getnframes() / wf.getframerate()
    except (wave.Error, OSError):
        return None


# ---------------------------------------------------------------------------
# Keyword matching: find the best source file for each target sound
# ---------------------------------------------------------------------------

def _score_file(wav_path, keywords, preference):
    """Score a WAV file against keywords and duration preference.

    Returns a numeric score; higher is better. Returns 0 if no keywords match.
    """
    # Use the filename (without extension) and parent directory name for matching
    fname = os.path.splitext(os.path.basename(wav_path))[0].lower()
    parent = os.path.basename(os.path.dirname(wav_path)).lower()
    searchable = f"{parent}/{fname}"

    # Replace common separators with spaces for word matching
    for sep in "-", "_", ".", " ":
        searchable = searchable.replace(sep, " ")

    words = set(searchable.split())

    # Count keyword hits
    hits = 0
    for kw in keywords:
        kw_lower = kw.lower()
        # Exact word match scores higher than substring
        if kw_lower in words:
            hits += 2
        elif kw_lower in searchable:
            hits += 1

    if hits == 0:
        return 0

    # Duration preference scoring — get file duration
    dur = _wav_duration_seconds(wav_path)
    if dur is None:
        dur = 0.5  # assume moderate if we can't read

    dur_score = 0.0
    if preference == "short":
        # Prefer files under 0.5s, penalize long files
        if dur <= 0.5:
            dur_score = 1.0
        elif dur <= 1.0:
            dur_score = 0.5
        else:
            dur_score = 0.2
    elif preference == "long":
        # Prefer files over 0.5s
        if dur >= 1.0:
            dur_score = 1.0
        elif dur >= 0.5:
            dur_score = 0.7
        else:
            dur_score = 0.3
    else:  # medium
        # Prefer 0.2-1.0s range
        if 0.2 <= dur <= 1.0:
            dur_score = 1.0
        elif dur < 0.2:
            dur_score = 0.5
        else:
            dur_score = 0.6

    # Combined score: keyword hits are primary, duration is secondary
    return hits * 10 + dur_score


def find_best_matches(wav_files):
    """For each sound in SOUND_MAP, find the best matching WAV from the packs.

    Manual overrides (MANUAL_OVERRIDES dict) are checked first — if a specific
    file is assigned and exists, it's used directly. Otherwise falls back to
    keyword matching.

    Returns dict: sfx_name -> source_wav_path (or None if no match).
    """
    matches = {}
    # Track which source files have been used to avoid duplicates
    used_files = set()

    for sfx_name, (keywords, preference) in SOUND_MAP.items():
        # Check manual override first
        if sfx_name in MANUAL_OVERRIDES:
            override_path = os.path.join(CACHE_DIR, MANUAL_OVERRIDES[sfx_name])
            if os.path.isfile(override_path):
                matches[sfx_name] = override_path
                used_files.add(override_path)
                continue
        scored = []
        for wav_path in wav_files:
            score = _score_file(wav_path, keywords, preference)
            if score > 0:
                scored.append((score, wav_path))

        # Sort by score descending
        scored.sort(key=lambda x: -x[0])

        # Pick the best unused file
        best = None
        for score, path in scored:
            if path not in used_files:
                best = path
                used_files.add(path)
                break

        # If all top matches are used, allow reuse from the best
        if best is None and scored:
            best = scored[0][1]

        matches[sfx_name] = best

    return matches


# ---------------------------------------------------------------------------
# Fallback: invoke gen_audio.py for missing sounds
# ---------------------------------------------------------------------------

def _run_gen_audio_fallback(sfx_name):
    """Generate a procedural fallback sound using gen_audio.py.

    Returns True if the fallback was generated successfully.
    """
    # Strip the "sfx_" prefix to get the gen_audio preset name
    preset = sfx_name[4:] if sfx_name.startswith("sfx_") else sfx_name

    gen_audio_path = os.path.join(SCRIPT_DIR, "gen_audio.py")
    if not os.path.exists(gen_audio_path):
        return False

    # Import gen_audio and generate the sound directly
    try:
        # Add tools dir to path so we can import gen_audio
        if SCRIPT_DIR not in sys.path:
            sys.path.insert(0, SCRIPT_DIR)
        import gen_audio
        if preset in gen_audio.SOUND_PRESETS:
            gen_audio.generate_sound(preset)
            return True
    except Exception as e:
        print(f"  WARNING: gen_audio fallback failed for '{preset}': {e}")

    return False


# ---------------------------------------------------------------------------
# Main pipeline: download -> match -> copy/normalize
# ---------------------------------------------------------------------------

def map_and_copy(pack_dirs, dry_run=False):
    """Find best keyword matches and copy/normalize to assets/audio/.

    Returns (mapped_count, fallback_count, missing_count).
    """
    print("\n--- Scanning packs for WAV files ---")
    wav_files = _find_wav_files(pack_dirs)
    print(f"Found {len(wav_files)} WAV files across {len(pack_dirs)} packs")

    print("\n--- Matching sounds by keyword ---")
    matches = find_best_matches(wav_files)

    os.makedirs(OUTPUT_DIR, exist_ok=True)

    mapped = 0
    fallback = 0
    missing = 0

    for sfx_name in sorted(SOUND_MAP.keys()):
        src = matches.get(sfx_name)
        # Use the source file's extension (OGG or WAV)
        src_ext = os.path.splitext(src)[1].lower() if src else ".wav"
        dst = os.path.join(OUTPUT_DIR, f"{sfx_name}{src_ext}")
        keywords = SOUND_MAP[sfx_name][0]

        if src is not None:
            # Show which pack file matched
            rel_src = os.path.relpath(src, CACHE_DIR)
            print(f"  {sfx_name:28s} <- {rel_src}")

            if not dry_run:
                # OGG files: copy directly (SDL_mixer handles them natively)
                # WAV files: normalize to 44100Hz mono
                if src.lower().endswith(".ogg"):
                    shutil.copy2(src, dst)
                    ok = True
                else:
                    ok = normalize_wav(src, dst)
                if ok:
                    dur_str = "?.??s"
                    size_kb = os.path.getsize(dst) / 1024
                    print(f"  {'':28s}    -> {size_kb:.0f} KB, {dur_str}")
                    mapped += 1
                else:
                    # Normalization failed — try fallback
                    print(f"  {'':28s}    WARNING: normalization failed, trying fallback")
                    if _run_gen_audio_fallback(sfx_name):
                        print(f"  {'':28s}    -> fallback (procedural)")
                        fallback += 1
                    else:
                        print(f"  {'':28s}    MISSING: no fallback available")
                        missing += 1
            else:
                mapped += 1
        else:
            # No match found — try procedural fallback
            print(f"  {sfx_name:28s} <- [no match for: {', '.join(keywords)}]")
            if not dry_run:
                if _run_gen_audio_fallback(sfx_name):
                    print(f"  {'':28s}    -> fallback (procedural)")
                    fallback += 1
                else:
                    print(f"  {'':28s}    MISSING")
                    missing += 1
            else:
                missing += 1

    return mapped, fallback, missing


def list_packs(pack_dirs):
    """List all WAV files found in the downloaded packs."""
    wav_files = _find_wav_files(pack_dirs)

    for pack_name, pack_dir in pack_dirs:
        print(f"\n=== {pack_name} ===")
        pack_wavs = [f for f in wav_files if f.startswith(pack_dir)]
        pack_wavs.sort()
        for wav_path in pack_wavs:
            rel = os.path.relpath(wav_path, pack_dir)
            dur = _wav_duration_seconds(wav_path)
            dur_str = f"{dur:.2f}s" if dur else "?.??s"
            print(f"  {rel:60s}  {dur_str}")
        print(f"  ({len(pack_wavs)} WAV files)")

    print(f"\nTotal: {len(wav_files)} WAV files across {len(pack_dirs)} packs")


# ---------------------------------------------------------------------------
# CLI entry point
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="Download CC0 sound effect packs and map to DungeonEngine SfxId names."
    )
    parser.add_argument(
        "--list-packs",
        action="store_true",
        help="Show available sounds in downloaded packs",
    )
    parser.add_argument(
        "--cache-only",
        action="store_true",
        help="Download and extract packs but don't map sounds",
    )
    parser.add_argument(
        "--force-download",
        action="store_true",
        help="Re-download packs even if cached",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Show what would be mapped without copying files",
    )
    args = parser.parse_args()

    print("=== DungeonEngine CC0 Audio Fetcher ===")
    print(f"Cache directory: {CACHE_DIR}")
    print(f"Output directory: {OUTPUT_DIR}")
    print()

    # Phase 1: Download and extract packs
    print("--- Phase 1: Download CC0 sound packs ---")
    pack_dirs = download_packs(force=args.force_download)

    if not pack_dirs:
        print("\nERROR: No packs were downloaded successfully.")
        print("Check your internet connection and try again.")
        sys.exit(1)

    print(f"\n{len(pack_dirs)}/{len(PACKS)} packs available")

    if args.list_packs:
        list_packs(pack_dirs)
        return

    if args.cache_only:
        print("\nCache populated. Run without --cache-only to map sounds.")
        return

    # Phase 2: Match and copy
    print("\n--- Phase 2: Map sounds to SfxId convention ---")
    mapped, fallback, missing_count = map_and_copy(pack_dirs, dry_run=args.dry_run)

    # Summary
    total = len(SOUND_MAP)
    print("\n--- Summary ---")
    print(f"Total target sounds: {total}")
    print(f"Mapped from packs:   {mapped}")
    print(f"Procedural fallback: {fallback}")
    print(f"Missing:             {missing_count}")

    if args.dry_run:
        print("\n(Dry run — no files were copied)")

    if missing_count > 0:
        print(f"\nWARNING: {missing_count} sound(s) could not be sourced.")
        print("Run 'python3 tools/gen_audio.py --all' to generate procedural fallbacks.")

    if mapped + fallback == total:
        print(f"\nAll {total} sounds are ready in {OUTPUT_DIR}")

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
            reverb = REVERB.get(sfx_name)

            # Build the ffmpeg filter chain: pitch shift + optional reverb
            filters = []
            if pitch != 1.0:
                filters.append(f"asetrate=44100*{pitch}")
                filters.append("aresample=44100")
            if reverb:
                filters.append(f"aecho={reverb}")

            if not filters and not fname.endswith(".ogg"):
                # No processing needed — WAV at natural pitch, no reverb
                skipped += 1
                continue

            tmp = src + ".tmp.wav"
            cmd = [ffmpeg, "-y", "-i", src]
            if filters:
                cmd += ["-af", ",".join(filters)]
            cmd += ["-ar", "44100", "-ac", "1", "-sample_fmt", "s16", tmp]
            ret = subprocess.run(cmd, capture_output=True)
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


if __name__ == "__main__":
    main()
