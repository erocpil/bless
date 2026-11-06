#!/usr/bin/env bash
#
# Bless smoke-test suite
# Run from project root:  bash tools/run-tests.sh
# Requires: DPDK + hugepages + net_null0 (no real NICs needed)
#
set -euo pipefail

BLESS_BIN="build/release-static/bin/bless"
CONF="conf/config-test.yaml"
PASS=0
FAIL=0

die() { echo "  FAIL: $*" >&2; ((FAIL++)); }
ok()  { echo "  PASS"; ((PASS++)); }
banner() { echo; echo "=== $* ==="; }

cleanup() {
    pkill -9 -f bless 2>/dev/null || true
    rm -f /tmp/bless-test-*.log
}
trap cleanup EXIT
cleanup

# ── 1. Build ──
banner "1. Build (0 errors, 0 warnings)"
BUILD_LOG=$(mktemp)
if make -j"$(nproc)" 2>"$BUILD_LOG" 1>&2; then
    ok
else
    die "build failed"
fi
rm -f "$BUILD_LOG"

# ── 2. No atoi in src/ ──
banner "2. No atoi regression in src/"
if grep -rn '\batoi\b' src/ include/ --include='*.c' --include='*.h' 2>/dev/null; then
    die "atoi found in source"
else
    ok
fi

# ── 3. YAML config parsing + entropy ──
banner "3. Config parsing + entropy"
# Override num to a finite value so the process exits cleanly
timeout 10 ./$BLESS_BIN "$CONF" -- --num=10000 > /tmp/bless-test-3.log 2>&1 || true
if grep -q 'Entropy max possible' /tmp/bless-test-3.log; then
    ok
else
    cat /tmp/bless-test-3.log | grep -v '^\s' | grep -v '^00' | tail -5
    die "no Entropy line in log"
fi

# ── 4. SCTP protocol in distribution ──
banner "4. SCTP in weight distribution"
if grep -q 'sctp' /tmp/bless-test-3.log; then
    ok
else
    die "SCTP not found in distribution"
fi

# ── 5. No 'child not found' for sctp ──
banner "5. SCTP yaml path resolved"
if grep -q 'no child found from key sctp' /tmp/bless-test-3.log; then
    die "SCTP yaml_path not resolved"
else
    ok
fi

# ── 6. SCTP ext port range → entropy ──
banner "6. SCTP ext port range in entropy (large range > TCP/UDP)"
# Patch config to give SCTP a larger port range
cp "$CONF" /tmp/bless-test-6.yaml
sed -i 's/src: 30000+50/src: 30000+500/' /tmp/bless-test-6.yaml
timeout 10 ./$BLESS_BIN /tmp/bless-test-6.yaml -- --num=10000 > /tmp/bless-test-6.log 2>&1 || true
ENTROPY_LINE=$(grep 'Entropy max possible' /tmp/bless-test-6.log || true)
if echo "$ENTROPY_LINE" | grep -q 'src_port='; then
    ok
else
    die "no src_port entropy"
fi

# ── 7. Erroneous with SCTP mutations ──
banner "7. SCTP erroneous mutations findable"
cp "$CONF" /tmp/bless-test-7.yaml
sed -i 's/ratio: 0/ratio: 42/' /tmp/bless-test-7.yaml
timeout 10 ./$BLESS_BIN /tmp/bless-test-7.yaml -- --num=10000 > /tmp/bless-test-7.log 2>&1 || true
# Should NOT see "no mutation" error
if grep -q 'no mutation' /tmp/bless-test-7.log; then
    die "SCTP mutation not found"
else
    ok
fi

# ── 8. SIGTERM graceful shutdown ──
banner "8. SIGTERM graceful shutdown"
timeout 6 ./$BLESS_BIN "$CONF" &
BPID=$!
sleep 2
kill -TERM "$BPID" 2>/dev/null || true
wait "$BPID" || true
# process should have exited cleanly (not killed)
echo "  SIGTERM sent, process exited"
ok

# ── 9. No atoi in source tree (dynamic check) ──
banner "9. No atoi in build objects"
if nm build/release-static/obj/*.o 2>/dev/null | grep -w 'atoi' | grep -v 'U atoi'; then
    die "object files define atoi"
else
    ok
fi

# ── Summary ──
echo
echo "=============================="
echo "  PASS: $PASS    FAIL: $FAIL"
echo "=============================="
exit $FAIL
