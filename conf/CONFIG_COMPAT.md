# Bless CLI ↔ YAML Compatibility Reference
#
# Every CLI option has a YAML equivalent under the injector: section,
# and vice versa.  This table documents the exact mapping.

# ═══════════════════════════════════════════════════════════════
# 1. SHORT OPTIONS (single-letter)
# ═══════════════════════════════════════════════════════════════
#
# -p PORTMASK   → injector → p: PORTMASK       Port bitmap
# -P            → injector → P: ""              Promiscuous mode (supply empty string for flag)
# -q N          → injector → q: N               Number of queues
# -T SECONDS    → injector → T: SECONDS         Stats timer period

# ═══════════════════════════════════════════════════════════════
# 2. LONG OPTIONS → injector:
# ═══════════════════════════════════════════════════════════════
#
# --no-mac-updating   → injector → no-mac-updating: ""
# --portmap=UUID      → injector → portmap: UUID
# --auto-start=BOOL   → injector → auto-start: true|false
# --mode=MODE         → injector → mode: tx-only|rx-only|fwd|handshake
# --num=N             → injector → num: -1|N
# --batch=N           → injector → batch: N
# --batch-delay-us=N  → injector → batch-delay-us: N
# --batch-jitter-us=N → injector → batch-jitter-us: N
# --sample-interval=N → injector → sample-interval: N
# --arp=N             → injector → arp: N       (protocol weight)
# --icmp=N            → injector → icmp: N
# --tcp=N             → injector → tcp: N
# --udp=N             → injector → udp: N
# --hs-rate=N         → injector → hs-rate: N
# --hs-timeout-us=N   → injector → hs-timeout-us: N
# --hs-mix-ratio=N    → injector → hs-mix-ratio: N
# --pps-rate=N        → injector → pps-rate: N  (NEW in token-bucket)
# --pps-burst=N       → injector → pps-burst: N (NEW)
# --bps-rate=N        → injector → bps-rate: N  (NEW)
# --bps-burst=N       → injector → bps-burst: N (NEW)

# ═══════════════════════════════════════════════════════════════
# 3. YAML-ONLY SECTIONS (no CLI equivalent)
# ═══════════════════════════════════════════════════════════════
#
# dpdk:     DPDK EAL arguments (vdev, no-pci, lcore mask, etc.)
#
# bless:    Packet construction recipe (MAC, IP, ports, VXLAN, IMIX, erroneous, etc.)
#           Entire section is YAML-only, parsed by config_parse_bless*().
#
# system:   HTTP/WebSocket server, daemonize, theme.
#           Parsed by config_parse_system() / config_parse_server().
#           The HTTP endpoints (/metrics, /api/stats, /wsURL) are YAML-only.

# ═══════════════════════════════════════════════════════════════
# 4. EXAMPLE: CLI command ↔ YAML equivalence
# ═══════════════════════════════════════════════════════════════
#
# CLI:
#   bless --no-pci -a --vdev=net_pcap0,iface=lo -l 0-2 -n 2 \
#         -- -p 0x1 -q 1 --mode=tx-only --num=-1 --batch=64 \
#         --pps-rate=10000 --pps-burst=512 --sample-interval=1 \
#         --arp=1 --icmp=2 --tcp=8 --udp=9
#
# Equivalent YAML (conf/config-rate-pps.yaml):
#   dpdk:
#     no-pci: ""
#     a: null
#     vdev: net_pcap0,iface=lo
#     l: 0-2
#     n: 2
#   injector:
#     p: 0x1
#     q: 1
#     mode: tx-only
#     num: -1
#     batch: 64
#     pps-rate: 10000
#     pps-burst: 512
#     sample-interval: 1
#     arp: 1
#     icmp: 2
#     tcp: 8
#     udp: 9
#   bless:
#     ... (YAML-only: MAC, IP ranges, VXLAN, erroneous, IMIX)
#   system:
#     ... (YAML-only: HTTP server)

# ═══════════════════════════════════════════════════════════════
# 5. LIMITATIONS
# ═══════════════════════════════════════════════════════════════
#
# - Flag options (no-mac-updating, -P) in YAML need empty string:
#     no-mac-updating: ""
#     P: ""
#
# - Sequence values (arrays) in injector are joined without separator,
#   so arrays only work for options that expect comma-separated values.
#   In practice, injector arrays are rare.
#
# - The bless: section cannot be expressed as CLI args at all.
#   IMIX, VXLAN, erroneous, IP range syntax (10.0.0.1+65536) are YAML-only.
#
# - DPDK single-char options (a, l, n) under dpdk: are mapped to
#   short flags by key length detection in config_parse_dpdk_internal().
