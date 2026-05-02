#!/bin/bash
# Fetches SDL2 as a git submodule. Retries on network failure.
# Run from the game/ root directory.

MAX_RETRIES=5
RETRY_DELAY=10

cd "$(dirname "$0")"

# Clean up any previous failed attempt
rm -rf external/SDL2
git submodule deinit -f external/SDL2 2>/dev/null
git rm --cached external/SDL2 2>/dev/null
rm -f .gitmodules

for i in $(seq 1 $MAX_RETRIES); do
    echo "Attempt $i/$MAX_RETRIES: Cloning SDL2..."
    git submodule add -b SDL2 --depth 1 https://github.com/libsdl-org/SDL.git external/SDL2 && {
        echo "SDL2 cloned successfully."
        exit 0
    }
    echo "Failed. Cleaning up and retrying in ${RETRY_DELAY}s..."
    rm -rf external/SDL2
    git submodule deinit -f external/SDL2 2>/dev/null
    git rm --cached external/SDL2 2>/dev/null
    rm -f .gitmodules
    sleep $RETRY_DELAY
done

echo "All attempts failed. Check your network connection and try again."
exit 1
