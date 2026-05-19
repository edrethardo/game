#!/usr/bin/env bash
# Build PC + Switch and deploy to Switch via nxlink over LAN.
# Usage: ./tools/build_deploy_switch_lan.sh [SWITCH_IP]
# Default IP: 192.168.2.109

set -e

SWITCH_IP="${1:-192.168.2.109}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(dirname "$SCRIPT_DIR")"
cd "$REPO_DIR"

echo "=== Fetching audio (if missing) ==="
if [ ! -f assets/audio/sfx_hit_melee.wav ]; then
    python3 tools/fetch_audio.py
fi

echo "=== Generating music (if missing) ==="
if [ ! -f assets/audio/music_tier1.ogg ]; then
    python3 tools/gen_audio.py --music-all
fi

echo "=== Cleaning stale music WAVs ==="
for wav in assets/audio/music_tier*.wav; do
    [ -f "$wav" ] && rm "$wav"
done

echo "=== Building PC ==="
cmake --build build

echo "=== Building Switch (docker devkitpro) ==="
docker run --rm -u "$(id -u):$(id -g)" \
    -v "$(pwd)":/game -w /game \
    devkitpro/devkita64 bash -c \
    "source /opt/devkitpro/switchvars.sh && \
     rm -f build-switch/DungeonEngine.nro \
           build-switch/src/DungeonEngine.elf \
           build-switch/src/CMakeFiles/DungeonEngine.dir/audio/audio.cpp.obj && \
     cmake --build build-switch"

echo "=== Deploying to Switch at $SWITCH_IP ==="
docker run --rm --network host \
    -v "$(pwd)":/game \
    devkitpro/devkita64 \
    nxlink -s /game/build-switch/DungeonEngine.nro -a "$SWITCH_IP"

echo "=== Done ==="
