#!/bin/bash
# deterministic-replay.sh — verify same seed => identical entropy
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BIN="${BLESS_BIN:-$SCRIPT_DIR/../build/release-static/bin/bless}"
CONFIG="${BLESS_CONFIG:-$SCRIPT_DIR/../conf/config-ci.yaml}"
SEED=42
NUM=50000
OUT1=$(mktemp)
OUT2=$(mktemp)
trap "rm -f $OUT1 $OUT2" EXIT

run_round() {
    local out="$1" round="$2"
    echo "=== Round $round (seed=$SEED, num=$NUM) ===" >&2
    $BIN "$CONFIG" --seed="$SEED" --num="$NUM" 2>/dev/null &
    local PID=$!

    local json=""
    for i in $(seq 1 200); do
        kill -0 $PID 2>/dev/null || break
        json=$(curl -s --noproxy '*' http://127.0.0.1:8000/api/stats 2>/dev/null || true)
        [ -n "$json" ] && [ "$json" != '{}' ] && echo "$json" > "$out"
        sleep 0.3
    done
    wait $PID 2>/dev/null || true
}

run_round "$OUT1" 1
sleep 2
run_round "$OUT2" 2

python3 "$SCRIPT_DIR/replay_compare.py" "$OUT1" "$OUT2"
