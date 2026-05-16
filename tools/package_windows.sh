#!/bin/bash
# Package the Windows cross-compiled build into a distributable zip.
#
# Usage:
#   ./tools/package_windows.sh
#
# Prerequisites:
#   - MinGW cross-compile done: cmake -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/windows.cmake -DCMAKE_BUILD_TYPE=Release && cmake --build build-win
#   - Audio fetched: python3 tools/fetch_audio.py
#
# Output: DungeonEngine-Windows.zip in the project root

set -e

GAME_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$GAME_ROOT/build-win"
PACKAGE_DIR="$GAME_ROOT/build-win/package"
EXE="$BUILD_DIR/src/DungeonEngine.exe"
ZIP_NAME="DungeonEngine-Windows.zip"

# Check exe exists
if [ ! -f "$EXE" ]; then
    echo "ERROR: $EXE not found. Run the cross-compile first:"
    echo "  cmake -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/windows.cmake -DCMAKE_BUILD_TYPE=Release"
    echo "  cmake --build build-win"
    exit 1
fi

echo "=== Packaging Windows build ==="

# Clean previous package
rm -rf "$PACKAGE_DIR"
mkdir -p "$PACKAGE_DIR"

# Copy executable
cp "$EXE" "$PACKAGE_DIR/"
echo "  Copied DungeonEngine.exe"

# Copy SDL2 DLLs (built by cmake in external/)
for dll in SDL2.dll SDL2_mixer.dll; do
    found=$(find "$BUILD_DIR" -name "$dll" -type f 2>/dev/null | head -1)
    if [ -n "$found" ]; then
        cp "$found" "$PACKAGE_DIR/"
        echo "  Copied $dll"
    fi
done

# Copy MinGW runtime DLLs that might be needed (if not statically linked)
for lib in libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll; do
    found=$(x86_64-w64-mingw32-g++ -print-file-name="$lib" 2>/dev/null)
    if [ -f "$found" ]; then
        cp "$found" "$PACKAGE_DIR/"
        echo "  Copied $lib (MinGW runtime)"
    fi
done

# Copy assets
cp -r "$GAME_ROOT/assets" "$PACKAGE_DIR/assets"
echo "  Copied assets/"

# Remove gitignored/unnecessary files from packaged assets
rm -f "$PACKAGE_DIR/assets/audio/.fetched"
rm -f "$PACKAGE_DIR/assets/config/controls.json"

echo "  Cleaned up unnecessary files"

# Create zip
cd "$GAME_ROOT"
rm -f "$ZIP_NAME"
cd "$PACKAGE_DIR"
zip -r "$GAME_ROOT/$ZIP_NAME" . -x "*.DS_Store" > /dev/null
cd "$GAME_ROOT"

SIZE=$(du -sh "$ZIP_NAME" | cut -f1)
echo ""
echo "=== Done: $ZIP_NAME ($SIZE) ==="
echo "Extract and run DungeonEngine.exe on Windows."
