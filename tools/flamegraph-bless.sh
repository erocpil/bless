#!/bin/bash
# flamegraph-bless.sh — capture a perf flame graph for a bless run.
#
# Usage:
#   tools/flamegraph-bless.sh [OPTIONS] <config.yaml>
#
# Options:
#   -f FREQ     Sampling frequency in Hz (default: 99)
#   -d SEC      Recording duration in seconds (default: 10)
#   -w SEC      Warmup delay after bless starts (default: 5)
#   -o FILE     Output SVG path (default: /tmp/bless_flamegraph_<ts>.svg)
#   -t TITLE    Flame graph title (default: "bless: <config>")
#   -k          Keep perf.data after exit (default: remove)
#
# The script starts bless with the given config, waits for warmup, attaches
# perf record -g --call-graph dwarf for the specified duration, then generates
# a flame graph SVG and cleans up.
#
# Requirements:
#   - perf (Linux perf_event)
#   - Perl (for stackcollapse-perf.pl and flamegraph.pl)
#   - bless binary at build/release-static/bin/bless

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BLESS_BIN="${SCRIPT_DIR}/../build/release-static/bin/bless"
FG_DIR="${SCRIPT_DIR}/flamegraph"
STACKCOLLAPSE="${FG_DIR}/stackcollapse-perf.pl"
FLAMEGRAPH="${FG_DIR}/flamegraph.pl"

# ── defaults ──────────────────────────────────────────────────────────
FREQ=99
DURATION=10
WARMUP=5
OUTPUT=""
TITLE=""
KEEP_PERF=0
METRICS_PORT=8000

# ── parse args ────────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        -f) FREQ="$2"; shift 2 ;;
        -d) DURATION="$2"; shift 2 ;;
        -w) WARMUP="$2"; shift 2 ;;
        -o) OUTPUT="$2"; shift 2 ;;
        -t) TITLE="$2"; shift 2 ;;
        -k) KEEP_PERF=1; shift ;;
        -h|--help)
            sed -n '2,19p' "$0"
            exit 0
            ;;
        -*) echo "Unknown option: $1"; exit 1 ;;
        *)  CONFIG="$1"; shift ;;
    esac
done

if [[ -z "${CONFIG:-}" ]]; then
    echo "ERROR: config file required"
    sed -n '2,19p' "$0"
    exit 1
fi

if [[ ! -f "$CONFIG" ]]; then
    echo "ERROR: config file not found: $CONFIG"
    exit 1
fi

if [[ ! -x "$BLESS_BIN" ]]; then
    echo "ERROR: bless binary not found at $BLESS_BIN — build first with: make -j"
    exit 1
fi

if ! command -v perf &>/dev/null; then
    echo "ERROR: perf not found"
    exit 1
fi

if ! command -v perl &>/dev/null; then
    echo "ERROR: perl not found (required by FlameGraph tools)"
    exit 1
fi

# ── output path ───────────────────────────────────────────────────────
TS="$(date +%Y%m%d_%H%M%S)"
PERF_DATA="/tmp/perf_bless_${TS}.data"
COLLAPSED="/tmp/perf_bless_${TS}.collapsed"
SVG="${OUTPUT:-/tmp/bless_flamegraph_${TS}.svg}"
TITLE="${TITLE:-bless: $(basename "$CONFIG")}"

# ── clean up stale processes ──────────────────────────────────────────
pkill -9 bless 2>/dev/null || true
sleep 1

# ── start bless ───────────────────────────────────────────────────────
echo "=== Starting bless with $CONFIG ==="
"$BLESS_BIN" "$CONFIG" &
BLESS_PID=$!

# Wait for /metrics endpoint
echo "=== Waiting for /metrics (up to ${WARMUP}s) ==="
DEADLINE=$((SECONDS + WARMUP))
while [[ $SECONDS -lt $DEADLINE ]]; do
    if curl -s "http://127.0.0.1:${METRICS_PORT}/metrics" 2>/dev/null \
        | grep -q 'dpdk_opackets'; then
        echo "  /metrics ready after $((WARMUP - (DEADLINE - SECONDS)))s"
        break
    fi
    sleep 0.5
done

if ! kill -0 "$BLESS_PID" 2>/dev/null; then
    echo "ERROR: bless exited before /metrics was ready"
    exit 1
fi

# Extra warmup for packet flow to stabilise
sleep 2

# ── perf record ───────────────────────────────────────────────────────
echo "=== Recording perf for ${DURATION}s @ ${FREQ}Hz ==="
perf record \
    -F "$FREQ" \
    -g --call-graph dwarf \
    -p "$BLESS_PID" \
    -o "$PERF_DATA" \
    -- sleep "$DURATION" 2>&1

if [[ ! -f "$PERF_DATA" ]]; then
    echo "ERROR: perf did not produce output"
    kill "$BLESS_PID" 2>/dev/null || true
    exit 1
fi

SAMPLES=$(perf report -i "$PERF_DATA" --stdio --header-only 2>/dev/null \
    | grep -oP 'Samples:\s+\K\d+' || echo "?")
echo "  captured ${SAMPLES:-?} samples, $(du -h "$PERF_DATA" | cut -f1)"

# ── kill bless ────────────────────────────────────────────────────────
echo "=== Stopping bless ==="
kill "$BLESS_PID" 2>/dev/null || true
wait "$BLESS_PID" 2>/dev/null || true

# ── generate flame graph ──────────────────────────────────────────────
echo "=== Generating flame graph ==="
perf script -i "$PERF_DATA" 2>/dev/null \
    | "$STACKCOLLAPSE" \
    > "$COLLAPSED"

"$FLAMEGRAPH" \
    --title "$TITLE" \
    --width 1200 \
    "$COLLAPSED" \
    > "$SVG" 2>/dev/null

echo "  SVG: $SVG ($(du -h "$SVG" | cut -f1))"

# ── cleanup ───────────────────────────────────────────────────────────
rm -f "$COLLAPSED"
if [[ $KEEP_PERF -eq 0 ]]; then
    rm -f "$PERF_DATA"
    echo "  (perf data removed; use -k to keep)"
else
    echo "  perf data: $PERF_DATA"
fi

echo "=== Done ==="
