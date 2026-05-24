#!/usr/bin/env bash
#
# tools/release.sh — bump the game version, commit it, tag it, and optionally
# push so the CI/CD pipeline cuts a release.
#
# How releases work here: .github/workflows/build.yml triggers on pushing a git
# tag matching 'v*'. It builds Windows/Linux/macOS, and the `release` job then
# publishes a GitHub Release with those artifacts. So "cut a release" =
# bump the version + push a v<X.Y.Z> tag.
#
# Source of truth for the released version is the latest v* git tag (CI keys off
# tags, so that's what actually ships). This script reads that, bumps it, and
# re-syncs the two committed human-facing version strings to match:
#   - src/CMakeLists.txt          : Switch NACP metadata (3rd quoted arg to nacptool)
#   - .github/workflows/build.yml : <CFBundleVersion> in the generated macOS Info.plist
#
# Usage:
#   tools/release.sh [patch|minor|major|X.Y.Z]   # bump level (default: patch)
#   tools/release.sh patch --push                # also push branch + tag (triggers CI)
#   tools/release.sh 1.0.0                        # set an explicit version
#
set -euo pipefail

# Always operate from the repo root regardless of where we're invoked.
cd "$(git rev-parse --show-toplevel)"

CMAKE_FILE="src/CMakeLists.txt"
WORKFLOW_FILE=".github/workflows/build.yml"

# --- parse args ------------------------------------------------------------
bump="patch"
do_push=0
for arg in "$@"; do
    case "$arg" in
        patch|minor|major)    bump="$arg" ;;
        --push)               do_push=1 ;;
        [0-9]*.[0-9]*.[0-9]*) bump="$arg" ;;   # explicit X.Y.Z
        *) echo "error: unknown argument '$arg'" >&2; exit 1 ;;
    esac
done

# --- determine the current version from the latest v* tag ------------------
# Tags are the release source of truth. Refresh from the remote first so we
# never reuse/collide with an already-published tag (offline: keep going).
git fetch --tags --quiet origin 2>/dev/null || true
latest_tag="$(git tag -l 'v*' | sort -V | tail -1)"
if [[ -n "$latest_tag" ]]; then
    current="${latest_tag#v}"
else
    # No tags yet — fall back to the version string in the CMake file.
    current="$(grep -oE '"[0-9]+\.[0-9]+\.[0-9]+"' "$CMAKE_FILE" | head -1 | tr -d '"')"
fi
if [[ -z "$current" ]]; then
    echo "error: could not determine current version (no v* tag, none in $CMAKE_FILE)" >&2
    exit 1
fi

# --- compute the new version ----------------------------------------------
if [[ "$bump" == *.*.* ]]; then
    new="$bump"                                  # explicit version requested
else
    IFS='.' read -r vmajor vminor vpatch <<< "$current"
    case "$bump" in
        major) vmajor=$((vmajor + 1)); vminor=0; vpatch=0 ;;
        minor) vminor=$((vminor + 1)); vpatch=0 ;;
        patch) vpatch=$((vpatch + 1)) ;;
    esac
    new="${vmajor}.${vminor}.${vpatch}"
fi

echo "Version: $current -> $new"

if git rev-parse -q --verify "refs/tags/v$new" >/dev/null; then
    echo "error: tag v$new already exists." >&2
    exit 1
fi

# --- require a clean tree so the tag captures exactly the version bump ------
if ! git diff --quiet || ! git diff --cached --quiet; then
    echo "error: working tree has uncommitted changes — commit or stash them first." >&2
    exit 1
fi

# --- rewrite the version in both files (anchored so we hit only the right token)
sed -i -E "s|(\"ed_rethardo\" )\"[0-9]+\.[0-9]+\.[0-9]+\"|\1\"$new\"|" "$CMAKE_FILE"
sed -i -E "s|(<key>CFBundleVersion</key><string>)[0-9]+\.[0-9]+\.[0-9]+(</string>)|\1$new\2|" "$WORKFLOW_FILE"

# --- commit + annotated tag ------------------------------------------------
git add "$CMAKE_FILE" "$WORKFLOW_FILE"
git commit -m "chore: release v$new"
git tag -a "v$new" -m "v$new"
echo "Committed version bump and created tag v$new."

# --- push (only with --push; pushing the tag triggers a public CI release) -
branch="$(git rev-parse --abbrev-ref HEAD)"
if [[ "$do_push" == 1 ]]; then
    git push origin "$branch"
    git push origin "v$new"
    echo "Pushed $branch + tag v$new — CI will build and publish the release."
else
    echo
    echo "Not pushed. To trigger CI/CD, run:"
    echo "    git push origin $branch && git push origin v$new"
fi
