# QUIC v1 Protocol Constructor

QUIC v1 (RFC 9000) packet generator for entropy injection into QUIC-aware
gateways and load balancers.

## Scope

Produces valid **QUIC Initial packets** carrying a minimal TLS 1.3 ClientHello
in a CRYPTO frame.  Packets are parseable by Wireshark and tshark.

Runs over UDP (Ethernet type 0x0800, IP protocol 17).  Registered as a bless
extension plugin -- zero framework changes required.

## Configuration

QUIC is configured under `bless.ether.type.ipv4.quic`:

```yaml
bless:
  ether:
    type:
      ipv4:
        udp:
          src: 443+4
          dst: 443
        quic:
          dcid-len: 8     # Destination Connection ID length (0-20, default 8)
          scid-len: 8     # Source Connection ID length (0-20, default 8)
          version: 1      # QUIC version (0 = QUIC v1 default 0x00000001)
```

Weights are set under `injector` like any other protocol:

```yaml
injector:
  quic: 10    # 10x weight in the distribution
```

CLI override: `bless config.yaml -- --quic=50`

## Wire Format

Each packet contains:

```
Ethernet (14) / IPv4 (20) / UDP (8) / QUIC Long Header / CRYPTO frame / TLS ClientHello
```

The TLS ClientHello is a fixed template with pre-computed lengths:

| Field | Value |
|-------|-------|
| TLS record version | TLS 1.0 (0x0301, QUIC convention) |
| Legacy version | TLS 1.2 (0x0303) |
| Cipher suite | TLS_AES_128_GCM_SHA256 (0x1301) |
| SNI | `bless.local` |
| Supported versions | TLS 1.3 (0x0304) |
| Key share | x25519 (32 zero bytes placeholder) |
| QUIC transport params | Empty (valid for Initial) |

## Anomaly Mutations

Three QUIC-specific mutations are available through the erroneous injection
framework (YAML `erroneous.class.quic`):

| Mutation | YAML key | What it does |
|----------|----------|--------------|
| Version anomaly | `version` | Replaces QUIC version with 0xDEADBEEF, triggering version-negotiation paths |
| CID length anomaly | `cid_len` | Sets DCID or SCID length to 255 (> RFC 9000 cap of 20), alternating per packet |
| Token length anomaly | `token_len` | Injects a non-zero Token length (32) into Initial packets, causing downstream parse errors |

Usage:

```yaml
bless:
  erroneous:
    ratio: 400        # ~39 % mutation rate
    class:
      quic: [ version, cid_len, token_len ]
```

All three mutations operate in-place on already-constructed mbufs -- no packet
rebuild needed.  Each modifies specific byte offsets in the QUIC long header
(past Eth + IPv4 + UDP headers).

## Limitations

- **Initial packets only.**  No Handshake, 0-RTT, or 1-RTT packets.
- **Fixed TLS template.**  The ClientHello is a constant byte array, not
  dynamically generated.  All packets share the same Connection IDs
  (deterministic per-length pattern) and the same TLS random.
- **No state machine.**  QUIC is treated as opaque payload above UDP.
- **UDP port sharing.**  QUIC reuses the UDP port ranges configured under
  `ipv4.udp`.  Dedicated QUIC port config may be added in the future.

## Integration

QUIC is registered via the bless plugin system (`bless_plugin.h`):

- Packet type name: `"quic"`
- Auto-assigned type index (5, after built-in arp/icmp/tcp/udp/sctp)
- Config parser: `bless_cfg.h` generic field framework
- All standard bless features apply: distribution weighting, VXLAN
  encapsulation, MTU trimming, entropy measurement.

## Verification

```bash
# Generate 4 QUIC packets
bless --vdev net_pcap0,tx_pcap=/tmp/quic.pcap conf/quic-test.yaml

# Inspect
tcpdump -r /tmp/quic.pcap            # "UDP, length 155"
tshark -r /tmp/quic.pcap -V          # Shows QUIC IETF dissection
```

Expected output: `tcpdump` shows UDP port 443 traffic.
Wireshark/tshark decode the QUIC long header, version 1, Initial packet type,
CRYPTO frame, and TLS ClientHello with SNI `bless.local`.
