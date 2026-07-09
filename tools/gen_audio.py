#!/usr/bin/env python3
"""Procedural sound effect generator using sfxr-style waveform synthesis.

Pure Python — no external dependencies beyond the standard library.
Generates punchy, chunky, retro/arcade sound effects matching the
Barony-style low-poly aesthetic of DungeonEngine.

Usage:
    python3 tools/gen_audio.py --all                 # generate all sounds
    python3 tools/gen_audio.py --type weapon_sword    # generate one
    python3 tools/gen_audio.py --list                 # list all presets
    python3 tools/gen_audio.py --out path.wav         # custom output path
"""

import argparse
import json
import math
import os
import random
import struct
import wave


def _load_manifest_slots():
    """Slot stems hand-picked via pick_sfx.py (tools/sound_selection.json).

    Bulk regeneration (--all) must not clobber these — pick_sfx.py owns them. Returns e.g.
    {"sfx_weapon_pistol", ...}; empty set if the manifest is absent/malformed.
    """
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "sound_selection.json")
    if not os.path.isfile(path):
        return set()
    try:
        with open(path) as f:
            m = json.load(f)
        if isinstance(m, dict) and isinstance(m.get("slots"), dict):
            return set(m["slots"].keys())
    except (OSError, ValueError):
        pass
    return set()


# ---------------------------------------------------------------------------
# Synthesis engine
# ---------------------------------------------------------------------------

def _waveform_square(phase):
    """Square wave: +1 / -1 at 50% duty cycle."""
    return 1.0 if (phase % 1.0) < 0.5 else -1.0


def _waveform_sawtooth(phase):
    """Sawtooth wave: ramps -1 to +1 over one cycle."""
    return 2.0 * (phase % 1.0) - 1.0


def _waveform_sine(phase):
    """Sine wave."""
    return math.sin(2.0 * math.pi * phase)


def _waveform_triangle(phase):
    """Triangle wave: linearly ramps up then down."""
    p = phase % 1.0
    return 4.0 * p - 1.0 if p < 0.5 else 3.0 - 4.0 * p


def _waveform_noise(_phase):
    """White noise — phase is ignored, each sample is independent."""
    return random.uniform(-1.0, 1.0)


_WAVEFORMS = {
    'square':   _waveform_square,
    'sawtooth': _waveform_sawtooth,
    'sine':     _waveform_sine,
    'triangle': _waveform_triangle,
    'noise':    _waveform_noise,
}


def _adsr_envelope(sample_idx, sample_rate, attack, decay, sustain_level,
                   sustain_time, release):
    """Return envelope amplitude [0..1] for the given sample index."""
    t = sample_idx / sample_rate
    if t < attack:
        # Attack: ramp 0 -> 1
        return t / attack if attack > 0.0 else 1.0
    t -= attack
    if t < decay:
        # Decay: ramp 1 -> sustain_level
        return 1.0 - (1.0 - sustain_level) * (t / decay) if decay > 0.0 else sustain_level
    t -= decay
    if t < sustain_time:
        # Sustain: hold at sustain_level
        return sustain_level
    t -= sustain_time
    if t < release:
        # Release: ramp sustain_level -> 0
        return sustain_level * (1.0 - t / release) if release > 0.0 else 0.0
    return 0.0


def synthesize(waveform='square', duration=0.2, sample_rate=44100,
               freq_start=440, freq_end=None,
               attack=0.01, decay=0.05, sustain_level=0.7, sustain_time=0.1,
               release=0.05, noise_mix=0.0, lowpass=1.0, volume=0.8,
               vibrato_freq=0, vibrato_depth=0, drive=1.0):
    """Core synthesis function. Returns list of 16-bit integer samples.

    Args:
        waveform: 'square', 'sawtooth', 'sine', 'triangle', or 'noise'
        duration: total duration in seconds (overridden by ADSR sum if shorter)
        sample_rate: samples per second (44100)
        freq_start: starting frequency in Hz
        freq_end: ending frequency in Hz (None = same as start)
        attack/decay/sustain_level/sustain_time/release: ADSR envelope params
        noise_mix: 0..1 blend of white noise into the signal
        lowpass: 0..1 IIR low-pass filter coefficient (1.0 = no filtering)
        volume: overall gain 0..1
        vibrato_freq: vibrato LFO frequency in Hz
        vibrato_depth: vibrato depth in Hz (frequency modulation amount)
    """
    if freq_end is None:
        freq_end = freq_start

    wave_fn = _WAVEFORMS.get(waveform, _waveform_square)

    # Total duration is at least the ADSR envelope length
    env_duration = attack + decay + sustain_time + release
    total_duration = max(duration, env_duration)
    num_samples = int(total_duration * sample_rate)

    samples = []
    phase = 0.0
    prev_filtered = 0.0

    for i in range(num_samples):
        t_norm = i / num_samples  # 0..1 progress through the sound

        # Frequency sweep (linear interpolation)
        freq = freq_start + (freq_end - freq_start) * t_norm

        # Vibrato modulation
        if vibrato_freq > 0 and vibrato_depth > 0:
            vib = vibrato_depth * math.sin(2.0 * math.pi * vibrato_freq * i / sample_rate)
            freq += vib

        # Advance phase
        phase += freq / sample_rate

        # Generate waveform sample
        samp = wave_fn(phase)

        # Mix in white noise
        if noise_mix > 0.0:
            samp = samp * (1.0 - noise_mix) + random.uniform(-1.0, 1.0) * noise_mix

        # ADSR envelope
        env = _adsr_envelope(i, sample_rate, attack, decay, sustain_level,
                             sustain_time, release)
        samp *= env

        # Soft-clipping distortion (tanh waveshaping) — Doom-style overdrive
        if drive > 1.0:
            samp = math.tanh(samp * drive) / math.tanh(drive)

        # Low-pass filter (simple single-pole IIR)
        if lowpass < 1.0:
            alpha = max(0.001, lowpass)
            samp = alpha * samp + (1.0 - alpha) * prev_filtered
            prev_filtered = samp
        else:
            prev_filtered = samp

        # Volume
        samp *= volume

        # Clamp and convert to 16-bit
        samp = max(-1.0, min(1.0, samp))
        samples.append(int(samp * 32767))

    return samples


def synthesize_chain(*args):
    """Concatenate multiple synthesize() results into one sample list.

    Each layer is a dict of kwargs to pass to synthesize().
    Accepts either a single list of dicts or dicts as positional args.
    """
    # Support both synthesize_chain([d1, d2]) and synthesize_chain(d1, d2)
    if len(args) == 1 and isinstance(args[0], list):
        layers = args[0]
    else:
        layers = args
    result = []
    for kwargs in layers:
        result.extend(synthesize(**kwargs))
    return result


def synthesize_mix(layers, master_volume=1.0):
    """Mix multiple layers together (additive), all starting at t=0.

    Each layer is a dict of kwargs to pass to synthesize().
    Returns combined 16-bit sample list.
    """
    rendered = [synthesize(**kw) for kw in layers]
    max_len = max(len(r) for r in rendered)
    mixed = [0.0] * max_len
    for r in rendered:
        for i, s in enumerate(r):
            mixed[i] += s / 32767.0
    # Normalize and convert
    peak = max(abs(s) for s in mixed) if mixed else 1.0
    if peak < 0.001:
        peak = 1.0
    scale = master_volume / peak
    return [int(max(-1.0, min(1.0, s * scale)) * 32767) for s in mixed]


# ---------------------------------------------------------------------------
# WAV writer
# ---------------------------------------------------------------------------

def write_wav(path, samples, sample_rate=44100):
    """Write 16-bit mono WAV file."""
    os.makedirs(os.path.dirname(path) or '.', exist_ok=True)
    with wave.open(path, 'w') as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(sample_rate)
        raw = struct.pack('<' + 'h' * len(samples), *samples)
        wf.writeframes(raw)


# ---------------------------------------------------------------------------
# Sound presets — weapons
# ---------------------------------------------------------------------------

def sfx_weapon_sword():
    """Quick sawtooth sweep, punchy melee slash with sine bass layer."""
    return synthesize_mix([
        dict(waveform='noise', duration=0.3, freq_start=400, freq_end=80, attack=0.002, decay=0.08, sustain_level=0.3, sustain_time=0.05, release=0.15, lowpass=0.15, volume=0.9, drive=2.5),
        dict(waveform='sawtooth', duration=0.25, freq_start=300, freq_end=80, attack=0.001, decay=0.05, sustain_level=0.2, sustain_time=0.03, release=0.15, noise_mix=0.4, lowpass=0.2, volume=0.5, drive=2.0),
        dict(waveform='sine', duration=0.3, freq_start=60, freq_end=40, attack=0.005, decay=0.1, sustain_level=0.4, sustain_time=0.1, release=0.1, volume=0.4),
    ])


def sfx_weapon_dagger():
    """Very short stab, high-pitched and sharp."""
    return synthesize_mix([
        dict(waveform='noise', duration=0.15, freq_start=600, freq_end=100, attack=0.001, decay=0.04, sustain_level=0.2, sustain_time=0.02, release=0.08, lowpass=0.2, volume=0.85, drive=3.0),
        dict(waveform='sawtooth', duration=0.12, freq_start=500, freq_end=150, attack=0.001, decay=0.03, sustain_level=0.15, sustain_time=0.01, release=0.07, noise_mix=0.5, lowpass=0.25, volume=0.4, drive=2.0),
        dict(waveform='sine', duration=0.1, freq_start=80, freq_end=40, attack=0.002, decay=0.03, sustain_level=0.3, sustain_time=0.02, release=0.04, volume=0.35),
    ])


def sfx_weapon_axe():
    """Heavy chop, low and crunchy."""
    return synthesize_mix([
        dict(waveform='noise', duration=0.35, freq_start=300, freq_end=50, attack=0.002, decay=0.1, sustain_level=0.3, sustain_time=0.08, release=0.15, lowpass=0.12, volume=0.95, drive=3.0),
        dict(waveform='sawtooth', duration=0.3, freq_start=200, freq_end=40, attack=0.003, decay=0.08, sustain_level=0.25, sustain_time=0.06, release=0.12, noise_mix=0.5, lowpass=0.15, volume=0.5, drive=2.5),
        dict(waveform='sine', duration=0.35, freq_start=50, freq_end=30, attack=0.005, decay=0.1, sustain_level=0.5, sustain_time=0.1, release=0.1, volume=0.5),
    ])


def sfx_weapon_claymore():
    """Big heavy sweep, deep and meaty."""
    return synthesize_mix([
        dict(waveform='noise', duration=0.45, freq_start=350, freq_end=40, attack=0.005, decay=0.12, sustain_level=0.3, sustain_time=0.1, release=0.2, lowpass=0.1, volume=0.95, drive=3.5),
        dict(waveform='sawtooth', duration=0.4, freq_start=250, freq_end=50, attack=0.005, decay=0.1, sustain_level=0.2, sustain_time=0.08, release=0.18, noise_mix=0.45, lowpass=0.12, volume=0.5, drive=2.5),
        dict(waveform='sine', duration=0.4, freq_start=45, freq_end=25, attack=0.01, decay=0.12, sustain_level=0.5, sustain_time=0.12, release=0.12, volume=0.5),
    ])


def sfx_weapon_pistol():
    """Sharp crack, short and snappy."""
    return synthesize_mix([
        dict(waveform='noise', duration=0.25, freq_start=800, freq_end=100, attack=0.001, decay=0.03, sustain_level=0.2, sustain_time=0.02, release=0.18, lowpass=0.2, volume=0.9, drive=3.5),
        dict(waveform='sawtooth', duration=0.2, freq_start=400, freq_end=80, attack=0.001, decay=0.04, sustain_level=0.15, sustain_time=0.02, release=0.13, noise_mix=0.7, lowpass=0.15, volume=0.5, drive=3.0),
        dict(waveform='sine', duration=0.2, freq_start=80, freq_end=40, attack=0.002, decay=0.05, sustain_level=0.4, sustain_time=0.05, release=0.1, volume=0.5),
    ])


def sfx_weapon_smg():
    """Rapid pop, very short burst."""
    return synthesize_mix([
        dict(waveform='noise', duration=0.15, freq_start=700, freq_end=100, attack=0.001, decay=0.02, sustain_level=0.15, sustain_time=0.01, release=0.1, lowpass=0.2, volume=0.85, drive=3.0),
        dict(waveform='sawtooth', duration=0.12, freq_start=350, freq_end=80, attack=0.001, decay=0.02, sustain_level=0.1, sustain_time=0.01, release=0.08, noise_mix=0.7, lowpass=0.15, volume=0.4, drive=2.5),
        dict(waveform='sine', duration=0.1, freq_start=70, freq_end=35, attack=0.001, decay=0.03, sustain_level=0.3, sustain_time=0.02, release=0.05, volume=0.45),
    ])


def sfx_weapon_carbine():
    """Bigger crack with more body, bass sine layer for depth."""
    return synthesize_mix([
        dict(waveform='noise', duration=0.35, freq_start=600, freq_end=60, attack=0.001, decay=0.04, sustain_level=0.2, sustain_time=0.03, release=0.25, lowpass=0.15, volume=0.95, drive=4.0),
        dict(waveform='sawtooth', duration=0.3, freq_start=300, freq_end=60, attack=0.001, decay=0.05, sustain_level=0.15, sustain_time=0.03, release=0.2, noise_mix=0.75, lowpass=0.12, volume=0.5, drive=3.0),
        dict(waveform='sine', duration=0.3, freq_start=60, freq_end=30, attack=0.003, decay=0.08, sustain_level=0.5, sustain_time=0.08, release=0.12, volume=0.55),
    ])


def sfx_weapon_revolver():
    """Heavy bang, lots of noise, muffled low-pass."""
    return synthesize_mix([
        dict(waveform='noise', duration=0.3, freq_start=900, freq_end=80, attack=0.001, decay=0.04, sustain_level=0.2, sustain_time=0.03, release=0.2, lowpass=0.2, volume=0.95, drive=4.0),
        dict(waveform='sawtooth', duration=0.25, freq_start=450, freq_end=60, attack=0.001, decay=0.04, sustain_level=0.15, sustain_time=0.02, release=0.18, noise_mix=0.8, lowpass=0.12, volume=0.5, drive=3.0),
        dict(waveform='sine', duration=0.25, freq_start=70, freq_end=30, attack=0.002, decay=0.06, sustain_level=0.5, sustain_time=0.06, release=0.12, volume=0.55),
    ])


def sfx_weapon_bow():
    """Twangy string release with vibrato."""
    return synthesize_mix([
        dict(waveform='sine', duration=0.3, freq_start=400, freq_end=120, attack=0.001, decay=0.06, sustain_level=0.3, sustain_time=0.08, release=0.15, vibrato_freq=8, vibrato_depth=20, volume=0.7),
        dict(waveform='noise', duration=0.2, freq_start=300, freq_end=80, attack=0.001, decay=0.04, sustain_level=0.15, sustain_time=0.02, release=0.12, lowpass=0.15, volume=0.4, drive=2.0),
        dict(waveform='sine', duration=0.25, freq_start=80, freq_end=50, attack=0.003, decay=0.05, sustain_level=0.3, sustain_time=0.05, release=0.12, volume=0.35),
    ])


def sfx_weapon_crossbow():
    """Mechanical thwack, square punch."""
    return synthesize_mix([
        dict(waveform='noise', duration=0.25, freq_start=500, freq_end=60, attack=0.001, decay=0.03, sustain_level=0.2, sustain_time=0.02, release=0.18, lowpass=0.15, volume=0.85, drive=2.5),
        dict(waveform='sawtooth', duration=0.2, freq_start=250, freq_end=60, attack=0.002, decay=0.04, sustain_level=0.2, sustain_time=0.03, release=0.12, noise_mix=0.5, lowpass=0.2, volume=0.45, drive=2.0),
        dict(waveform='sine', duration=0.2, freq_start=70, freq_end=35, attack=0.003, decay=0.05, sustain_level=0.3, sustain_time=0.04, release=0.08, volume=0.4),
    ])


def sfx_weapon_throw():
    """Whoosh — noise sweep through low-pass."""
    return synthesize_mix([
        dict(waveform='noise', duration=0.3, freq_start=500, freq_end=80, attack=0.002, decay=0.06, sustain_level=0.25, sustain_time=0.08, release=0.14, lowpass=0.2, volume=0.75, drive=1.5),
        dict(waveform='sine', duration=0.25, freq_start=200, freq_end=60, attack=0.005, decay=0.05, sustain_level=0.2, sustain_time=0.05, release=0.1, volume=0.3),
    ])


def sfx_weapon_molotov():
    """Glass break (noise burst) followed by fire whoosh."""
    return synthesize_chain([
        dict(waveform='noise', duration=0.12, freq_start=1200, freq_end=300, attack=0.001, decay=0.03, sustain_level=0.3, sustain_time=0.02, release=0.06, lowpass=0.25, volume=0.8, drive=3.0),
        dict(waveform='noise', duration=0.3, freq_start=400, freq_end=80, attack=0.005, decay=0.08, sustain_level=0.3, sustain_time=0.1, release=0.1, lowpass=0.12, volume=0.7, drive=2.0),
    ])


def sfx_weapon_wand():
    """Magic zap — sine with vibrato, airy."""
    return synthesize_mix([
        dict(waveform='sine', duration=0.3, freq_start=600, freq_end=200, attack=0.003, decay=0.06, sustain_level=0.3, sustain_time=0.08, release=0.12, vibrato_freq=6, vibrato_depth=20, volume=0.65),
        dict(waveform='noise', duration=0.25, freq_start=400, freq_end=100, attack=0.005, decay=0.05, sustain_level=0.2, sustain_time=0.05, release=0.1, lowpass=0.15, volume=0.35, drive=2.0),
        dict(waveform='sine', duration=0.25, freq_start=80, freq_end=50, attack=0.005, decay=0.06, sustain_level=0.3, sustain_time=0.06, release=0.08, volume=0.3),
    ])


def sfx_weapon_staff():
    """Deeper magic pulse, resonant sine."""
    return synthesize_mix([
        dict(waveform='sine', duration=0.4, freq_start=400, freq_end=100, attack=0.008, decay=0.08, sustain_level=0.35, sustain_time=0.12, release=0.15, vibrato_freq=4, vibrato_depth=15, volume=0.65),
        dict(waveform='noise', duration=0.35, freq_start=300, freq_end=60, attack=0.008, decay=0.07, sustain_level=0.25, sustain_time=0.08, release=0.12, lowpass=0.12, volume=0.4, drive=2.0),
        dict(waveform='sine', duration=0.35, freq_start=60, freq_end=30, attack=0.01, decay=0.1, sustain_level=0.4, sustain_time=0.1, release=0.1, volume=0.4),
    ])


def sfx_reload():
    """Mechanical click-clack — two short square pops with a gap."""
    return synthesize_chain([
        dict(waveform='noise', duration=0.08, freq_start=500, freq_end=150, attack=0.001, decay=0.02, sustain_level=0.2, sustain_time=0.01, release=0.04, lowpass=0.2, volume=0.8, drive=2.5),
        dict(waveform='noise', duration=0.04, freq_start=100, freq_end=100, attack=0.001, decay=0.001, sustain_level=0.0, sustain_time=0.035, release=0.001, volume=0.0),
        dict(waveform='noise', duration=0.08, freq_start=400, freq_end=100, attack=0.001, decay=0.02, sustain_level=0.2, sustain_time=0.01, release=0.04, lowpass=0.15, volume=0.7, drive=2.0),
    ])


# ---------------------------------------------------------------------------
# Sound presets — combat
# ---------------------------------------------------------------------------

def sfx_hit_melee():
    """Thud — noise burst, heavily filtered."""
    return synthesize_mix([
        dict(waveform='noise', duration=0.2, freq_start=300, freq_end=50, attack=0.001, decay=0.03, sustain_level=0.3, sustain_time=0.03, release=0.12, lowpass=0.15, volume=0.9, drive=2.5),
        dict(waveform='sine', duration=0.2, freq_start=60, freq_end=30, attack=0.002, decay=0.05, sustain_level=0.4, sustain_time=0.05, release=0.1, volume=0.45),
    ])


def sfx_hit_hitscan():
    """Sharp impact — noise snap plus square ping."""
    return synthesize_mix([
        dict(waveform='noise', duration=0.2, freq_start=800, freq_end=80, attack=0.001, decay=0.02, sustain_level=0.15, sustain_time=0.01, release=0.15, lowpass=0.2, volume=0.85, drive=3.5),
        dict(waveform='sawtooth', duration=0.15, freq_start=400, freq_end=100, attack=0.001, decay=0.03, sustain_level=0.1, sustain_time=0.01, release=0.1, noise_mix=0.6, lowpass=0.15, volume=0.4, drive=2.0),
        dict(waveform='sine', duration=0.15, freq_start=70, freq_end=35, attack=0.002, decay=0.04, sustain_level=0.3, sustain_time=0.03, release=0.08, volume=0.4),
    ])


def sfx_hit_projectile():
    """Splat — wet noise burst, low-passed."""
    return synthesize_mix([
        dict(waveform='noise', duration=0.2, freq_start=400, freq_end=60, attack=0.001, decay=0.04, sustain_level=0.2, sustain_time=0.03, release=0.12, lowpass=0.15, volume=0.85, drive=2.5),
        dict(waveform='sine', duration=0.2, freq_start=80, freq_end=40, attack=0.002, decay=0.05, sustain_level=0.3, sustain_time=0.04, release=0.1, volume=0.4),
    ])


def sfx_enemy_hit():
    """Muffled thud — dull impact on flesh/bone."""
    return synthesize_mix([
        dict(waveform='noise', duration=0.15, freq_start=350, freq_end=60, attack=0.001, decay=0.03, sustain_level=0.2, sustain_time=0.02, release=0.08, lowpass=0.12, volume=0.8, drive=2.0),
        dict(waveform='sine', duration=0.12, freq_start=60, freq_end=30, attack=0.002, decay=0.03, sustain_level=0.3, sustain_time=0.02, release=0.05, volume=0.35),
    ])


def sfx_enemy_death():
    """Descending groan — sawtooth sweep down."""
    return synthesize_mix([
        dict(waveform='sawtooth', duration=0.6, freq_start=200, freq_end=30, attack=0.005, decay=0.1, sustain_level=0.4, sustain_time=0.2, release=0.25, noise_mix=0.35, lowpass=0.15, volume=0.8, drive=2.5),
        dict(waveform='noise', duration=0.5, freq_start=300, freq_end=40, attack=0.01, decay=0.08, sustain_level=0.2, sustain_time=0.15, release=0.2, lowpass=0.1, volume=0.4, drive=2.0),
        dict(waveform='sine', duration=0.5, freq_start=50, freq_end=20, attack=0.01, decay=0.1, sustain_level=0.5, sustain_time=0.15, release=0.15, volume=0.45),
    ])


def sfx_player_hit():
    """Crunch — noise burst mixed with low sine tone."""
    return synthesize_mix([
        dict(waveform='noise', duration=0.2, freq_start=500, freq_end=80, attack=0.001, decay=0.03, sustain_level=0.25, sustain_time=0.02, release=0.12, lowpass=0.15, volume=0.85, drive=3.0),
        dict(waveform='sine', duration=0.2, freq_start=100, freq_end=40, attack=0.002, decay=0.05, sustain_level=0.4, sustain_time=0.04, release=0.1, lowpass=0.2, volume=0.5),
    ])


def sfx_player_death():
    """Deep descending tone — dramatic sawtooth sweep."""
    return synthesize_mix([
        dict(waveform='sawtooth', duration=0.8, freq_start=180, freq_end=20, attack=0.01, decay=0.12, sustain_level=0.4, sustain_time=0.35, release=0.3, noise_mix=0.3, lowpass=0.12, volume=0.9, drive=2.5),
        dict(waveform='noise', duration=0.7, freq_start=200, freq_end=30, attack=0.015, decay=0.1, sustain_level=0.2, sustain_time=0.25, release=0.25, lowpass=0.08, volume=0.4, drive=2.0),
        dict(waveform='sine', duration=0.7, freq_start=40, freq_end=15, attack=0.02, decay=0.15, sustain_level=0.5, sustain_time=0.2, release=0.2, volume=0.5),
    ])


# ---------------------------------------------------------------------------
# Sound presets — skills
# ---------------------------------------------------------------------------

def sfx_skill_fire():
    """Crackle — noise with descending sine, fiery vibrato."""
    return synthesize_mix([
        dict(waveform='noise', duration=0.4, freq_start=500, freq_end=100, attack=0.005, decay=0.08, sustain_level=0.35, sustain_time=0.15, release=0.15, lowpass=0.15, volume=0.75, drive=2.5),
        dict(waveform='sawtooth', duration=0.35, freq_start=300, freq_end=80, attack=0.008, decay=0.06, sustain_level=0.2, sustain_time=0.1, release=0.12, noise_mix=0.5, lowpass=0.12, volume=0.4, drive=2.0),
        dict(waveform='sine', duration=0.3, freq_start=80, freq_end=40, attack=0.01, decay=0.08, sustain_level=0.4, sustain_time=0.1, release=0.1, volume=0.35),
    ])


def sfx_skill_ice():
    """Crystalline shimmer — high sine with fast vibrato."""
    return synthesize_mix([
        dict(waveform='sine', duration=0.3, freq_start=1500, freq_end=400, attack=0.002, decay=0.05, sustain_level=0.25, sustain_time=0.05, release=0.18, vibrato_freq=12, vibrato_depth=25, volume=0.6),
        dict(waveform='noise', duration=0.25, freq_start=800, freq_end=100, attack=0.001, decay=0.03, sustain_level=0.15, sustain_time=0.03, release=0.15, lowpass=0.2, volume=0.5, drive=2.0),
        dict(waveform='sine', duration=0.2, freq_start=100, freq_end=50, attack=0.005, decay=0.05, sustain_level=0.3, sustain_time=0.04, release=0.08, volume=0.3),
    ])


def sfx_skill_lightning():
    """Electric zap — fast sawtooth descent with noise."""
    return synthesize_mix([
        dict(waveform='sawtooth', duration=0.25, freq_start=1500, freq_end=100, attack=0.001, decay=0.03, sustain_level=0.2, sustain_time=0.02, release=0.18, noise_mix=0.6, lowpass=0.25, volume=0.85, drive=4.0),
        dict(waveform='noise', duration=0.2, freq_start=600, freq_end=50, attack=0.001, decay=0.02, sustain_level=0.15, sustain_time=0.01, release=0.15, lowpass=0.15, volume=0.5, drive=3.0),
        dict(waveform='sine', duration=0.15, freq_start=80, freq_end=30, attack=0.002, decay=0.04, sustain_level=0.3, sustain_time=0.03, release=0.08, volume=0.4),
    ])


def sfx_skill_blood():
    """Wet squelch — heavily filtered noise."""
    return synthesize_mix([
        dict(waveform='noise', duration=0.25, freq_start=300, freq_end=50, attack=0.001, decay=0.04, sustain_level=0.3, sustain_time=0.05, release=0.15, lowpass=0.1, volume=0.85, drive=2.5),
        dict(waveform='sine', duration=0.2, freq_start=80, freq_end=30, attack=0.003, decay=0.05, sustain_level=0.4, sustain_time=0.05, release=0.08, volume=0.4),
    ])


def sfx_skill_dash():
    """Whoosh — noise sweep with decreasing filter cutoff."""
    # Simulate filter sweep by chaining two segments with different lowpass
    return synthesize_chain([
        dict(waveform='noise', duration=0.15, freq_start=600, freq_end=200, attack=0.002, decay=0.03, sustain_level=0.3, sustain_time=0.04, release=0.07, lowpass=0.25, volume=0.75, drive=1.5),
        dict(waveform='noise', duration=0.15, freq_start=200, freq_end=40, attack=0.002, decay=0.03, sustain_level=0.2, sustain_time=0.03, release=0.07, lowpass=0.08, volume=0.6, drive=1.5),
    ])


def sfx_skill_heal():
    """Ascending chime — bright sine sweep up."""
    return synthesize_mix([
        dict(waveform='sine', duration=0.5, freq_start=300, freq_end=800, attack=0.01, decay=0.08, sustain_level=0.4, sustain_time=0.2, release=0.18, vibrato_freq=5, vibrato_depth=15, volume=0.6),
        dict(waveform='triangle', duration=0.4, freq_start=600, freq_end=1000, attack=0.015, decay=0.06, sustain_level=0.3, sustain_time=0.15, release=0.15, volume=0.35),
        dict(waveform='sine', duration=0.4, freq_start=100, freq_end=200, attack=0.01, decay=0.08, sustain_level=0.3, sustain_time=0.12, release=0.12, volume=0.3),
    ])


def sfx_skill_buff():
    """Ascending tone — triangle wave, sparkly."""
    return synthesize_mix([
        dict(waveform='triangle', duration=0.4, freq_start=250, freq_end=800, attack=0.008, decay=0.06, sustain_level=0.35, sustain_time=0.15, release=0.15, vibrato_freq=6, vibrato_depth=20, volume=0.6),
        dict(waveform='sine', duration=0.35, freq_start=500, freq_end=1200, attack=0.01, decay=0.05, sustain_level=0.25, sustain_time=0.1, release=0.15, volume=0.3),
        dict(waveform='sine', duration=0.3, freq_start=80, freq_end=150, attack=0.01, decay=0.06, sustain_level=0.3, sustain_time=0.08, release=0.1, volume=0.3),
    ])


def sfx_skill_summon():
    """Warble — sine with heavy vibrato, magical."""
    return synthesize_mix([
        dict(waveform='sine', duration=0.6, freq_start=200, freq_end=500, attack=0.02, decay=0.1, sustain_level=0.35, sustain_time=0.25, release=0.2, vibrato_freq=6, vibrato_depth=25, volume=0.6),
        dict(waveform='noise', duration=0.5, freq_start=300, freq_end=80, attack=0.025, decay=0.08, sustain_level=0.2, sustain_time=0.2, release=0.15, lowpass=0.1, volume=0.35, drive=2.0),
        dict(waveform='sine', duration=0.5, freq_start=50, freq_end=80, attack=0.02, decay=0.1, sustain_level=0.4, sustain_time=0.15, release=0.15, volume=0.35),
    ])


def sfx_skill_explosion():
    """Boom — big noise burst, heavy low-pass."""
    return synthesize_mix([
        dict(waveform='noise', duration=0.5, freq_start=400, freq_end=20, attack=0.001, decay=0.05, sustain_level=0.35, sustain_time=0.15, release=0.28, lowpass=0.08, volume=0.95, drive=4.0),
        dict(waveform='sawtooth', duration=0.4, freq_start=200, freq_end=20, attack=0.002, decay=0.06, sustain_level=0.2, sustain_time=0.1, release=0.22, noise_mix=0.6, lowpass=0.08, volume=0.5, drive=3.0),
        dict(waveform='sine', duration=0.5, freq_start=40, freq_end=15, attack=0.005, decay=0.1, sustain_level=0.6, sustain_time=0.15, release=0.2, volume=0.6),
    ])


def sfx_skill_stun():
    """Dull thud then ringing sine — two-part stun effect."""
    return synthesize_chain([
        dict(waveform='noise', duration=0.1, freq_start=400, freq_end=60, attack=0.001, decay=0.02, sustain_level=0.25, sustain_time=0.01, release=0.06, lowpass=0.1, volume=0.85, drive=3.0),
        dict(waveform='sine', duration=0.35, freq_start=400, freq_end=200, attack=0.005, decay=0.06, sustain_level=0.3, sustain_time=0.12, release=0.15, vibrato_freq=6, vibrato_depth=25, volume=0.5),
    ])


# ---------------------------------------------------------------------------
# Sound presets — items
# ---------------------------------------------------------------------------

def sfx_item_pickup():
    """Coin ding — short ascending sine."""
    return synthesize_mix([
        dict(waveform='sine', duration=0.2, freq_start=800, freq_end=1200, attack=0.002, decay=0.04, sustain_level=0.3, sustain_time=0.05, release=0.1, vibrato_freq=8, vibrato_depth=20, volume=0.65),
        dict(waveform='sine', duration=0.15, freq_start=80, freq_end=60, attack=0.003, decay=0.03, sustain_level=0.3, sustain_time=0.03, release=0.06, volume=0.3),
    ])


def sfx_item_equip():
    """Metallic clank — square descent with noise."""
    return synthesize_mix([
        dict(waveform='noise', duration=0.15, freq_start=500, freq_end=100, attack=0.001, decay=0.03, sustain_level=0.2, sustain_time=0.02, release=0.08, lowpass=0.2, volume=0.8, drive=2.5),
        dict(waveform='sine', duration=0.12, freq_start=70, freq_end=40, attack=0.002, decay=0.03, sustain_level=0.3, sustain_time=0.02, release=0.05, volume=0.35),
    ])


def sfx_item_drop():
    """Soft thud — filtered noise tap."""
    return synthesize_mix([
        dict(waveform='noise', duration=0.12, freq_start=300, freq_end=60, attack=0.001, decay=0.03, sustain_level=0.2, sustain_time=0.01, release=0.06, lowpass=0.12, volume=0.7, drive=2.0),
        dict(waveform='sine', duration=0.1, freq_start=60, freq_end=30, attack=0.002, decay=0.02, sustain_level=0.3, sustain_time=0.01, release=0.04, volume=0.3),
    ])


def sfx_potion_use():
    """D2-style potion: cork pop, 3 rapid gulps (bubbling sine pulses), breathy exhale."""
    # Cork pop — short noise burst
    cork = synthesize(waveform='noise', duration=0.06, freq_start=1200, freq_end=400,
                      attack=0.001, decay=0.01, sustain_level=0.2, sustain_time=0.01,
                      release=0.03, lowpass=0.3, volume=0.6, drive=2.0)
    # Three rapid gulps — sine pulses sweeping up then down (bubbling)
    gulp1 = synthesize(waveform='sine', duration=0.08, freq_start=250, freq_end=450,
                       attack=0.002, decay=0.015, sustain_level=0.35, sustain_time=0.02,
                       release=0.02, noise_mix=0.2, volume=0.7)
    gulp2 = synthesize(waveform='sine', duration=0.08, freq_start=280, freq_end=500,
                       attack=0.002, decay=0.015, sustain_level=0.3, sustain_time=0.02,
                       release=0.02, noise_mix=0.2, volume=0.65)
    gulp3 = synthesize(waveform='sine', duration=0.1, freq_start=220, freq_end=400,
                       attack=0.002, decay=0.02, sustain_level=0.3, sustain_time=0.025,
                       release=0.03, noise_mix=0.25, volume=0.6)
    # Breathy exhale — filtered noise fading out
    exhale = synthesize(waveform='noise', duration=0.25, freq_start=300, freq_end=80,
                        attack=0.01, decay=0.05, sustain_level=0.15, sustain_time=0.08,
                        release=0.1, lowpass=0.1, volume=0.4)
    return cork + gulp1 + gulp2 + gulp3 + exhale


# ---------------------------------------------------------------------------
# Sound presets — UI
# ---------------------------------------------------------------------------

def sfx_ui_click():
    """Tiny pop — minimal square blip."""
    return synthesize(waveform='square', duration=0.03, freq_start=1000,
                      freq_end=1000, attack=0.001, decay=0.01,
                      sustain_level=0.3, sustain_time=0.005, release=0.015,
                      volume=0.6)


def sfx_ui_back():
    """Descending pip — quick square drop."""
    return synthesize(waveform='square', duration=0.05, freq_start=800,
                      freq_end=400, attack=0.001, decay=0.015,
                      sustain_level=0.3, sustain_time=0.01, release=0.015,
                      volume=0.6)


def sfx_ui_confirm():
    """Ascending pip — quick square rise."""
    return synthesize(waveform='square', duration=0.06, freq_start=600,
                      freq_end=1200, attack=0.001, decay=0.015,
                      sustain_level=0.4, sustain_time=0.015, release=0.03,
                      volume=0.65)


def sfx_menu_hover():
    """Subtle tick — very short square blip."""
    return synthesize(waveform='square', duration=0.02, freq_start=1500,
                      freq_end=1500, attack=0.001, decay=0.005,
                      sustain_level=0.2, sustain_time=0.005, release=0.01,
                      volume=0.45)


# ---------------------------------------------------------------------------
# Sound presets — movement
# ---------------------------------------------------------------------------

def sfx_footstep_stone():
    """Dull tap — filtered noise, stone floor."""
    return synthesize_mix([
        dict(waveform='noise', duration=0.12, freq_start=300, freq_end=60, attack=0.001, decay=0.02, sustain_level=0.2, sustain_time=0.01, release=0.07, lowpass=0.12, volume=0.6, drive=2.0),
        dict(waveform='sine', duration=0.1, freq_start=50, freq_end=30, attack=0.002, decay=0.02, sustain_level=0.3, sustain_time=0.01, release=0.04, volume=0.3),
    ])


def sfx_footstep_metal():
    """Sharper tap — noise plus high square ping, metallic ring."""
    return synthesize_mix([
        dict(waveform='noise', duration=0.1, freq_start=400, freq_end=80, attack=0.001, decay=0.02, sustain_level=0.2, sustain_time=0.01, release=0.05, lowpass=0.15, volume=0.6, drive=2.0),
        dict(waveform='sine', duration=0.08, freq_start=800, freq_end=400, attack=0.001, decay=0.02, sustain_level=0.15, sustain_time=0.01, release=0.04, lowpass=0.3, volume=0.3),
    ])


# ---------------------------------------------------------------------------
# Sound presets — enemies
# ---------------------------------------------------------------------------

def sfx_enemy_footstep():
    """Heavier thud — deeper filtered noise."""
    return synthesize_mix([
        dict(waveform='noise', duration=0.15, freq_start=250, freq_end=40, attack=0.001, decay=0.03, sustain_level=0.25, sustain_time=0.02, release=0.08, lowpass=0.1, volume=0.7, drive=2.0),
        dict(waveform='sine', duration=0.12, freq_start=50, freq_end=25, attack=0.002, decay=0.03, sustain_level=0.4, sustain_time=0.02, release=0.05, volume=0.4),
    ])


def sfx_enemy_attack():
    """Grunt/swipe — descending sawtooth, short and aggressive."""
    return synthesize_mix([
        dict(waveform='sawtooth', duration=0.2, freq_start=300, freq_end=60, attack=0.003, decay=0.04, sustain_level=0.3, sustain_time=0.04, release=0.1, noise_mix=0.35, lowpass=0.2, volume=0.8, drive=2.5),
        dict(waveform='sine', duration=0.15, freq_start=60, freq_end=30, attack=0.005, decay=0.04, sustain_level=0.3, sustain_time=0.03, release=0.06, volume=0.35),
    ])


def sfx_boss_roar():
    """Deep roar — long descending sawtooth with noise."""
    return synthesize_mix([
        dict(waveform='sawtooth', duration=0.8, freq_start=200, freq_end=30, attack=0.02, decay=0.12, sustain_level=0.45, sustain_time=0.35, release=0.28, noise_mix=0.4, lowpass=0.15, volume=0.9, drive=3.0),
        dict(waveform='noise', duration=0.7, freq_start=300, freq_end=40, attack=0.025, decay=0.1, sustain_level=0.2, sustain_time=0.25, release=0.2, lowpass=0.08, volume=0.4, drive=2.0),
        dict(waveform='sine', duration=0.7, freq_start=40, freq_end=15, attack=0.03, decay=0.15, sustain_level=0.5, sustain_time=0.2, release=0.2, volume=0.5),
    ])


def sfx_boss_stomp():
    """Heavy impact — noise thud plus low sine rumble."""
    return synthesize_mix([
        dict(waveform='noise', duration=0.35, freq_start=300, freq_end=20, attack=0.001, decay=0.04, sustain_level=0.3, sustain_time=0.05, release=0.25, lowpass=0.08, volume=0.95, drive=4.0),
        dict(waveform='sine', duration=0.4, freq_start=50, freq_end=15, attack=0.003, decay=0.08, sustain_level=0.6, sustain_time=0.12, release=0.18, volume=0.6),
        dict(waveform='noise', duration=0.3, freq_start=150, freq_end=30, attack=0.005, decay=0.06, sustain_level=0.2, sustain_time=0.08, release=0.15, lowpass=0.06, volume=0.35, drive=2.5),
    ])


# ---------------------------------------------------------------------------
# Sound presets — environment
# ---------------------------------------------------------------------------

def sfx_door_open():
    """Creaking door — sawtooth with vibrato, up-down sweep."""
    return synthesize_chain([
        dict(waveform='sawtooth', duration=0.25, freq_start=150, freq_end=300, attack=0.01, decay=0.05, sustain_level=0.3, sustain_time=0.08, release=0.1, vibrato_freq=5, vibrato_depth=20, noise_mix=0.2, lowpass=0.2, volume=0.6, drive=1.5),
        dict(waveform='sawtooth', duration=0.25, freq_start=300, freq_end=100, attack=0.005, decay=0.05, sustain_level=0.25, sustain_time=0.08, release=0.1, vibrato_freq=5, vibrato_depth=20, noise_mix=0.2, lowpass=0.2, volume=0.5, drive=1.5),
    ])


def sfx_level_up():
    """Ascending fanfare — sine stepping through notes."""
    # Four ascending notes: ~400, 600, 800, 1200 Hz
    return synthesize_chain(
        dict(waveform='sine', duration=0.1, freq_start=400, freq_end=400,
             attack=0.005, decay=0.02, sustain_level=0.5, sustain_time=0.04,
             release=0.035, volume=0.7),
        dict(waveform='sine', duration=0.1, freq_start=600, freq_end=600,
             attack=0.005, decay=0.02, sustain_level=0.5, sustain_time=0.04,
             release=0.035, volume=0.7),
        dict(waveform='sine', duration=0.1, freq_start=800, freq_end=800,
             attack=0.005, decay=0.02, sustain_level=0.5, sustain_time=0.04,
             release=0.035, volume=0.7),
        dict(waveform='sine', duration=0.2, freq_start=1200, freq_end=1200,
             attack=0.005, decay=0.03, sustain_level=0.6, sustain_time=0.08,
             release=0.085, volume=0.8),
    )


# ---------------------------------------------------------------------------
# Preset registry — maps name -> (default_filename, generator_function)
# ---------------------------------------------------------------------------

SOUND_PRESETS = {
    # Weapons
    'weapon_sword':    ('sfx_weapon_sword.wav',    sfx_weapon_sword),
    'weapon_dagger':   ('sfx_weapon_dagger.wav',   sfx_weapon_dagger),
    'weapon_axe':      ('sfx_weapon_axe.wav',      sfx_weapon_axe),
    'weapon_claymore': ('sfx_weapon_claymore.wav', sfx_weapon_claymore),
    'weapon_pistol':   ('sfx_weapon_pistol.wav',   sfx_weapon_pistol),
    'weapon_smg':      ('sfx_weapon_smg.wav',      sfx_weapon_smg),
    'weapon_carbine':  ('sfx_weapon_carbine.wav',  sfx_weapon_carbine),
    'weapon_revolver': ('sfx_weapon_revolver.wav', sfx_weapon_revolver),
    'weapon_bow':      ('sfx_weapon_bow.wav',      sfx_weapon_bow),
    'weapon_crossbow': ('sfx_weapon_crossbow.wav', sfx_weapon_crossbow),
    'weapon_throw':    ('sfx_weapon_throw.wav',    sfx_weapon_throw),
    'weapon_molotov':  ('sfx_weapon_molotov.wav',  sfx_weapon_molotov),
    'weapon_wand':     ('sfx_weapon_wand.wav',     sfx_weapon_wand),
    'weapon_staff':    ('sfx_weapon_staff.wav',    sfx_weapon_staff),
    'reload':          ('sfx_reload.wav',          sfx_reload),
    # Combat
    'hit_melee':       ('sfx_hit_melee.wav',       sfx_hit_melee),
    'hit_hitscan':     ('sfx_hit_hitscan.wav',     sfx_hit_hitscan),
    'hit_projectile':  ('sfx_hit_projectile.wav',  sfx_hit_projectile),
    'enemy_hit':       ('sfx_enemy_hit.wav',       sfx_enemy_hit),
    'enemy_death':     ('sfx_enemy_death.wav',     sfx_enemy_death),
    'player_hit':      ('sfx_player_hit.wav',      sfx_player_hit),
    'player_death':    ('sfx_player_death.wav',    sfx_player_death),
    # Skills
    'skill_fire':      ('sfx_skill_fire.wav',      sfx_skill_fire),
    'skill_ice':       ('sfx_skill_ice.wav',       sfx_skill_ice),
    'skill_lightning': ('sfx_skill_lightning.wav',  sfx_skill_lightning),
    'skill_blood':     ('sfx_skill_blood.wav',     sfx_skill_blood),
    'skill_dash':      ('sfx_skill_dash.wav',      sfx_skill_dash),
    'skill_heal':      ('sfx_skill_heal.wav',      sfx_skill_heal),
    'skill_buff':      ('sfx_skill_buff.wav',      sfx_skill_buff),
    'skill_summon':    ('sfx_skill_summon.wav',     sfx_skill_summon),
    'skill_explosion': ('sfx_skill_explosion.wav',  sfx_skill_explosion),
    'skill_stun':      ('sfx_skill_stun.wav',      sfx_skill_stun),
    # Items
    'item_pickup':     ('sfx_item_pickup.wav',     sfx_item_pickup),
    'item_equip':      ('sfx_item_equip.wav',      sfx_item_equip),
    'item_drop':       ('sfx_item_drop.wav',       sfx_item_drop),
    'potion_use':      ('sfx_potion_use.wav',      sfx_potion_use),
    # UI
    'ui_click':        ('sfx_ui_click.wav',        sfx_ui_click),
    'ui_back':         ('sfx_ui_back.wav',         sfx_ui_back),
    'ui_confirm':      ('sfx_ui_confirm.wav',      sfx_ui_confirm),
    'menu_hover':      ('sfx_menu_hover.wav',      sfx_menu_hover),
    # Movement
    'footstep_stone':  ('sfx_footstep_stone.wav',  sfx_footstep_stone),
    'footstep_metal':  ('sfx_footstep_metal.wav',  sfx_footstep_metal),
    # Enemies
    'enemy_footstep':  ('sfx_enemy_footstep.wav',  sfx_enemy_footstep),
    'enemy_attack':    ('sfx_enemy_attack.wav',    sfx_enemy_attack),
    'boss_roar':       ('sfx_boss_roar.wav',       sfx_boss_roar),
    'boss_stomp':      ('sfx_boss_stomp.wav',      sfx_boss_stomp),
    # Environment
    'door_open':       ('sfx_door_open.wav',       sfx_door_open),
    'level_up':        ('sfx_level_up.wav',        sfx_level_up),
}


# ---------------------------------------------------------------------------
# Generation entry point
# ---------------------------------------------------------------------------

def generate_sound(sound_type, out_path=None):
    """Generate a single sound effect and write it to a WAV file."""
    if sound_type not in SOUND_PRESETS:
        print(f"Unknown sound type: {sound_type}")
        return False

    default_file, gen_func = SOUND_PRESETS[sound_type]
    if out_path is None:
        game_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        out_path = os.path.join(game_root, "assets", "audio", default_file)

    # Seed per-preset for reproducible output
    random.seed(hash(sound_type) & 0xFFFFFFFF)
    samples = gen_func()
    write_wav(out_path, samples)
    print(f"Wrote {out_path}  ({len(samples)} samples, "
          f"{len(samples)/44100:.3f}s)")
    return True


# ---------------------------------------------------------------------------
# Procedural ambient music — one track per floor tier, progressively darker
# ---------------------------------------------------------------------------

def _apply_fade(samples, fade_in_s=2.0, fade_out_s=2.0, sr=44100):
    """Apply fade-in and fade-out to a sample list for seamless looping."""
    fade_in = int(fade_in_s * sr)
    fade_out = int(fade_out_s * sr)
    for i in range(min(fade_in, len(samples))):
        t = i / fade_in
        samples[i] = int(samples[i] * t)
    for i in range(min(fade_out, len(samples))):
        idx = len(samples) - 1 - i
        t = i / fade_out
        samples[idx] = int(samples[idx] * t)
    return samples


def music_tier1_dungeon():
    """Tier 1 (floors 1-10): Stone Dungeon — eerie, sparse, quiet."""
    samples = synthesize_mix([
        # Low sine drone — the foundation
        dict(waveform='sine', duration=60.0, freq_start=80, freq_end=75,
             attack=3.0, decay=2.0, sustain_level=0.4, sustain_time=50.0, release=5.0,
             volume=0.5, vibrato_freq=0.3, vibrato_depth=2),
        # Filtered wind noise — corridor ambience
        dict(waveform='noise', duration=60.0, freq_start=200, freq_end=150,
             attack=4.0, decay=3.0, sustain_level=0.15, sustain_time=48.0, release=5.0,
             lowpass=0.04, volume=0.3),
        # Very subtle high sine — distant ringing
        dict(waveform='sine', duration=60.0, freq_start=440, freq_end=430,
             attack=5.0, decay=2.0, sustain_level=0.05, sustain_time=48.0, release=5.0,
             volume=0.12, vibrato_freq=0.5, vibrato_depth=3),
    ], master_volume=0.6)
    return _apply_fade(samples)


def music_tier2_catacombs():
    """Tier 2 (floors 11-20): Catacombs — ominous, deeper, slow pulse."""
    samples = synthesize_mix([
        # Deep drone
        dict(waveform='sine', duration=60.0, freq_start=50, freq_end=48,
             attack=3.0, decay=2.0, sustain_level=0.5, sustain_time=50.0, release=5.0,
             volume=0.55, vibrato_freq=0.2, vibrato_depth=1.5),
        # Detuned harmonic — unsettling interval
        dict(waveform='sine', duration=60.0, freq_start=75, freq_end=73,
             attack=4.0, decay=3.0, sustain_level=0.25, sustain_time=48.0, release=5.0,
             volume=0.3, vibrato_freq=0.15, vibrato_depth=2),
        # Slow pulsing sub
        dict(waveform='sine', duration=60.0, freq_start=35, freq_end=35,
             attack=2.0, decay=1.0, sustain_level=0.3, sustain_time=52.0, release=5.0,
             volume=0.35, vibrato_freq=0.08, vibrato_depth=5),
        # Metallic resonance — filtered noise
        dict(waveform='noise', duration=60.0, freq_start=300, freq_end=200,
             attack=5.0, decay=3.0, sustain_level=0.08, sustain_time=47.0, release=5.0,
             lowpass=0.03, volume=0.2),
    ], master_volume=0.6)
    return _apply_fade(samples)


def music_tier3_caverns():
    """Tier 3 (floors 21-30): Caverns — oppressive, heavy sub-bass, dripping."""
    samples = synthesize_mix([
        # Heavy sub-bass drone
        dict(waveform='sine', duration=60.0, freq_start=40, freq_end=38,
             attack=3.0, decay=2.0, sustain_level=0.6, sustain_time=50.0, release=5.0,
             volume=0.6, vibrato_freq=0.1, vibrato_depth=1),
        # Oppressive mid — sawtooth with heavy filtering
        dict(waveform='sawtooth', duration=60.0, freq_start=100, freq_end=90,
             attack=5.0, decay=3.0, sustain_level=0.2, sustain_time=47.0, release=5.0,
             lowpass=0.05, volume=0.3, drive=1.5),
        # Drip texture — noise bursts
        dict(waveform='noise', duration=60.0, freq_start=400, freq_end=100,
             attack=6.0, decay=4.0, sustain_level=0.1, sustain_time=45.0, release=5.0,
             lowpass=0.06, volume=0.2),
        # Rumble
        dict(waveform='noise', duration=60.0, freq_start=60, freq_end=40,
             attack=4.0, decay=3.0, sustain_level=0.15, sustain_time=48.0, release=5.0,
             lowpass=0.02, volume=0.25),
    ], master_volume=0.6)
    return _apply_fade(samples)


def music_tier4_hellforge():
    """Tier 4 (floors 31-40): Hellforge — infernal, aggressive, machinery."""
    samples = synthesize_mix([
        # Aggressive sawtooth drone
        dict(waveform='sawtooth', duration=60.0, freq_start=60, freq_end=55,
             attack=2.0, decay=2.0, sustain_level=0.45, sustain_time=51.0, release=5.0,
             lowpass=0.08, volume=0.5, drive=2.0),
        # Distorted noise crackle — fire ambience
        dict(waveform='noise', duration=60.0, freq_start=500, freq_end=200,
             attack=3.0, decay=2.0, sustain_level=0.2, sustain_time=50.0, release=5.0,
             lowpass=0.1, volume=0.3, drive=3.0),
        # Fast sub-pulse — distant machinery
        dict(waveform='square', duration=60.0, freq_start=30, freq_end=30,
             attack=2.0, decay=1.0, sustain_level=0.3, sustain_time=52.0, release=5.0,
             volume=0.3, vibrato_freq=2.0, vibrato_depth=8),
        # Mid-range menace
        dict(waveform='sawtooth', duration=60.0, freq_start=150, freq_end=130,
             attack=5.0, decay=3.0, sustain_level=0.15, sustain_time=47.0, release=5.0,
             lowpass=0.06, volume=0.25, drive=1.8),
    ], master_volume=0.6)
    return _apply_fade(samples)


def music_tier5_void():
    """Tier 5 (floors 41-50): Void — alien, dissonant, ultra-deep, unsettling."""
    samples = synthesize_mix([
        # Ultra-low sine — at the edge of hearing
        dict(waveform='sine', duration=60.0, freq_start=30, freq_end=28,
             attack=4.0, decay=3.0, sustain_level=0.5, sustain_time=48.0, release=5.0,
             volume=0.55, vibrato_freq=0.05, vibrato_depth=1),
        # Dissonant tritone interval — deeply unsettling
        dict(waveform='sine', duration=60.0, freq_start=42, freq_end=40,
             attack=5.0, decay=3.0, sustain_level=0.3, sustain_time=47.0, release=5.0,
             volume=0.35, vibrato_freq=0.07, vibrato_depth=1.5),
        # Alien modulation — sine with heavy vibrato
        dict(waveform='sine', duration=60.0, freq_start=200, freq_end=180,
             attack=6.0, decay=4.0, sustain_level=0.1, sustain_time=45.0, release=5.0,
             volume=0.2, vibrato_freq=3.0, vibrato_depth=40),
        # Void hum — filtered square
        dict(waveform='square', duration=60.0, freq_start=55, freq_end=50,
             attack=4.0, decay=3.0, sustain_level=0.2, sustain_time=48.0, release=5.0,
             lowpass=0.03, volume=0.3),
        # Distant whispers — barely-there noise
        dict(waveform='noise', duration=60.0, freq_start=800, freq_end=400,
             attack=8.0, decay=5.0, sustain_level=0.05, sustain_time=42.0, release=5.0,
             lowpass=0.02, volume=0.15),
    ], master_volume=0.6)
    return _apply_fade(samples)


MUSIC_PRESETS = {
    'tier1': ('music_tier1.wav', music_tier1_dungeon),  # .wav generated then compressed to .ogg
    'tier2': ('music_tier2.wav', music_tier2_catacombs),
    'tier3': ('music_tier3.wav', music_tier3_caverns),
    'tier4': ('music_tier4.wav', music_tier4_hellforge),
    'tier5': ('music_tier5.wav', music_tier5_void),
}


def generate_music(music_type, out_path=None):
    """Generate a music track."""
    if music_type not in MUSIC_PRESETS:
        print(f"Unknown music type: {music_type}")
        return False
    default_file, gen_func = MUSIC_PRESETS[music_type]
    if out_path is None:
        game_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        out_path = os.path.join(game_root, "assets", "audio", default_file)
    random.seed(hash(music_type) & 0xFFFFFFFF)
    print(f"Generating {music_type} (~60s, this takes a moment)...")
    samples = gen_func()
    write_wav(out_path, samples)
    print(f"Wrote {out_path}  ({len(samples)} samples, {len(samples)/44100:.1f}s)")

    # Compress to OGG Vorbis for smaller binary size (~10× reduction)
    ogg_path = out_path.replace(".wav", ".ogg")
    try:
        import subprocess
        result = subprocess.run(
            ["ffmpeg", "-y", "-i", out_path, "-c:a", "libvorbis", "-q:a", "4", ogg_path],
            capture_output=True, text=True)
        if result.returncode == 0:
            wav_size = os.path.getsize(out_path) / (1024 * 1024)
            ogg_size = os.path.getsize(ogg_path) / (1024 * 1024)
            os.remove(out_path)  # remove WAV, keep only OGG
            print(f"Compressed to OGG: {wav_size:.1f}MB -> {ogg_size:.1f}MB")
        else:
            print(f"ffmpeg not available, keeping WAV ({out_path})")
    except FileNotFoundError:
        print(f"ffmpeg not found, keeping WAV ({out_path})")

    return True


def main():
    parser = argparse.ArgumentParser(
        description="Procedural sound effect generator (sfxr-style synthesis).")
    parser.add_argument("--type", choices=list(SOUND_PRESETS.keys()),
                        help="Generate a single sound preset")
    parser.add_argument("--out", type=str, default=None,
                        help="Custom output WAV path")
    parser.add_argument("--all", action="store_true",
                        help="Generate all sound presets")
    parser.add_argument("--list", action="store_true",
                        help="List all available presets")
    parser.add_argument("--music", choices=list(MUSIC_PRESETS.keys()),
                        help="Generate a single music track")
    parser.add_argument("--music-all", action="store_true",
                        help="Generate all music tracks")
    args = parser.parse_args()

    if args.list:
        print(f"Available presets ({len(SOUND_PRESETS)}):")
        for name, (fname, _) in SOUND_PRESETS.items():
            print(f"  {name:20s} -> {fname}")
        print(f"\nMusic tracks ({len(MUSIC_PRESETS)}):")
        for name, (fname, _) in MUSIC_PRESETS.items():
            print(f"  {name:20s} -> {fname}")
        return

    if args.music_all:
        print(f"Generating {len(MUSIC_PRESETS)} music tracks...")
        for name in MUSIC_PRESETS:
            generate_music(name)
        print("Done.")
        return

    if args.music:
        generate_music(args.music, args.out)
        return

    if args.all:
        manifest_slots = _load_manifest_slots()  # hand-picked slots owned by pick_sfx.py
        print(f"Generating {len(SOUND_PRESETS)} sound effects...")
        for name in SOUND_PRESETS:
            # Don't clobber hand-picks on a bulk regen (explicit --type still generates them).
            if ("sfx_" + name) in manifest_slots:
                print(f"  skipping {name} (hand-picked — owned by pick_sfx.py)")
                continue
            generate_sound(name)
        print("Done.")
        return

    if not args.type:
        parser.error("--type is required (or use --all / --list / --music / --music-all)")

    generate_sound(args.type, args.out)


if __name__ == "__main__":
    main()
