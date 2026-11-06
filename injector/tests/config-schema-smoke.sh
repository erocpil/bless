#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
binary=${1:-"$repo_root/injector/build/release-static/bin/injector"}
config="$repo_root/injector/conf/config-smoke-pcap.yaml"
tmp=$(mktemp -d "${TMPDIR:-/tmp}/injector-schema-smoke.XXXXXX")
trap 'rm -rf -- "$tmp"' EXIT HUP INT TERM

if ! (cd "$tmp" && "$binary" "$config" >run.log 2>&1); then
	cat "$tmp/run.log"
	echo "injector failed to load the Bless-compatible smoke config" >&2
	exit 1
fi

if [ ! -s "$tmp/tx.pcap" ]; then
	cat "$tmp/run.log"
	echo "injector did not create the TX PCAP" >&2
	exit 1
fi

# A PCAP global header is 24 bytes; 32 packet records need at least another
# 16-byte record header each, even before packet data.
pcap_bytes=$(wc -c < "$tmp/tx.pcap")
if [ "$pcap_bytes" -lt 536 ]; then
	echo "PCAP output is too small for 32 packets: $pcap_bytes bytes" >&2
	exit 1
fi

sed 's/^bless:/application:/' "$config" > "$tmp/legacy.yaml"
expect_rejected() {
	config_path=$1
	expected=$2
	label=$3
	output="$tmp/$label.log"
	if "$binary" "$config_path" > "$output" 2>&1; then
		echo "$label unexpectedly succeeded" >&2
		exit 1
	fi
	if ! grep -Fq "$expected" "$output"; then
		cat "$output"
		echo "$label failed for an unexpected reason" >&2
		exit 1
	fi
}

expect_rejected "$tmp/legacy.yaml" "no bless config found" legacy-schema

sed 's/mode: tx-only/mode: rx-only/' "$config" > "$tmp/rx-only.yaml"
expect_rejected "$tmp/rx-only.yaml" "supports only mode: tx-only" unsupported-mode

sed '/payload: "injector schema smoke test"/a\          proto: 17' \
	"$config" > "$tmp/unsupported-proto.yaml"
expect_rejected "$tmp/unsupported-proto.yaml" \
	"unsupported config setting: proto" unsupported-proto

awk '{ print; if ($0 == "system:") print "  server:\n    enable: true" }' \
	"$config" > "$tmp/server-enabled.yaml"
expect_rejected "$tmp/server-enabled.yaml" \
	"does not support system.server.enable: true" unsupported-server

echo "injector schema smoke test passed (32 TX packets captured; legacy and unsupported config rejected)"
