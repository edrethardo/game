#!/usr/bin/env bash
# run_coop_local.sh — launch a host + client pair of the game locally and tee
# stdout/stderr to per-instance log files so a netcode session can be inspected
# after the fact (grep for [FIRE-CL], [FIRE-SV], [INTERP-PROJ], snap rx, etc.).
#
# Usage:
#   tools/run_coop_local.sh            # launch host then client, wait for both
#   tools/run_coop_local.sh --build    # build first, then launch
#   tools/run_coop_local.sh --tail     # also tail both logs in the foreground
#
# Notes:
#   - Run from anywhere; the script cd's to the repo root so `./assets/` is found.
#   - Both instances share the same save_*.dat in cwd. Acceptable for testing;
#     don't expect independent save states.
#   - Logs land in logs/host-<ts>.log and logs/client-<ts>.log, plus stable
#     symlinks logs/host.log and logs/client.log for easy grep.
#   - Ctrl+C kills both instances and exits.

set -euo pipefail

# --- locate repo root (one level above this script) ---------------------------
SCRIPT_DIR="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

BIN="./build/src/DungeonEngine"
DO_BUILD=0
DO_TAIL=0
for arg in "$@"; do
    case "$arg" in
        --build) DO_BUILD=1 ;;
        --tail)  DO_TAIL=1 ;;
        -h|--help)
            sed -n '2,18p' "$0"
            exit 0
            ;;
        *) echo "unknown arg: $arg" >&2; exit 2 ;;
    esac
done

if [[ "$DO_BUILD" -eq 1 ]]; then
    echo "Building DungeonEngine..."
    cmake --build build
fi

if [[ ! -x "$BIN" ]]; then
    echo "Binary not found at $BIN" >&2
    echo "Run: cmake -B build && cmake --build build" >&2
    echo "Or pass --build to this script." >&2
    exit 1
fi

mkdir -p logs
TS=$(date +%Y%m%d-%H%M%S)
HOST_LOG="logs/host-${TS}.log"
CLIENT_LOG="logs/client-${TS}.log"
ln -sf "host-${TS}.log"   logs/host.log
ln -sf "client-${TS}.log" logs/client.log

# --- cleanup: kill both children on Ctrl+C or normal exit ---------------------
HOST_PID=""
CLIENT_PID=""
cleanup() {
    echo
    echo "Stopping instances..."
    [[ -n "$HOST_PID"   ]] && kill "$HOST_PID"   2>/dev/null || true
    [[ -n "$CLIENT_PID" ]] && kill "$CLIENT_PID" 2>/dev/null || true
    wait 2>/dev/null || true
}
trap cleanup EXIT INT TERM

echo "Host log:   $HOST_LOG  (logs/host.log)"
echo "Client log: $CLIENT_LOG  (logs/client.log)"
echo

echo "Launching host..."
"$BIN" > "$HOST_LOG" 2>&1 &
HOST_PID=$!

# Brief delay so the host process binds the listen socket before the client
# tries to connect. Without this the client's first connect attempt can hit the
# pre-host window and time out (see engine_menu.cpp's M10 connect-timeout path).
sleep 1.0

echo "Launching client..."
"$BIN" > "$CLIENT_LOG" 2>&1 &
CLIENT_PID=$!

echo
echo "host pid=$HOST_PID  client pid=$CLIENT_PID"
echo "Ctrl+C to stop both instances."
echo

if [[ "$DO_TAIL" -eq 1 ]]; then
    # Stream both logs to this terminal, prefixed with [host]/[client] so the
    # two streams are distinguishable when grepping the scrollback.
    tail -F -q "$HOST_LOG" "$CLIENT_LOG" &
    TAIL_PID=$!
    trap '[[ -n "${TAIL_PID:-}" ]] && kill "$TAIL_PID" 2>/dev/null; cleanup' EXIT INT TERM
fi

wait
