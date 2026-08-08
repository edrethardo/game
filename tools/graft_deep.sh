#!/usr/bin/env bash
# graft build --deep, guaranteed to run through the no-think shim.
#
# WHY: Qwen emits a reasoning preamble that costs ~1000 tokens per card and
# truncates on `length`. Measured on this repo: 1 completion/min with thinking
# ON, 85/min through the shim — a 70x difference that turns a 20-minute build
# into an overnight one. tools/nothink_proxy.py injects
# chat_template_kwargs {"enable_thinking": false} per REQUEST, so vLLM itself
# can stay in thinking mode for everything else.
#
# The endpoint settings live in .env (graft loads it via dotenv), so a bare
# `graft build --deep` already uses the shim. This script exists for the one
# thing .env cannot do: make sure the shim is actually running first.
set -u
REPO=/home/aaron/game
SHIM=$REPO/tools/nothink_proxy.py
PORT=8011
cd "$REPO" || exit 1

if ! curl -s -m 3 -o /dev/null "http://127.0.0.1:$PORT/v1/models"; then
    echo "no-think shim not responding on :$PORT — starting it"
    # setsid: outlive the shell that launched it, so a long build is never
    # orphaned when the caller's terminal or agent session goes away.
    setsid nohup python3 "$SHIM" > /tmp/nothink_proxy.log 2>&1 < /dev/null &
    for _ in $(seq 1 20); do
        curl -s -m 2 -o /dev/null "http://127.0.0.1:$PORT/v1/models" && break
        sleep 1
    done
fi
curl -s -m 3 -o /dev/null "http://127.0.0.1:$PORT/v1/models" \
    || { echo "ERROR: shim still down — refusing to run --deep against a thinking endpoint"; exit 1; }

echo "shim up on :$PORT — starting graft build --deep $*"
exec /home/aaron/.local/bin/graft build --deep "$@"
