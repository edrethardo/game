#!/usr/bin/env bash
# Build PC + Switch and deploy to Switch via nxlink over LAN.
# Usage: ./tools/build_deploy_switch_lan.sh [SWITCH_IP]
# Default IP: 192.168.2.109

set -e

SWITCH_IP="${1:-192.168.2.109}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(dirname "$SCRIPT_DIR")"
cd "$REPO_DIR"

echo "=== Fetching audio ==="
python3 tools/fetch_audio.py

echo "=== Compressing music to OGG (if WAV present) ==="
for wav in assets/audio/music_tier*.wav; do
    [ -f "$wav" ] || continue
    ogg="${wav%.wav}.ogg"
    [ -f "$ogg" ] && continue
    echo "  $wav → $ogg"
    ffmpeg -y -i "$wav" -c:a libvorbis -q:a 4 "$ogg" && rm "$wav"
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
