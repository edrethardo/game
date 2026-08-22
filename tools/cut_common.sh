#!/usr/bin/env bash
# cut_common.sh — shared helpers for the two trailer cut scripts (WB-273).
# Everything renders to uniform 1280x720/60/yuv420p segments, hard cuts via concat,
# then ONE final pass adds the music bed and upscales to the 1920x1080 Steam master.
set -euo pipefail
TAKES="${TAKES:-/home/aaron/game_takes}"
WORK="$TAKES/.cut_work"; mkdir -p "$WORK"
FONT=/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf
SEG=0; LIST="$WORK/concat.txt"; : > "$LIST"

# NB: the counter must increment in the PARENT shell — `out=$(seg_out)` runs the increment in a
# subshell, so every call returned seg_001 and each segment overwrote the last (the first render
# of the cinematic was the end card ten times over, 35 s of WISHLIST ON STEAM).
next_seg() { SEG=$((SEG+1)); OUT=$(printf '%s/seg_%03d.mp4' "$WORK" "$SEG"); }

# clip <take-dir-name> <start> <dur>
clip() {
  next_seg; local out="$OUT"
  ffmpeg -hide_banner -loglevel error -y -ss "$2" -t "$3" -i "$TAKES/$1/take.mp4" \
    -vf "scale=1280:720,fps=60,format=yuv420p" -an -c:v libx264 -preset fast -crf 18 "$out"
  echo "file '$out'" >> "$LIST"
}

# card <text> <dur> [fade=in|out|both|none]  — \n in text = line break
card() {
  next_seg; local out="$OUT" fade filt; fade="${3:-none}"
  filt="drawtext=fontfile=$FONT:text='$1':fontcolor=0xE8D9A0:fontsize=54:line_spacing=18:x=(w-text_w)/2:y=(h-text_h)/2"
  case "$fade" in
    in)   filt="$filt,fade=t=in:st=0:d=0.8";;
    out)  filt="$filt,fade=t=out:st=$(echo "$2-0.9"|bc):d=0.9";;
    both) filt="$filt,fade=t=in:st=0:d=0.8,fade=t=out:st=$(echo "$2-0.9"|bc):d=0.9";;
  esac
  ffmpeg -hide_banner -loglevel error -y -f lavfi -i "color=c=0x0A0A10:s=1280x720:r=60:d=$2" \
    -vf "$filt,format=yuv420p" -an -c:v libx264 -preset fast -crf 18 "$out"
  echo "file '$out'" >> "$LIST"
}

# finish <out.mp4> — concat + placeholder music (afaded to the video length) + 1080p master
finish() {
  local silent="$WORK/silent.mp4"
  ffmpeg -hide_banner -loglevel error -y -f concat -safe 0 -i "$LIST" -c copy "$silent"
  local dur; dur=$(ffprobe -v error -show_entries format=duration -of csv=p=0 "$silent")
  ffmpeg -hide_banner -loglevel error -y -i "$silent" -i "$TAKES/PLACEHOLDER_music_bed.wav" \
    -filter_complex "[1:a]atrim=0:$dur,afade=t=out:st=$(echo "$dur-2.5"|bc):d=2.5[a]" \
    -map 0:v -map "[a]" -vf "scale=1920:1080:flags=lanczos" \
    -c:v libx264 -preset slow -crf 18 -c:a aac -b:a 192k -movflags +faststart -shortest "$1"
  echo "== $1: $(ffprobe -v error -show_entries format=duration -of csv=p=0 "$1") s =="
}
