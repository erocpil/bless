# Parsing Subsystem

BLESS uses a dedicated parsing layer (`src/parse.c`, `include/bless_parse.h`)
for IP address, port, and IPv6 range parsing.  This layer is pure C with no
DPDK, civetweb, or hardware dependencies, enabling unit testing and fuzzing
without DPDK linkage.

---

## 1. Address Syntax

### Base+N Range

```
172.16.0.1+10     ->  base IP = 172.16.0.1, range = 10
10000+100         ->  base port = 10000, range = 100
```

The `+N` suffix specifies a range of N consecutive values starting from the
base.  A bare value without `+` (e.g. `172.16.0.1` or `8080`) is equivalent to
`+0` -- a single value with range=0.

### Array Syntax

```
[ 10.0.0.1, 20.0.0.2, 30.0.0.3 ]
[ 80, 443, 8080 ]
```

Comma-separated list of explicit values.  The array syntax is parsed by the
YAML layer; the parse functions themselves handle only the `base+N` format.

### IP:VNI Syntax (VXLAN only)

```
172.16.0.1:100+10  ->  outer source IP = 172.16.0.1, base VNI = 100, range = 10
```

The outer source IP for VXLAN supports an optional `:VNI` suffix.  The
destination IP is pure IP only.

---

## 2. API

```c
// Parse "8080" or "8080+10"
int bless_parse_port_range(const char *data, uint16_t *port, int32_t *range);

// Parse "172.16.0.1" or "172.16.0.1+10"
int bless_parse_ip_range(const char *data, uint32_t *ip, int64_t *range);

// Parse "2001:db8::1" or "2001:db8::1+100"
int bless_parse_ipv6_range(const char *data, uint8_t addr[16], int64_t *range);
```

All functions return 0 on success, -1 on parse failure.  IP addresses are
returned in network byte order.

---

## 3. Design: Pure-C Testability

The parse functions are deliberately separated from the DPDK-dependent config
and bless modules.  This means:

- **Unit tests link the real code, not copies.**  `test/unit/test_core.c`
  includes `bless_parse.h` and links `src/parse.c`.  The IPv6 tests
  (`test_ipv6.c`) do the same.
- **Fuzz targets can link the same implementation.**  `oss-fuzz/` targets
  compile `parse.c` directly.
- **Static analysis reaches one implementation.**  There is no stale copy in
  test code that diverges from production.
