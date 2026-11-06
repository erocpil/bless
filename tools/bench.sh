#!/bin/bash
# bench.sh — Automated BLESS benchmark runner
#
# Collects PPS throughput across multiple frame-size profiles and
# rate-limited scenarios.  Requires root (DPDK hugepages) and a
# DPDK-compatible NIC.
#
# Usage:
#   sudo ./tools/bench.sh --port 0x1 --lcores 0-1 --runtime 30
#
# Output: bench-YMD-HMS.csv + bench-YMD-HMS.log
set -euo pipefail

BIN="${BLESS_BIN:-./build/release-static/bin/bless}"
CONF_DIR="./conf"
RUNTIME=30
PORT_MASK="0x1"
LCORES="0-1"
OUT_CSV=""
OUT_LOG=""
PROFILES=("bench-udp64.yaml" "bench-imix.yaml")

usage() {
    cat <<EOF
Usage: $0 [options]

Options:
  --port MASK        Port mask (default: 0x1)
  --lcores LIST      Lcore list for DPDK (default: 0-1)
  --runtime SEC      Test duration per profile (default: 30)
  --bin PATH         Path to bless binary (default: ./build/release-static/bin/bless)
  -h, --help         Show this help

Profiles: ${PROFILES[*]}
EOF
    exit 0
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --port) PORT_MASK="$2"; shift 2 ;;
        --lcores) LCORES="$2"; shift 2 ;;
        --runtime) RUNTIME="$2"; shift 2 ;;
        --bin) BIN="$2"; shift 2 ;;
        -h|--help) usage ;;
        *) echo "Unknown: $1"; usage ;;
    esac
done

TS=$(date +%Y%m%d-%H%M%S)
OUT_CSV="bench-${TS}.csv"
OUT_LOG="bench-${TS}.log"

echo "=== BLESS Benchmark $(date -Iseconds) ===" | tee "$OUT_LOG"
echo "Binary: $BIN" | tee -a "$OUT_LOG"
echo "lcores: $LCORES  port: $PORT_MASK  runtime: ${RUNTIME}s" | tee -a "$OUT_LOG"
echo "" | tee -a "$OUT_LOG"

# Header
echo "profile,frame_size,seed,pps_rate_target,achieved_mpps,opackets,oerrors,sampler_dropped" > "$OUT_CSV"

run_one() {
    local profile="$1"
    local seed="$2"
    local pps_rate="$3"   # 0 = unlimited
    local conf="$CONF_DIR/$profile"

    if [[ ! -f "$conf" ]]; then
        echo "SKIP: config not found: $conf" | tee -a "$OUT_LOG"
        return
    fi

    local label="${profile}-seed${seed}-pps${pps_rate}"

    echo "--- $label ---" | tee -a "$OUT_LOG"

    # Start bless in background
    "$BIN" -l "$LCORES" -n 2 --socket-mem 1024,0 -- \
        --port "$PORT_MASK" --mode tx-only --num -1 \
        --pps-rate "$pps_rate" --seed "$seed" \
        --config "$conf" \
        > /tmp/bless-bench.log 2>&1 &
    local pid=$!

    sleep 2  # wait for EAL init

    # Wait for runtime, collecting metrics every 2s
    local end_ts=$((SECONDS + RUNTIME))
    local samples=()
    while [[ $SECONDS -lt $end_ts ]]; do
        sleep 2
        if ! kill -0 "$pid" 2>/dev/null; then
            echo "FAIL: bless exited early" | tee -a "$OUT_LOG"
            cat /tmp/bless-bench.log >> "$OUT_LOG"
            return 1
        fi
        local m=$(curl -s --noproxy '*' http://127.0.0.1:8000/metrics 2>/dev/null || true)
        local opackets=$(echo "$m" | grep -oP 'dpdk_opackets\{[^}]*\}\s+\K\d+' | awk '{s+=$1} END{print s}')
        local oerrors=$(echo "$m" | grep -oP 'dpdk_oerrors\{[^}]*\}\s+\K\d+' | awk '{s+=$1} END{print s}')
        local dropped=$(echo "$m" | grep -oP 'bless_sampler_dropped \K\d+' | head -1)
        samples+=("$opackets $oerrors $dropped")
        echo "  t=$((SECONDS - (end_ts - RUNTIME)))s  opackets=$opackets  oerrors=$oerrors  dropped=$dropped" | tee -a "$OUT_LOG"
    done

    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    sleep 1

    # Compute achieved PPS from last two samples (rate)
    local n=${#samples[@]}
    if [[ $n -lt 2 ]]; then
        echo "WARN: too few samples for $label" | tee -a "$OUT_LOG"
        echo "$profile,$pps_rate,$seed,0,0,0,0" >> "$OUT_CSV"
        return
    fi

    local last_ops=$(echo "${samples[$((n-1))]}" | awk '{print $1}')
    local prev_ops=$(echo "${samples[$((n-2))]}" | awk '{print $1}')
    local last_errs=$(echo "${samples[$((n-1))]}" | awk '{print $2}')
    local last_drop=$(echo "${samples[$((n-1))]}" | awk '{print $3}')
    local delta_ops=$((last_ops - prev_ops))
    local mpps=$(echo "scale=3; $delta_ops / 2000000" | bc)  # 2s interval

    echo "RESULT: $label  mpps=$mpps  opackets=$last_ops  oerrors=$last_errs  dropped=$last_drop" | tee -a "$OUT_LOG"
    echo "$profile,$pps_rate,$seed,$mpps,$last_ops,$last_errs,$last_drop" >> "$OUT_CSV"
}

# Run through profiles and rate targets
for profile in "${PROFILES[@]}"; do
    for seed in 0; do
        for pps_rate in 0; do
            run_one "$profile" "$seed" "$pps_rate"
        done
    done
done

echo "" | tee -a "$OUT_LOG"
echo "=== Done ===" | tee -a "$OUT_LOG"
echo "CSV: $OUT_CSV" | tee -a "$OUT_LOG"
echo "Log: $OUT_LOG" | tee -a "$OUT_LOG"
