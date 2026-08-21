#!/usr/bin/env bash
# encode_trailer.sh — turn a --record frame dump into a 60 fps H.264 clip (WB-268).
#
# The game's --record mode locksteps the sim (one tick per frame) and writes
# <dir>/frame_000000.png onwards plus a manifest.txt naming the tick rate. This
# script is the other half of that contract: encode at the manifest's fps so the
# clip plays in exact real time, however slowly the capture itself ran.
#
#   tools/encode_trailer.sh <record-dir> [out.mp4] [--scale 1920x1080]
#
# CRF 18 / preset slow: visually lossless enough for an edit master; the final
# trailer export re-encodes anyway, so never cut from a lossier intermediate.
set -euo pipefail

DIR="${1:?usage: encode_trailer.sh <record-dir> [out.mp4] [--scale WxH]}"
OUT="${2:-}"
SCALE=""
if [ "${OUT}" = "--scale" ]; then OUT=""; SCALE="${3:?--scale needs WxH}";
elif [ "${3:-}" = "--scale" ]; then SCALE="${4:?--scale needs WxH}"; fi
[ -n "$OUT" ] || OUT="${DIR%/}/take.mp4"

[ -e "$DIR/frame_000000.png" ] || { echo "no frames in $DIR (expected frame_000000.png)" >&2; exit 1; }

# fps comes from the manifest when the run exited cleanly; 60 is the engine's
# fixed tick rate and the correct default for a run that crashed before writing it.
FPS=60
if [ -f "$DIR/manifest.txt" ]; then
  FPS="$(sed -n 's/^fps=\([0-9]\+\)$/\1/p' "$DIR/manifest.txt" | head -1)"
  [ -n "$FPS" ] || FPS=60
fi

VF="format=yuv420p"
[ -n "$SCALE" ] && VF="scale=${SCALE/x/:}:flags=lanczos,format=yuv420p"

ffmpeg -hide_banner -loglevel warning -y \
  -framerate "$FPS" -i "$DIR/frame_%06d.png" \
  -c:v libx264 -preset slow -crf 18 -vf "$VF" -movflags +faststart \
  "$OUT"

echo "encoded: $OUT ($(ffprobe -v error -select_streams v:0 -show_entries stream=nb_frames,avg_frame_rate -of csv=p=0 "$OUT" 2>/dev/null || true))"
