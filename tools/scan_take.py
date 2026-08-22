#!/usr/bin/env python3
"""scan_take.py — find usable trim windows in a take.mp4 (WB-272/273 QC).

The v6 pipeline deletes the PNG frame dumps after encoding (a 1080p dump is ~1 GB), so the
old QC trick — judging frames by their PNG file size on disk — no longer has files to judge.
This scans the ENCODED take instead: it decodes a low-rate sample ladder and measures each
sample's JPEG-compressed size plus mean luma, which separates real gameplay (busy, bright
enough, compresses badly) from the three known dead spots that have each shipped in a cut
once: floor-transition slates (near-black), the death screen (dark vignette + text), and
menu/loading frames.

  tools/scan_take.py <take.mp4> [--step 0.25] [--min-kb 45] [--blue]

Prints every clean window (start dur), longest first. --blue additionally prints the top
frozen-orb moments (bright-blue pixel mass per sample) — the sorcerer-opener trim rides that.
"""
import argparse, io, subprocess, sys

from PIL import Image


def sample(path, step):
    """Decode ~4 fps samples straight from the take; yields (t, jpeg_kb, mean_luma, img)."""
    fps = 1.0 / step
    proc = subprocess.Popen(
        ["ffmpeg", "-v", "error", "-i", path, "-vf", f"fps={fps},scale=480:270",
         "-c:v", "mjpeg", "-q:v", "5", "-f", "image2pipe", "-"],
        stdout=subprocess.PIPE)
    buf = proc.stdout.read()
    proc.wait()
    # split the MJPEG stream on JPEG SOI markers
    out, start, i, t = [], None, 0, 0.0
    while True:
        j = buf.find(b"\xff\xd8\xff", (start + 2) if start is not None else 0)
        if start is not None:
            chunk = buf[start:j if j != -1 else len(buf)]
            img = Image.open(io.BytesIO(chunk)).convert("RGB")
            px = img.resize((48, 27))
            luma = sum(0.299 * r + 0.587 * g + 0.114 * b for r, g, b in px.getdata()) / (48 * 27)
            out.append((round(i * step, 2), len(chunk) / 1024.0, luma, px))
            i += 1
        if j == -1:
            break
        start = j
    return out


def windows(samples, min_kb, min_luma=14.0):
    """Contiguous runs where the sample is both busy and lit; returns [(start, dur)]."""
    runs, cur = [], None
    for t, kb, luma, _ in samples:
        ok = kb >= min_kb and luma >= min_luma
        if ok and cur is None:
            cur = t
        elif not ok and cur is not None:
            runs.append((cur, round(t - cur, 2))); cur = None
    if cur is not None and samples:
        runs.append((cur, round(samples[-1][0] - cur, 2)))
    return sorted(runs, key=lambda w: -w[1])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("take")
    ap.add_argument("--step", type=float, default=0.25)
    ap.add_argument("--min-kb", type=float, default=45.0)
    ap.add_argument("--blue", action="store_true",
                    help="also rank frozen-orb moments by bright-blue pixel mass")
    a = ap.parse_args()

    s = sample(a.take, a.step)
    if not s:
        sys.exit("no samples decoded")
    print(f"{len(s)} samples over {s[-1][0]:.1f}s  "
          f"(kb min/med/max {min(x[1] for x in s):.0f}/{sorted(x[1] for x in s)[len(s)//2]:.0f}/{max(x[1] for x in s):.0f})")
    for w in windows(s, a.min_kb)[:6]:
        print(f"clean {w[0]:7.2f} +{w[1]:.2f}s")
    if a.blue:
        scored = []
        for t, _, _, px in s:
            blue = sum(1 for r, g, b in px.getdata() if b > 120 and b > r + 30 and b > g + 15)
            scored.append((blue, t))
        for blue, t in sorted(scored, reverse=True)[:8]:
            print(f"blue  {t:7.2f}  mass={blue}")


if __name__ == "__main__":
    main()
