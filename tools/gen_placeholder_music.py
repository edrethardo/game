#!/usr/bin/env python3
"""gen_placeholder_music.py — a LONG provisional music bed for the trailer cuts (WB-273).

THIS SCRIPT IS THE SAVED SCORE (user, 2026-08-23): the RNG seed is FIXED (0xC4A6), so a
re-run reproduces the shipped bed BIT-IDENTICALLY (verified by md5 against
game_takes/PLACEHOLDER_music_bed.wav — e366a18cc61866b1333154a408905ca8). Do not change
the seed, the BPM, or the section layout without minting a NEW filename: the trailers'
credits name this exact piece ("CLAUDE — ORIGINAL SCORE"), and game_takes/SCORE.md
documents its provenance. Pure synthesis (numpy sine/noise) — no samples, no external
work, no CC author.

PLACEHOLDER by design: the real track is the user's pick via the music picker (WB-274);
this exists so the cuts can be timed against something with a pulse today. Synthesized from
scratch (sine/noise only), so there is no licence question — it is ours by construction.

Shape: ~120 s dark hybrid bed at 112 BPM — a low drone, a four-on-the-floor kick with a
sidechain-style dip, sparse hats, an eight-bar riser, and a hard stop at the end for the
title card. Deliberately monotonous: filler must sit UNDER footage, not compete with it.
"""
import numpy as np, struct, sys

SR = 44100
BPM = 112.0
BEAT = 60.0 / BPM
BARS = 56                      # 56 bars * 4 beats * ~0.536 s = ~120 s
N = int(BARS * 4 * BEAT * SR)
t = np.arange(N) / SR

rng = np.random.default_rng(0xC4A6)

# --- Drone: detuned saw-ish stack on A1 (55 Hz), slow filter wobble ---
def saw(f, tt): return 2.0 * ((f * tt) % 1.0) - 1.0
drone = 0.20 * saw(55.0, t) + 0.16 * saw(55.5, t) + 0.10 * saw(110.2, t)
wob = 0.5 + 0.5 * np.sin(2 * np.pi * t / 16.0)          # 16 s swell
# One-pole lowpass, cutoff moving 200..900 Hz with the wobble
out = np.zeros_like(drone); y = 0.0
alpha = np.clip((200 + 700 * wob) / SR * 2 * np.pi, 0.001, 0.5)
for i in range(N):
    y += alpha[i] * (drone[i] - y); out[i] = y
drone = out

# --- Kick: 4-on-floor, pitch-dropping sine burst ---
kick = np.zeros(N)
kt = np.arange(int(0.28 * SR)) / SR
kenv = np.exp(-kt * 18.0)
kwave = np.sin(2 * np.pi * (120 * np.exp(-kt * 9.0) + 40) * kt) * kenv
for b in range(BARS * 4):
    i0 = int(b * BEAT * SR)
    if b >= 8:                                           # 2 intro bars drone-only
        kick[i0:i0 + len(kwave)] += kwave[: max(0, min(len(kwave), N - i0))]

# --- Hats: offbeat noise ticks from bar 9 ---
hat = np.zeros(N)
ht = np.arange(int(0.05 * SR)) / SR
hwave = rng.standard_normal(len(ht)) * np.exp(-ht * 90.0) * 0.14
for b in range(BARS * 4):
    i0 = int((b + 0.5) * BEAT * SR)
    if b >= 32 and i0 + len(hwave) < N:
        hat[i0:i0 + len(hwave)] += hwave

# --- Riser: filtered-noise swell every 8 bars, released on the downbeat ---
riser = np.zeros(N)
for bar in range(6, BARS, 8):
    i0, i1 = int(bar * 4 * BEAT * SR), int((bar + 2) * 4 * BEAT * SR)
    if i1 >= N: break
    seg = rng.standard_normal(i1 - i0)
    env = np.linspace(0, 1, i1 - i0) ** 2 * 0.22
    riser[i0:i1] += seg * env

# --- Sidechain dip on every kick so the drone pumps ---
duck = np.ones(N)
for b in range(8, BARS * 4):
    i0 = int(b * BEAT * SR); i1 = min(N, i0 + int(0.22 * SR))
    duck[i0:i1] *= np.linspace(0.45, 1.0, i1 - i0)

mix = (drone * duck) + kick * 0.9 + hat + riser
# Hard stop 1.5 s before the end (the title card's silence), tiny fade to zero.
stop = N - int(1.5 * SR)
mix[stop:] = 0.0
mix[:int(0.5 * SR)] *= np.linspace(0, 1, int(0.5 * SR))
mix /= max(1e-9, np.max(np.abs(mix))); mix *= 0.85     # -1.4 dBFS headroom

pcm = (mix * 32767).astype(np.int16)
stereo = np.repeat(pcm, 2)                              # dual mono
out = sys.argv[1] if len(sys.argv) > 1 else "placeholder_bed.wav"
with open(out, "wb") as f:
    data = stereo.tobytes()
    f.write(b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE")
    f.write(b"fmt " + struct.pack("<IHHIIHH", 16, 1, 2, SR, SR * 4, 4, 16))
    f.write(b"data" + struct.pack("<I", len(data)) + data)
print(f"wrote {out}: {N/SR:.1f} s at {BPM:.0f} BPM (PLACEHOLDER — user picks the real track)")
