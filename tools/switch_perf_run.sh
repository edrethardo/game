#!/usr/bin/env bash
# switch_perf_run.sh — measure frame rate ON THE CONSOLE, not by inference.
#
# WHY. Every performance number this project has for the stacked floors is desktop draw-call
# accounting. The Switch is the machine that actually drops to 30 FPS, and it had never been
# measured — which is how a "fix" (the Y-banded section split) got built against an assumed
# bottleneck and turned out 48% WORSE when finally A/B'd.
#
# It works by launching the game over nxlink with `-s`, which streams the console's stdout back
# here, and passing launch flags via --args so the run lands on a specific floor type with the bot
# driving. The game emits one [DEVPERF] line per second carrying the two things desktop cannot
# supply: the delivered frame rate, and whether the console is DOCKED or HANDHELD (the Switch CPU
# runs at 1020 MHz in both modes and only the GPU clock changes, so a docked/handheld gap is
# GPU-bound and no gap means CPU submission).
#
# The console must be sitting on the HOMEBREW MENU — netloader only listens there.
#
#   ./tools/switch_perf_run.sh                    # VERTICAL_HALL, 180 s
#   ./tools/switch_perf_run.sh --fourstory 240    # the Descent maze, 240 s
#   ./tools/switch_perf_run.sh --floor 12         # a flat floor, for the baseline comparison
#
# DOCK OR UNDOCK PART-WAY THROUGH. The summary splits by mode, and having both in one capture is
# what makes the comparison fair — same build, same floor, same seed.

set -u

# Pin the C locale for the summary. awk's printf follows LC_NUMERIC, and on a comma-decimal machine
# (de_DE here) the frame rates come out as "53,5" — the same trap that once corrupted every float in
# the balance CSV. Measurements must not depend on the machine that prints them.
export LC_ALL=C

STYLE="${1:---vhall}"
SECONDS_TO_RUN="${2:-180}"
SWITCH_IP="${SWITCH_IP:-192.168.2.54}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
NRO="$ROOT/build-switch/DungeonEngine.nro"
OUT="${OUT:-$ROOT/switch_perf.log}"

if [ ! -f "$NRO" ]; then
  echo "ERROR: $NRO not found — build it first:"
  echo "  docker run --rm -u \$(id -u):\$(id -g) -v \$PWD:/game -w /game devkitpro/devkita64 \\"
  echo "    bash -c 'source /opt/devkitpro/switchvars.sh && cmake --build build-switch -j8'"
  exit 2
fi

echo "=== Switch perf capture ==="
echo "  console : $SWITCH_IP  (must be on the HOMEBREW MENU)"
echo "  scenario: --autoplay --new warrior --devperf $STYLE"
echo "  duration: ${SECONDS_TO_RUN}s   ->  $OUT"
echo
echo "  Dock or undock part-way through to get both modes in one capture."
echo

# -s keeps nxlink attached as a stdio server so the console's printf output streams back to us.
timeout "$SECONDS_TO_RUN" docker run --rm --network host -v "$ROOT:/game" devkitpro/devkita64 \
  nxlink -s -a "$SWITCH_IP" --args "--autoplay --new warrior --devperf $STYLE" \
  /game/build-switch/DungeonEngine.nro 2>&1 | tee "$OUT"

echo
if ! grep -q "DEVPERF" "$OUT"; then
  echo "=== NO SAMPLES ==="
  if grep -qi "connection to .* failed" "$OUT"; then
    echo "nxlink could not connect. The console must be sitting on the homebrew menu"
    echo "(netloader only listens there) and awake, on the same network."
  else
    echo "Connected, but the game emitted no [DEVPERF] lines. Either it did not reach gameplay,"
    echo "or this .nro predates the probe — rebuild build-switch and retry."
  fi
  exit 1
fi

echo "=== RESULTS ==="
awk '
  /\[DEVPERF\]/ {
    mode=""; fps=""; d=""; ent=""; style=""
    for (i = 1; i <= NF; i++) {
      split($i, kv, "=")
      if (kv[1] == "mode")  mode  = kv[2]
      if (kv[1] == "fps")   fps   = kv[2]+0
      if (kv[1] == "D")     d     = kv[2]+0
      if (kv[1] == "ent")   ent   = kv[2]+0
      if (kv[1] == "style") style = kv[2]
    }
    # Drop the warm-up sample: m_displayFps is 0 until the first full second elapses, and a zero
    # would both halve the average and masquerade as the worst frame rate of the run.
    if (mode == "" || fps == "" || fps == 0) next
    n[mode]++; sf[mode]+=fps; sd[mode]+=d; se[mode]+=ent
    if (mnf[mode] == "" || fps < mnf[mode]) mnf[mode]=fps
    if (fps > mxf[mode]) mxf[mode]=fps
    if (d > mxd[mode]) mxd[mode]=d
    styles[style]=1
  }
  END {
    printf "%-10s %6s %8s %8s %8s %9s %9s\n", "MODE","n","fps avg","fps min","fps max","draw avg","draw max"
    for (m in n)
      printf "%-10s %6d %8.1f %8d %8d %9.0f %9d\n", m, n[m], sf[m]/n[m], mnf[m], mxf[m], sd[m]/n[m], mxd[m]
    printf "\nlayout styles seen:"; for (s in styles) printf " %s", s; printf "\n"
    printf "(style 4 = VERTICAL_HALL, 5 = FOUR_STORY, 0 = flat rooms)\n"
  }
' "$OUT"

echo
echo "How to read it:"
echo "  60 fps, draw ~350   -> the occlusion cull fixed it; draw calls were the limiter."
echo "  ~30 fps, draw ~350  -> cull worked but draw calls were NEVER the limiter. Fill rate next."
echo "  draw still ~700     -> the cull is not engaging on the device. Fix that before anything else."
echo "  docked == handheld  -> CPU submission bound, not GPU."
