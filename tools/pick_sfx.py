#!/usr/bin/env python3
"""Interactive weapon/reload SFX picker for DungeonEngine.

The game's weapon and reload sounds are auto-selected by fetch_audio.py (keyword
matching over ~9 CC0 packs + a fixed pitch shift). This tool adds a human-in-the-
loop pass: for each of the 15 weapon/reload slots it ranks the best CC0 candidates,
serves a small local web app so you can audition them and dial in effects
(pitch / stretch / gain / reverb / lowpass / trim) with live preview, then writes
the polished result into assets/audio/ at the exact format the engine loads
(44100 Hz, 16-bit, mono WAV).

Choices persist to a git-tracked manifest (tools/sound_selection.json) so they
survive the (gitignored) assets/audio/ regeneration and can be re-rendered
headlessly with --apply.

It REUSES fetch_audio.py by import (download/scan/normalize helpers, keyword seeds,
current-pick overrides) rather than duplicating that logic, and ffmpeg does all the
DSP (numpy/scipy are used only for candidate ranking features).

Usage:
    python3 tools/pick_sfx.py                      # rank + serve UI at http://localhost:8765
    python3 tools/pick_sfx.py --slot sfx_weapon_pistol
    python3 tools/pick_sfx.py --apply              # headless: re-render every manifest slot
    python3 tools/pick_sfx.py --list-slots         # print slots + saved state
"""

import argparse
import io
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import wave
from collections import OrderedDict
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

# numpy/scipy are needed ONLY for candidate ranking (the interactive server). The
# headless --apply path renders through ffmpeg alone, so we import them lazily via
# _load_dsp() and keep --apply dependency-free — that's what lets CI run --apply with
# just ffmpeg installed (matching fetch_audio.py's stdlib-only footprint).
np = None
find_peaks = None


def _load_dsp():
    """Import numpy + scipy on first ranking use. Raises ImportError if unavailable."""
    global np, find_peaks
    if np is None:
        import numpy as _np
        from scipy.signal import find_peaks as _find_peaks
        np = _np
        find_peaks = _find_peaks


# fetch_audio lives beside us; import it for its proven pack/normalize helpers and
# keyword/override tables. Importing is side-effect-free (its main() is __main__-guarded).
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
GAME_ROOT = os.path.dirname(SCRIPT_DIR)
if SCRIPT_DIR not in sys.path:
    sys.path.insert(0, SCRIPT_DIR)
import fetch_audio  # noqa: E402

OUTPUT_DIR = os.path.join(GAME_ROOT, "assets", "audio")
MANIFEST_PATH = os.path.join(SCRIPT_DIR, "sound_selection.json")
FFMPEG = shutil.which("ffmpeg") or "ffmpeg"
SR = 44100  # engine SFX sample rate

# ---------------------------------------------------------------------------
# Slot configuration
# ---------------------------------------------------------------------------

# The 15 slots this tool owns (display order). Names match SfxId filenames.
SLOTS = [
    "sfx_weapon_sword", "sfx_weapon_dagger", "sfx_weapon_axe", "sfx_weapon_claymore",
    "sfx_weapon_cleaver",
    "sfx_weapon_pistol", "sfx_weapon_smg", "sfx_weapon_carbine", "sfx_weapon_revolver",
    "sfx_weapon_bow", "sfx_weapon_crossbow", "sfx_weapon_throw", "sfx_weapon_molotov",
    "sfx_weapon_chakram",
    "sfx_weapon_wand", "sfx_weapon_staff", "sfx_reload",
    "sfx_ricochet",  # chakram/bounce-projectile wall reflect
]

SLOT_CLASS = {
    "sfx_weapon_sword": "melee", "sfx_weapon_dagger": "melee",
    "sfx_weapon_axe": "melee", "sfx_weapon_claymore": "melee", "sfx_weapon_cleaver": "melee",
    "sfx_weapon_pistol": "gun", "sfx_weapon_smg": "gun",
    "sfx_weapon_carbine": "gun", "sfx_weapon_revolver": "gun",
    "sfx_weapon_bow": "bow", "sfx_weapon_crossbow": "bow",
    "sfx_weapon_throw": "throw", "sfx_weapon_molotov": "throw", "sfx_weapon_chakram": "throw",
    "sfx_weapon_wand": "magic", "sfx_weapon_staff": "magic",
    "sfx_reload": "reload",
    "sfx_ricochet": "ricochet",
}

# Per acoustic-class ranking profile:
#   strong/weak : filename/dir keyword tiers (strong=+3, weak=+1 per hit)
#   dur         : preferred (lo, hi) duration window in seconds
#   centroid    : target spectral centroid in Hz ("brightness")
#   sharp       : reward a fast transient attack (percussive onset)
#   multi       : reward multiple transients (e.g. reload = clicks/racks)
CLASS_PROFILES = {
    "gun": {
        "strong": ["gunshot", "gun", "shot", "shoot", "bang", "pistol", "smg", "rifle", "revolver", "blast"],
        "weak": ["pop", "crack", "fire", "hit", "slam", "impact"],
        "dur": (0.05, 0.60), "centroid": 3500, "sharp": True, "multi": False,
    },
    "melee": {
        "strong": ["sword", "blade", "slash", "slice", "swish", "swoosh", "stab", "chop", "cleave", "axe", "dagger", "knife"],
        "weak": ["swing", "cut", "hit", "whoosh", "metal"],
        "dur": (0.10, 0.80), "centroid": 2500, "sharp": True, "multi": False,
    },
    "bow": {
        "strong": ["bow", "arrow", "twang", "thwack", "bolt", "crossbow"],
        "weak": ["snap", "string", "swish", "whoosh", "launch"],
        "dur": (0.08, 0.70), "centroid": 2200, "sharp": True, "multi": False,
    },
    "magic": {
        "strong": ["magic", "magical", "spell", "cast", "zap", "wand", "staff", "arcane"],
        "weak": ["energy", "glow", "pulse", "fire", "charge"],
        "dur": (0.10, 0.90), "centroid": 3000, "sharp": False, "multi": False,
    },
    "throw": {
        "strong": ["swish", "whoosh", "throw", "swoosh", "swing"],
        "weak": ["wind", "air", "glass", "break", "shatter", "fire"],
        "dur": (0.10, 0.70), "centroid": 2500, "sharp": True, "multi": False,
    },
    "reload": {
        "strong": ["reload", "mag", "magazine", "clip", "cock", "rack", "latch", "lever", "bolt"],
        "weak": ["metal", "click", "mechanical", "gun", "load", "chamber", "slide"],
        "dur": (0.30, 1.20), "centroid": 3500, "sharp": False, "multi": True,
    },
    "ricochet": {
        "strong": ["ricochet", "ping", "ting", "zing", "clink", "metal", "bounce"],
        "weak": ["hit", "impact", "clank", "tink", "spark"],
        "dur": (0.05, 0.50), "centroid": 5000, "sharp": True, "multi": False,
    },
}

# Slider defaults for a slot with no saved manifest entry (identity = untouched source).
DEFAULT_PARAMS = {"trim": False, "pitch": 1.0, "stretch": 1.0, "gain": 0.0, "reverb": 0.0, "lowpass": None}

KEYWORD_SHORTLIST = 60  # decode+feature at most this many keyword hits per slot


# ---------------------------------------------------------------------------
# Small helpers
# ---------------------------------------------------------------------------

def clamp(v, lo, hi):
    return lo if v < lo else hi if v > hi else v


def _tokens(path):
    """Tokenize a candidate path: parent-dir + filename, all separators (incl. '/')
    normalized to spaces, lowercased into a word set + the joined string."""
    fname = os.path.splitext(os.path.basename(path))[0].lower()
    parent = os.path.basename(os.path.dirname(path)).lower()
    s = f"{parent}/{fname}"
    for sep in "-_./ ":
        s = s.replace(sep, " ")
    return s, set(s.split())


def _kw_hit(term, words, joined):
    """A keyword hits if it's a whole word, or (only for terms >=4 chars) a substring.

    The length gate is what stops short terms false-matching inside longer words —
    e.g. "mag" must NOT match "magical", but "shot" should still match "gunshot".
    """
    if term in words:
        return True
    return len(term) >= 4 and term in joined


def keyword_score(path, profile, slot):
    """Keyword hit score for a candidate against a class profile + per-slot seed terms."""
    joined, words = _tokens(path)
    hits = 0
    for w in profile["strong"]:
        if _kw_hit(w, words, joined):
            hits += 3
    for w in profile["weak"]:
        if _kw_hit(w, words, joined):
            hits += 1
    # Per-slot seed keywords borrowed from fetch_audio's SOUND_MAP nudge the right file up.
    for w in fetch_audio.SOUND_MAP.get(slot, ([],))[0]:
        if _kw_hit(w, words, joined):
            hits += 2
    return hits


# ---------------------------------------------------------------------------
# Decode + acoustic features (numpy/scipy) — used only for ranking
# ---------------------------------------------------------------------------

def decode_np(path):
    """Decode any wav/ogg to a mono float32 numpy array at SR via ffmpeg pipe.

    Used instead of the wave module because sources are frequently OGG, which
    Python's `wave` cannot read.
    """
    _load_dsp()
    cmd = [FFMPEG, "-v", "quiet", "-i", path, "-f", "f32le", "-ac", "1", "-ar", str(SR), "-"]
    p = subprocess.run(cmd, capture_output=True)
    if p.returncode != 0 or not p.stdout:
        return None
    return np.frombuffer(p.stdout, dtype="<f4")


def features(x):
    """Compute cheap acoustic descriptors used by acoustic_score()."""
    n = len(x)
    if n < 16:
        return None
    x = x.astype(np.float64)
    peak = float(np.max(np.abs(x))) or 1e-9
    rms = float(np.sqrt(np.mean(x * x)))
    env = np.abs(x)
    # attack time = seconds from start to first sample reaching 90% of peak envelope
    attack_idx = int(np.argmax(env >= 0.9 * peak))
    attack_t = attack_idx / SR
    # spectral centroid ("brightness") of a Hann-windowed magnitude spectrum
    mag = np.abs(np.fft.rfft(x * np.hanning(n)))
    freqs = np.fft.rfftfreq(n, 1.0 / SR)
    centroid = float(np.sum(freqs * mag) / (np.sum(mag) + 1e-9))
    # transient count: envelope peaks above 30% of max, at least 30 ms apart
    peaks, _ = find_peaks(env, height=0.3 * peak, distance=max(1, int(0.03 * SR)))
    return {"dur": n / SR, "rms": rms, "attack_t": attack_t,
            "centroid": centroid, "n_transients": int(len(peaks))}


def acoustic_score(feats, profile):
    """Map features onto a [0,1] fit against the class profile."""
    dur = feats["dur"]
    dlo, dhi = profile["dur"]
    if dlo <= dur <= dhi:
        dur_fit = 1.0
    else:
        dur_fit = max(0.0, 1.0 - min(abs(dur - dlo), abs(dur - dhi)) / max(dhi, 1e-6))
    tgt = profile["centroid"]
    bright_fit = 1.0 - min(1.0, abs(feats["centroid"] - tgt) / tgt)
    attack_fit = (1.0 - min(1.0, feats["attack_t"] / 0.05)) if profile["sharp"] else 0.5
    if profile["multi"]:
        multi_fit = min(1.0, feats["n_transients"] / 3.0)
    else:
        multi_fit = 1.0 if feats["n_transients"] <= 2 else 0.5
    rms_fit = min(1.0, feats["rms"] / 0.15)  # avoid near-silent clips winning
    return 0.35 * dur_fit + 0.25 * bright_fit + 0.20 * attack_fit + 0.15 * multi_fit + 0.05 * rms_fit


# ---------------------------------------------------------------------------
# Effects: ffmpeg filter chain -> peak-normalized 16-bit mono samples
# ---------------------------------------------------------------------------

def atempo_chain(factor):
    """Decompose a tempo factor into a chain of atempo filters, since a single
    atempo instance only accepts 0.5..2.0."""
    out = []
    f = factor
    while f > 2.0 + 1e-9:
        out.append("atempo=2.0")
        f /= 2.0
    while f < 0.5 - 1e-9:
        out.append("atempo=0.5")
        f *= 2.0
    if abs(f - 1.0) > 1e-3:
        out.append(f"atempo={f:.5f}")
    return out


def build_af(p):
    """Build the ffmpeg -af graph for the effect params.

    Order: trim -> pitch (duration-preserving) -> stretch (constant pitch) ->
    lowpass -> reverb -> gain -> format. Peak-normalize happens afterwards in render().
    """
    f = []
    if p.get("trim"):
        # strip leading/trailing near-silence
        f.append("silenceremove=start_periods=1:start_threshold=-50dB:start_silence=0.01:"
                 "stop_periods=1:stop_threshold=-50dB:stop_silence=0.05")
    pit = p.get("pitch", 1.0)
    if abs(pit - 1.0) > 1e-3:
        # asetrate shifts pitch AND duration; atempo(1/pit) restores duration -> true pitch shift
        f.append(f"asetrate={SR}*{pit:.5f}")
        f.append(f"aresample={SR}")
        f += atempo_chain(1.0 / pit)
    st = p.get("stretch", 1.0)
    if abs(st - 1.0) > 1e-3:
        f += atempo_chain(st)  # independent time-stretch, pitch unchanged
    if p.get("lowpass"):
        f.append(f"lowpass=f={int(p['lowpass'])}")
    w = p.get("reverb", 0.0)
    if w > 1e-3:
        # aecho tail scaled by the wet amount; two fixed early reflections
        f.append(f"aecho=1:{0.3 + 0.5 * w:.3f}:40|90:{0.3 * w:.3f}|{0.2 * w:.3f}")
    g = p.get("gain", 0.0)
    if abs(g) > 1e-3:
        f.append(f"volume={g:.2f}dB")
    f.append(f"aformat=sample_rates={SR}:channel_layouts=mono")
    return ",".join(f)


def render(src, params):
    """Render src through the effect chain, then peak-normalize to -3 dBFS.

    Returns (samples_float_list, None) on success or (None, error_string) on failure.
    Normalizing LAST guarantees no clipping regardless of gain.
    """
    af = build_af(params)
    with tempfile.TemporaryDirectory() as td:
        tmp = os.path.join(td, "r.wav")
        cmd = [FFMPEG, "-v", "error", "-y", "-i", src, "-af", af,
               "-ar", str(SR), "-ac", "1", "-sample_fmt", "s16", "-f", "wav", tmp]
        r = subprocess.run(cmd, capture_output=True)
        if r.returncode != 0 or not os.path.isfile(tmp):
            return None, (r.stderr.decode(errors="replace").strip() or "ffmpeg render failed")
        res = fetch_audio._read_wav_raw(tmp)  # ffmpeg output is guaranteed WAV -> safe for wave module
    if res is None:
        return None, "could not read rendered wav"
    samples, _sr, ch, _sw = res
    if ch != 1:
        samples = fetch_audio._stereo_to_mono(samples, ch)
    # reuse fetch_audio's peak normalizer so output matches shipped assets exactly (no extra trim)
    samples = fetch_audio._normalize_peak(samples, target_db=-3.0)
    return samples, None


def raw_wav_bytes(src):
    """Decode src to an untouched 44100/16/mono WAV (for dry A/B audition)."""
    cmd = [FFMPEG, "-v", "error", "-i", src, "-ar", str(SR), "-ac", "1",
           "-sample_fmt", "s16", "-f", "wav", "-"]
    r = subprocess.run(cmd, capture_output=True)
    return r.stdout if r.returncode == 0 and r.stdout else None


def samples_to_wav_bytes(samples):
    """Serialize float samples in [-1,1] to a 16-bit mono WAV byte string."""
    ints = [int(clamp(s, -1.0, 1.0) * 32767) for s in samples]
    buf = io.BytesIO()
    with wave.open(buf, "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(SR)
        wf.writeframes(struct.pack(f"<{len(ints)}h", *ints))
    return buf.getvalue()


# ---------------------------------------------------------------------------
# Manifest (git-tracked reproducible selections)
# ---------------------------------------------------------------------------

def load_manifest():
    if os.path.isfile(MANIFEST_PATH):
        try:
            with open(MANIFEST_PATH) as f:
                m = json.load(f)
            if isinstance(m, dict) and isinstance(m.get("slots"), dict):
                return m
        except (OSError, ValueError):
            pass
    return {"version": 1, "generated_by": "pick_sfx.py", "slots": {}}


def save_manifest(m):
    m["version"] = 1
    m["generated_by"] = "pick_sfx.py"
    with open(MANIFEST_PATH, "w") as f:
        json.dump(m, f, indent=2, sort_keys=True)
        f.write("\n")


def sanitize_params(d):
    """Clamp/normalize a raw param dict (from query string or JSON body)."""
    lp = d.get("lowpass", None)
    try:
        lp = int(float(lp)) if lp not in (None, "", 0, "0", "null") else None
    except (TypeError, ValueError):
        lp = None
    return {
        "trim": bool(d.get("trim", False)) if not isinstance(d.get("trim"), str)
                else d.get("trim") in ("1", "true", "True"),
        "pitch": clamp(float(d.get("pitch", 1.0)), 0.5, 2.0),
        "stretch": clamp(float(d.get("stretch", 1.0)), 0.5, 2.0),
        "gain": clamp(float(d.get("gain", 0.0)), -24.0, 24.0),
        "reverb": clamp(float(d.get("reverb", 0.0)), 0.0, 1.0),
        "lowpass": (clamp(lp, 500, 20000) if lp else None),
    }


def write_slot_wav(slot, samples):
    """Write final samples to assets/audio/<slot>.wav and remove any shadowing .ogg.

    The engine loads sfx_<name>.ogg BEFORE .wav, so a stale OGG would hide our pick.
    """
    os.makedirs(OUTPUT_DIR, exist_ok=True)
    dst = os.path.join(OUTPUT_DIR, f"{slot}.wav")
    with open(dst, "wb") as f:
        f.write(samples_to_wav_bytes(samples))
    ogg = os.path.join(OUTPUT_DIR, f"{slot}.ogg")
    if os.path.isfile(ogg):
        os.remove(ogg)
    return dst


# ---------------------------------------------------------------------------
# Candidate pool bootstrapping
# ---------------------------------------------------------------------------

def discover_packs():
    """Return [(name, dir)] for packs already extracted in the cache."""
    dirs = []
    cache = fetch_audio.CACHE_DIR
    if not os.path.isdir(cache):
        return dirs
    for name, _ in fetch_audio.PACKS + [(n, None) for n, _ in fetch_audio.INDIVIDUAL_FILES]:
        d = os.path.join(cache, name)
        if os.path.exists(os.path.join(d, ".extracted")):
            dirs.append((name, d))
    return dirs


def ensure_pool(interactive):
    """Ensure the CC0 cache exists and return the list of candidate file paths."""
    dirs = discover_packs()
    if not dirs:
        if interactive:
            ans = input("CC0 audio cache is empty. Download packs now (~tens of MB)? [y/N] ").strip().lower()
            if ans == "y":
                dirs = fetch_audio.download_packs(force=False)
            else:
                print("Aborted. Run: python3 tools/fetch_audio.py --cache-only")
                sys.exit(1)
        else:
            print("ERROR: CC0 audio cache is empty.")
            print("Run: python3 tools/fetch_audio.py --cache-only")
            sys.exit(1)
    if not dirs:
        print("ERROR: no packs available after download.")
        sys.exit(1)
    files = [os.path.normpath(p) for p in fetch_audio._find_wav_files(dirs)]
    print(f"Candidate pool: {len(files)} CC0 files across {len(dirs)} packs")
    return files


# ---------------------------------------------------------------------------
# App state: ranking + preview cache
# ---------------------------------------------------------------------------

class App:
    def __init__(self, files, top, focus_slot=None):
        self.pool = files
        self.path_index = {p: i for i, p in enumerate(files)}
        self.top = top
        self.focus_slot = focus_slot
        self.feat_cache = {}                # path -> features dict or None
        self.rank_cache = {}                # slot -> ranked candidate list
        self.preview_cache = OrderedDict()  # key -> wav bytes (LRU, capped)
        self.manifest = load_manifest()
        self.lock = threading.Lock()

    # -- current shipped pick for a slot (manifest wins, else fetch_audio override) --
    def current_source(self, slot):
        entry = self.manifest["slots"].get(slot)
        rel = entry["source"] if entry else fetch_audio.MANUAL_OVERRIDES.get(slot)
        if not rel:
            return None
        path = os.path.normpath(os.path.join(fetch_audio.CACHE_DIR, rel))
        return path if os.path.isfile(path) else None

    def get_features(self, path):
        # decode outside the lock (slow); a rare duplicate decode is harmless
        with self.lock:
            if path in self.feat_cache:
                return self.feat_cache[path]
        x = decode_np(path)
        feats = features(x) if x is not None else None
        with self.lock:
            self.feat_cache[path] = feats
        return feats

    def rank(self, slot):
        with self.lock:
            if slot in self.rank_cache:
                return self.rank_cache[slot]
        profile = CLASS_PROFILES[SLOT_CLASS[slot]]
        # stage 1: keyword score whole pool, keep the best few for the (slow) acoustic pass
        scored = [(keyword_score(p, profile, slot), p) for p in self.pool]
        scored = [t for t in scored if t[0] > 0]
        scored.sort(key=lambda t: -t[0])
        shortlist = scored[:KEYWORD_SHORTLIST]

        cur = self.current_source(slot)
        cur_idx = self.path_index.get(cur) if cur else None

        results = []
        seen = set()
        for hits, p in shortlist:
            feats = self.get_features(p)
            if feats is None:
                continue
            idx = self.path_index[p]
            seen.add(idx)
            results.append(self._cand(idx, p, hits, feats, profile))
        # always include the current pick even if keyword-filtered out, so it can be A/B'd
        if cur_idx is not None and cur_idx not in seen:
            feats = self.get_features(cur)
            if feats is not None:
                results.append(self._cand(cur_idx, cur, keyword_score(cur, profile, slot), feats, profile))

        for r in results:
            r["current"] = (r["id"] == cur_idx)
        # sort by score, but pin the current pick to the very top
        results.sort(key=lambda r: (0 if r["current"] else 1, -r["score"]))
        top = results[:self.top]
        with self.lock:
            self.rank_cache[slot] = top
        return top

    def _cand(self, idx, path, hits, feats, profile):
        return {
            "id": idx,
            "rel": os.path.relpath(path, fetch_audio.CACHE_DIR),
            "dur": round(feats["dur"], 3),
            "centroid": int(feats["centroid"]),
            "score": round(hits * 100 + acoustic_score(feats, profile) * 50, 1),
            "hits": hits,
        }

    # -- payloads --
    def slots_payload(self):
        out = []
        for slot in SLOTS:
            entry = self.manifest["slots"].get(slot)
            out.append({
                "slot": slot,
                "cls": SLOT_CLASS[slot],
                "saved": entry is not None,
                "source": entry["source"] if entry else fetch_audio.MANUAL_OVERRIDES.get(slot, ""),
                "params": entry["params"] if entry else dict(DEFAULT_PARAMS),
            })
        return {"slots": out, "focus": self.focus_slot or ""}

    def candidates(self, slot):
        if slot not in SLOT_CLASS:
            return None
        return {"slot": slot, "candidates": self.rank(slot)}

    def _cache_key(self, cand, p):
        return (cand, round(p["pitch"], 2), round(p["stretch"], 2),
                round(p["gain"] * 2) / 2.0, round(p["reverb"], 2),
                (p["lowpass"] // 100 * 100) if p["lowpass"] else 0, bool(p["trim"]))

    def render_preview(self, cand, params):
        key = self._cache_key(cand, params)
        with self.lock:
            if key in self.preview_cache:
                self.preview_cache.move_to_end(key)
                return self.preview_cache[key], None
        samples, err = render(self.pool[cand], params)
        if samples is None:
            return None, err
        data = samples_to_wav_bytes(samples)
        with self.lock:
            self.preview_cache[key] = data
            self.preview_cache.move_to_end(key)
            while len(self.preview_cache) > 64:
                self.preview_cache.popitem(last=False)
        return data, None

    def save(self, slot, cand, params):
        if slot not in SLOT_CLASS:
            return {"ok": False, "error": f"unknown slot {slot}"}
        if not (0 <= cand < len(self.pool)):
            return {"ok": False, "error": "bad candidate id"}
        src = self.pool[cand]
        samples, err = render(src, params)
        if samples is None:
            return {"ok": False, "error": err}
        dst = write_slot_wav(slot, samples)
        rel = os.path.relpath(src, fetch_audio.CACHE_DIR)
        self.manifest["slots"][slot] = {"source": rel, "params": params}
        save_manifest(self.manifest)
        # invalidate this slot's rank cache so the new pick shows as "current" on reload
        with self.lock:
            self.rank_cache.pop(slot, None)
        return {"ok": True, "path": os.path.relpath(dst, GAME_ROOT),
                "dur": round(len(samples) / SR, 3), "bytes": os.path.getsize(dst)}


# ---------------------------------------------------------------------------
# HTTP handler
# ---------------------------------------------------------------------------

class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass  # keep the console clean; we print() our own status

    def _send(self, code, body, ctype="application/json"):
        if isinstance(body, (dict, list)):
            body = json.dumps(body).encode()
        elif isinstance(body, str):
            body = body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass  # browser aborted a preview mid-drag — expected

    def _params_from_query(self, q):
        raw = {k: v[0] for k, v in q.items()}
        return sanitize_params(raw)

    def do_GET(self):
        u = urlparse(self.path)
        q = parse_qs(u.query)
        app = self.server.app
        if u.path == "/":
            self._send(200, INDEX_HTML.replace("__FOCUS__", json.dumps(app.focus_slot or "")), "text/html")
        elif u.path == "/favicon.ico":
            self._send(204, b"", "image/x-icon")
        elif u.path == "/api/slots":
            self._send(200, app.slots_payload())
        elif u.path == "/api/candidates":
            slot = q.get("slot", [""])[0]
            res = app.candidates(slot)
            self._send(200 if res else 400, res or {"error": "unknown slot"})
        elif u.path in ("/preview", "/raw"):
            try:
                cand = int(q.get("cand", ["-1"])[0])
            except ValueError:
                cand = -1
            if not (0 <= cand < len(app.pool)):
                self._send(400, {"error": "bad candidate id"})
                return
            if u.path == "/raw":
                data = raw_wav_bytes(app.pool[cand])
                if data is None:
                    self._send(500, {"error": "decode failed"})
                else:
                    self._send(200, data, "audio/wav")
            else:
                data, err = app.render_preview(cand, self._params_from_query(q))
                if data is None:
                    self._send(500, {"error": err})
                else:
                    self._send(200, data, "audio/wav")
        else:
            self._send(404, {"error": "not found"})

    def do_POST(self):
        app = self.server.app
        n = int(self.headers.get("Content-Length", 0) or 0)
        try:
            data = json.loads(self.rfile.read(n) or b"{}")
        except ValueError:
            self._send(400, {"error": "bad json"})
            return
        if self.path == "/save":
            params = sanitize_params(data.get("params", {}))
            res = app.save(data.get("slot", ""), int(data.get("cand", -1)), params)
            self._send(200 if res.get("ok") else 400, res)
        elif self.path == "/shutdown":
            self._send(200, {"ok": True})
            threading.Thread(target=self.server.shutdown, daemon=True).start()
        else:
            self._send(404, {"error": "not found"})


# ---------------------------------------------------------------------------
# Embedded single-page UI (vanilla HTML/CSS/JS, no external assets)
# ---------------------------------------------------------------------------

INDEX_HTML = r"""<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8"><title>DungeonEngine SFX Picker</title>
<style>
  :root { color-scheme: dark; }
  * { box-sizing: border-box; }
  body { margin:0; font:14px/1.4 system-ui,sans-serif; background:#15171c; color:#d8dbe0; }
  header { padding:10px 16px; background:#1d2129; border-bottom:1px solid #2c313c; display:flex;
           align-items:center; gap:12px; }
  header h1 { font-size:15px; margin:0; font-weight:600; }
  header .hint { color:#8a919e; font-size:12px; }
  #wrap { display:flex; height:calc(100vh - 45px); }
  #slots { width:210px; overflow-y:auto; border-right:1px solid #2c313c; }
  .slot { padding:8px 12px; cursor:pointer; border-bottom:1px solid #23272f; display:flex;
          justify-content:space-between; align-items:center; }
  .slot:hover { background:#20242c; }
  .slot.active { background:#2a3140; }
  .slot .cls { color:#7f8794; font-size:11px; }
  .slot .dot { width:8px; height:8px; border-radius:50%; background:#3f4757; }
  .slot.saved .dot { background:#4a9d5b; }
  #main { flex:1; overflow-y:auto; padding:16px; }
  #cands { border:1px solid #2c313c; border-radius:6px; overflow:hidden; margin-bottom:16px; }
  .cand { display:flex; align-items:center; gap:10px; padding:7px 10px; border-bottom:1px solid #23272f;
          cursor:pointer; }
  .cand:hover { background:#20242c; }
  .cand.sel { background:#2a3140; }
  .cand.cur { border-left:3px solid #c9a227; }
  .cand .rel { flex:1; font-family:ui-monospace,monospace; font-size:12px; overflow:hidden;
               text-overflow:ellipsis; white-space:nowrap; }
  .cand .meta { color:#8a919e; font-size:11px; width:150px; text-align:right; }
  .cand button { font-size:11px; padding:2px 8px; }
  .tag { background:#c9a227; color:#15171c; font-size:10px; padding:1px 5px; border-radius:3px;
         font-weight:700; }
  #fx { display:grid; grid-template-columns:110px 1fr 70px; gap:8px 12px; align-items:center;
        max-width:640px; }
  #fx label { color:#aeb4bf; }
  #fx input[type=range] { width:100%; }
  #fx .val { font-family:ui-monospace,monospace; text-align:right; color:#d8dbe0; }
  .bar { margin-top:16px; display:flex; gap:10px; align-items:center; max-width:640px; }
  button { background:#2f3644; color:#d8dbe0; border:1px solid #3c4557; border-radius:5px;
           padding:7px 14px; cursor:pointer; }
  button:hover { background:#3a4252; }
  button.primary { background:#3a6ea5; border-color:#4a80bd; }
  button:disabled { opacity:.4; cursor:default; }
  #toast { margin-left:auto; font-size:12px; color:#8a919e; }
  #toast.ok { color:#6bcf7f; } #toast.err { color:#e56b6b; }
  h2 { font-size:13px; color:#aeb4bf; margin:0 0 8px; text-transform:uppercase; letter-spacing:.04em; }
</style></head>
<body>
<header>
  <h1>Weapon / Reload SFX Picker</h1>
  <span class="hint">pick a slot &rarr; audition candidates &rarr; dial in FX &rarr; save</span>
</header>
<div id="wrap">
  <div id="slots"></div>
  <div id="main">
    <div id="empty" class="hint">Select a slot on the left to begin.</div>
    <div id="editor" style="display:none">
      <h2 id="slotTitle"></h2>
      <div id="cands"></div>
      <h2>Effects</h2>
      <div id="fx"></div>
      <div class="bar">
        <button id="playFx" class="primary" disabled>&#9654; Preview FX</button>
        <button id="save" disabled>Save to assets/audio</button>
        <button id="reset">Reset FX</button>
        <span id="toast"></span>
      </div>
    </div>
  </div>
</div>
<audio id="au"></audio>
<script>
const FOCUS = __FOCUS__;
const au = document.getElementById('au');
let slots = [], cur = null, selCand = null, cands = [], blobUrl = null;
const FX = [
  { k:'pitch',   label:'Pitch',    min:0.5, max:2,     step:0.01, def:1,  fmt:v=>v.toFixed(2)+'x' },
  { k:'stretch', label:'Stretch',  min:0.5, max:2,     step:0.01, def:1,  fmt:v=>v.toFixed(2)+'x' },
  { k:'gain',    label:'Gain',     min:-24, max:24,    step:0.5,  def:0,  fmt:v=>v.toFixed(1)+' dB' },
  { k:'reverb',  label:'Reverb',   min:0,   max:1,     step:0.05, def:0,  fmt:v=>Math.round(v*100)+'%' },
  { k:'lowpass', label:'Low-pass', min:500, max:20000, step:100,  def:20000, fmt:v=>v>=20000?'off':(v/1000).toFixed(1)+' kHz' },
];

function toast(msg, cls){ const t=document.getElementById('toast'); t.textContent=msg; t.className=cls||''; }

async function loadSlots(){
  const r = await fetch('/api/slots'); const d = await r.json();
  slots = d.slots;
  const el = document.getElementById('slots'); el.innerHTML='';
  slots.forEach(s => {
    const div = document.createElement('div');
    div.className = 'slot' + (s.saved?' saved':'');
    div.innerHTML = `<span>${s.slot.replace('sfx_','')}<div class="cls">${s.cls}</div></span><span class="dot"></span>`;
    div.onclick = () => selectSlot(s.slot);
    div.dataset.slot = s.slot;
    el.appendChild(div);
  });
  if (FOCUS) selectSlot(FOCUS);
}

function params(){
  const p = {};
  FX.forEach(f => { p[f.k] = parseFloat(document.getElementById('fx_'+f.k).value); });
  if (p.lowpass >= 20000) p.lowpass = null;           // slider max == off
  p.trim = document.getElementById('fx_trim').checked;
  return p;
}
function qs(p){
  const u = new URLSearchParams();
  u.set('cand', selCand);
  ['pitch','stretch','gain','reverb'].forEach(k=>u.set(k,p[k]));
  u.set('lowpass', p.lowpass||0); u.set('trim', p.trim?1:0);
  return u.toString();
}

function buildFx(saved){
  const fx = document.getElementById('fx'); fx.innerHTML='';
  FX.forEach(f => {
    let v = f.def;
    if (saved && saved[f.k] != null) v = saved[f.k];
    if (f.k==='lowpass' && (!saved || saved.lowpass==null)) v = 20000;
    const val = document.createElement('span'); val.className='val'; val.id='val_'+f.k; val.textContent=f.fmt(v);
    const lab = document.createElement('label'); lab.textContent=f.label;
    const inp = document.createElement('input');
    inp.type='range'; inp.id='fx_'+f.k; inp.min=f.min; inp.max=f.max; inp.step=f.step; inp.value=v;
    inp.oninput = () => { val.textContent=f.fmt(parseFloat(inp.value)); schedulePreview(); };
    fx.appendChild(lab); fx.appendChild(inp); fx.appendChild(val);
  });
  // trim checkbox row
  const lab = document.createElement('label'); lab.textContent='Trim silence';
  const cb = document.createElement('input'); cb.type='checkbox'; cb.id='fx_trim';
  cb.checked = saved ? !!saved.trim : false; cb.onchange = schedulePreview;
  const spacer = document.createElement('span');
  fx.appendChild(lab); fx.appendChild(cb); fx.appendChild(spacer);
}

async function selectSlot(slot){
  cur = slots.find(s => s.slot===slot); selCand=null;
  document.querySelectorAll('.slot').forEach(d => d.classList.toggle('active', d.dataset.slot===slot));
  document.getElementById('empty').style.display='none';
  document.getElementById('editor').style.display='';
  document.getElementById('slotTitle').textContent = slot + '  (' + cur.cls + ')';
  buildFx(cur.params);
  document.getElementById('playFx').disabled = true;
  document.getElementById('save').disabled = true;
  toast('ranking candidates…');
  const r = await fetch('/api/candidates?slot='+encodeURIComponent(slot));
  const d = await r.json(); cands = d.candidates || [];
  const box = document.getElementById('cands'); box.innerHTML='';
  cands.forEach(c => {
    const row = document.createElement('div');
    row.className = 'cand' + (c.current?' cur':'');
    row.innerHTML =
      `<span class="rel">${c.rel}</span>`+
      `<span class="meta">${c.dur.toFixed(2)}s · ${c.centroid}Hz · ${c.score}${c.current?' <span class=tag>CURRENT</span>':''}</span>`+
      `<button>&#9654; dry</button>`;
    row.querySelector('button').onclick = (e)=>{ e.stopPropagation(); playDry(c.id); };
    row.onclick = () => pickCand(c.id, row);
    box.appendChild(row);
  });
  toast(cands.length + ' candidates', 'ok');
  // auto-select the pinned current pick so FX preview is immediately usable
  if (cands.length){ const el=box.children[0]; pickCand(cands[0].id, el); }
}

function pickCand(id, row){
  selCand = id;
  document.querySelectorAll('.cand').forEach(r=>r.classList.remove('sel'));
  if (row) row.classList.add('sel');
  document.getElementById('playFx').disabled=false;
  document.getElementById('save').disabled=false;
  schedulePreview();
}

function playDry(id){ stopBlob(); au.src='/raw?cand='+id; au.play().catch(()=>{}); }

let previewTimer=null;
function schedulePreview(){ clearTimeout(previewTimer); previewTimer=setTimeout(preview, 350); }
async function preview(){
  if (selCand==null) return;
  toast('rendering…');
  try {
    const r = await fetch('/preview?'+qs(params()));
    if (!r.ok){ const e=await r.json(); toast('ffmpeg: '+(e.error||'error'),'err'); return; }
    stopBlob(); blobUrl = URL.createObjectURL(await r.blob());
    au.src = blobUrl; au.play().catch(()=>{}); toast('preview','ok');
  } catch(e){ toast('preview failed','err'); }
}
function stopBlob(){ if (blobUrl){ URL.revokeObjectURL(blobUrl); blobUrl=null; } }

document.getElementById('playFx').onclick = preview;
document.getElementById('reset').onclick = () => { buildFx(null); schedulePreview(); };
document.getElementById('save').onclick = async () => {
  if (selCand==null) return;
  toast('saving…');
  const r = await fetch('/save', {method:'POST', headers:{'Content-Type':'application/json'},
    body: JSON.stringify({slot:cur.slot, cand:selCand, params:params()})});
  const d = await r.json();
  if (d.ok){ toast('saved '+d.path+' ('+d.dur+'s, '+Math.round(d.bytes/1024)+'KB)','ok');
             const s=slots.find(x=>x.slot===cur.slot); s.saved=true; s.params=params();
             document.querySelector('.slot[data-slot="'+cur.slot+'"]').classList.add('saved'); }
  else { toast('save failed: '+(d.error||''),'err'); }
};
loadSlots();
</script>
</body></html>"""


# ---------------------------------------------------------------------------
# --apply (headless re-render from manifest)
# ---------------------------------------------------------------------------

def do_apply():
    manifest = load_manifest()
    entries = manifest["slots"]
    if not entries:
        print("Manifest is empty — nothing to apply. Pick some sounds first (run without --apply).")
        return 0
    if not os.path.isdir(fetch_audio.CACHE_DIR):
        print("ERROR: CC0 cache missing. Run: python3 tools/fetch_audio.py --cache-only")
        return 1
    ok = 0
    fail = 0
    for slot, entry in sorted(entries.items()):
        src = os.path.normpath(os.path.join(fetch_audio.CACHE_DIR, entry["source"]))
        if not os.path.isfile(src):
            print(f"  MISSING  {slot:22s} <- {entry['source']} (run fetch_audio.py)")
            fail += 1
            continue
        samples, err = render(src, sanitize_params(entry.get("params", {})))
        if samples is None:
            print(f"  FAIL     {slot:22s}: {err}")
            fail += 1
            continue
        dst = write_slot_wav(slot, samples)
        print(f"  OK       {slot:22s} <- {entry['source']}  ({len(samples)/SR:.2f}s)")
        ok += 1
    print(f"\nApplied {ok} slot(s), {fail} failed.")
    return 1 if fail else 0


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description="Interactive weapon/reload SFX picker for DungeonEngine.")
    ap.add_argument("--port", type=int, default=8765, help="web server port (default 8765)")
    ap.add_argument("--host", default="127.0.0.1",
                    help="bind address (default 127.0.0.1; use 0.0.0.0 to expose on the LAN)")
    ap.add_argument("--slot", help="focus a single slot on launch (e.g. sfx_weapon_pistol)")
    ap.add_argument("--apply", action="store_true", help="headless: re-render all manifest slots and exit")
    ap.add_argument("--no-open-browser", action="store_true", help="don't auto-open the browser")
    ap.add_argument("--cache-dir", help="override build/audio_cache location")
    ap.add_argument("--top", type=int, default=30, help="candidates shown per slot (default 30)")
    ap.add_argument("--list-slots", action="store_true", help="print slots + saved state and exit")
    args = ap.parse_args()

    if not shutil.which("ffmpeg"):
        print("ERROR: ffmpeg not found on PATH — required for decoding and effects.")
        sys.exit(1)

    # Redirect fetch_audio's cache/output globals if the user overrode the cache dir.
    if args.cache_dir:
        fetch_audio.CACHE_DIR = os.path.abspath(args.cache_dir)

    if args.slot and args.slot not in SLOT_CLASS:
        print(f"ERROR: unknown slot '{args.slot}'. Valid: {', '.join(SLOTS)}")
        sys.exit(1)

    if args.list_slots:
        m = load_manifest()
        print(f"{'SLOT':24s} {'CLASS':7s} STATE")
        for s in SLOTS:
            e = m["slots"].get(s)
            state = f"saved <- {e['source']}" if e else f"default <- {fetch_audio.MANUAL_OVERRIDES.get(s,'?')}"
            print(f"{s:24s} {SLOT_CLASS[s]:7s} {state}")
        return

    if args.apply:
        sys.exit(do_apply())

    # The interactive server ranks candidates, which needs numpy + scipy.
    try:
        _load_dsp()
    except ImportError:
        print("ERROR: the interactive picker needs numpy + scipy for candidate ranking.")
        print("Install them (pip install numpy scipy) or use --apply for a headless re-render.")
        sys.exit(1)

    files = ensure_pool(interactive=not args.no_open_browser)
    app = App(files, top=args.top, focus_slot=args.slot)

    try:
        httpd = ThreadingHTTPServer((args.host, args.port), Handler)
    except OSError as e:
        print(f"ERROR: could not bind {args.host}:{args.port}: {e}")
        print("Another instance may be running — try --port <other>.")
        sys.exit(1)
    httpd.app = app

    # When bound to all interfaces, resolve the outbound LAN IP so the printed URL
    # is reachable from other devices (the connect() sends no packets).
    display_host = args.host
    if args.host in ("0.0.0.0", "::"):
        try:
            import socket
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.connect(("8.8.8.8", 80))
            display_host = s.getsockname()[0]
            s.close()
        except OSError:
            display_host = "localhost"
    url = f"http://{display_host}:{args.port}"
    print(f"\nSFX picker serving at {url}   (Ctrl-C to stop)")
    if args.host in ("0.0.0.0", "::"):
        print("  (exposed on the LAN — anyone on this network can reach it)")
    print(f"Scope: {len(SLOTS)} weapon/reload slots.  Manifest: {os.path.relpath(MANIFEST_PATH, GAME_ROOT)}")
    if not args.no_open_browser:
        try:
            import webbrowser
            webbrowser.open(url)
        except Exception:
            pass  # headless / no browser — the URL is printed above
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nStopped.")


if __name__ == "__main__":
    main()
