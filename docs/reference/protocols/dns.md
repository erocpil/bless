# Application-Layer Protocol Constructors

Bless supports DNS, NTP, and HTTP as extension protocol types via the
plugin system.  Each protocol registers its own packet constructor and
YAML config subtree.

## DNS (UDP/53)

Generates DNS query packets with random Transaction IDs, rotating
domain names, and mixed query types.  Suitable for DNS DPI stress
testing, recursive resolver load testing, and authoritative server
throughput measurement.

### YAML Configuration

```yaml
bless:
  dns:
    src: "1024+65535"     # source port range
    dst: "53"             # destination port (DNS)
    names:
      - "www.google.com"
      - "api.github.com"
      - "cdn.cloudflare.net"
    qtypes: [A, AAAA, MX, ANY]
```

### Fields

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `src` | port range | 1024+65535 | Source UDP port |
| `dst` | port range | 53 | Destination UDP port |
| `names` | string array | www.example.com, api.example.org, cdn.example.net | Domain names (dot-separated, auto-encoded to DNS wire format) |
| `qtypes` | string array | [A] | Query types: A, NS, CNAME, SOA, MX, TXT, AAAA, SRV, ANY, or numeric 1-65535 |

### Wireshark Verification

Filter: `dns`.  Each packet should show a valid DNS query with
randomised Transaction ID and QTYPE.

### Entropy Dimensions

- Transaction ID (16-bit random, 16 bits max entropy)
- QNAME (variable, up to ~20 bits per configured name list)
- QTYPE (1-8 bits depending on diversity)

## NTP (UDP/123)

Generates NTP client/server packets with randomised timestamps,
stratum, and poll interval.  Four 64-bit timestamps provide
high-entropy payload suitable for testing NTP protocol parsing,
amplification detection, and stateful firewall session tracking.

### YAML Configuration

```yaml
bless:
  ntp:
    src: "1024+65535"
    dst: "123"
    modes: [3, 4]        # 3=client, 4=server
    versions: [3, 4]
```

### Fields

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `src` | port range | 1024+65535 | Source UDP port |
| `dst` | port range | 123 | Destination UDP port (NTP) |
| `modes` | string array | [3, 4] | NTP modes: 3=client, 4=server |
| `versions` | string array | [4] | NTP versions: 3, 4 |

### Wireshark Verification

Filter: `ntp`.  Verify `li_vn_mode`, `stratum`, and `xmit_ts` vary
across packets.

## HTTP (TCP/80)

Generates minimal HTTP/1.1 GET requests with rotating methods,
URI paths, and Host headers.  Designed for L7 DPI throughput testing
of gateways, load balancers, and WAFs.

### YAML Configuration

```yaml
bless:
  http:
    src: "1024+65535"
    dst: "80"
    methods: "GET,POST,HEAD"
    paths: "/,/api/v1,/index.html,/health"
    hosts: "example.com,api.example.org,cdn.example.net"
```

### Fields

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `src` | port range | 1024+65535 | Source TCP port |
| `dst` | port range | 80 | Destination TCP port (HTTP) |
| `methods` | string (CSV) | GET | HTTP methods, comma-separated |
| `paths` | string (CSV) | / | URI paths, comma-separated |
| `hosts` | string (CSV) | example.com | Host header values, comma-separated |

### Wireshark Verification

Filter: `http.request`.  Each packet should show a well-formed HTTP/1.1
request with varying method, URI, and Host header.

## Weight Configuration

Protocol weights control the proportion of packets in the distribution:

```bash
# CLI
bless ... --dns 30 --ntp 20 --http 10 --udp 40

# WebSocket
{"cmd":"set","key":"weight","value":"dns:30,ntp:20,http:10,udp:40"}
```

## Building

No special build flags required -- DNS/NTP/HTTP are compiled in by
default as extension plugins.  Verify:

```bash
bless help | grep -E "dns|ntp|http"
```

## Known Limitations

- DNS: Wire-format names must be < 255 bytes total.  Labels are limited
  to 63 bytes each.  No EDNS0 / DNSSEC extensions.
- NTP: Only client (mode 3) and server (mode 4) modes are implemented.
  No NTP authentication (RFC 8915).
- HTTP: Payload is regenerated per-packet (not per-batch) for maximum
  URI entropy, which adds ~200 ns per packet on modern CPUs.  For
  maximum PPS, reduce the `paths` list to a single entry.

## See Also

- `entropy-theory.md` -- Application-layer entropy section
- `conf/dns-ntp-http.yaml` -- Example config
- `include/dns.h`, `include/ntp.h`, `include/http.h` -- Extension config structs
- `src/proto_dns.c`, `src/proto_ntp.c`, `src/proto_http.c` -- Constructors
