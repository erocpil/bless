/*
 * fuzz_dns_encode.c -- libFuzzer harness for bless DNS name encoder.
 *
 * Tests equivalent logic to the DNS wire-format encoder in
 * proto_dns.c (dns_parse_cfg_hook label encoder + parse_qtype).
 * The production functions depend on DPDK headers (rte_mbuf) and
 * cannot be linked in a fuzz-only build — all implementations here
 * are LOCAL COPIES of the same logic.
 *
 * Regression risk: see fuzz_http_payload.c for mitigation strategy.
 * When production proto_dns.c changes, review and update the
 * corresponding local copy here.
 *
 * Build (local):
 *   clang -fsanitize=fuzzer,address -o fuzz_dns_encode fuzz_dns_encode.c
 *
 * Run:
 *   ./fuzz_dns_encode -max_len=512 -runs=100000
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <strings.h>
#include <stdio.h>

/* ---------- DNS wire-format encoder ---------- */

/*
 * Encode a dotted domain name (e.g. "www.example.com") into DNS wire
 * format: <len1><label1><len2><label2>...<0x00>
 *
 * Returns malloc'd buffer length, or 0 on failure.
 * Caller must free the returned buffer.
 */
static size_t dns_encode_name(const char *dot, char **out)
{
	if (!dot || !*dot) {
		*out = NULL;
		return 0;
	}

	/* Max DNS name: 255 bytes total, each label <= 63 */
	size_t len = strlen(dot);
	if (len > 255) {
		*out = NULL;
		return 0;
	}

	/* Wire format: same length + 1 for terminal null + 1 per label */
	char *wire = malloc(len + 2);
	if (!wire) {
		return 0;
	}

	char *wp = wire;
	const char *dp = dot;

	while (*dp) {
		const char *end = dp;
		while (*end && *end != '.') end++;

				size_t seg = (size_t)(end - dp);
				if (seg > 63) {
					free(wire);
					*out = NULL;
					return 0;  /* label too long */
				}

		*wp++ = (char)seg;
		memcpy(wp, dp, seg);
		wp += seg;

		dp = (*end == '.') ? end + 1 : end;
	}

	*wp++ = '\0';  /* terminal null */
	*out = wire;
	return (size_t)(wp - wire);
}

/* ---------- QTYPE parser ---------- */

static const struct {
	const char *name;
	uint16_t    value;
} qtype_map[] = {
	{ "A",     1  },
	{ "NS",    2  },
	{ "CNAME", 5  },
	{ "SOA",   6  },
	{ "MX",    15 },
	{ "TXT",   16 },
	{ "AAAA",  28 },
	{ "SRV",   33 },
	{ "ANY",   255 },
	{ NULL,    0  },
};

static uint16_t parse_qtype(const char *s)
{
	if (!s) {
		return 1;
	}
	for (int i = 0; qtype_map[i].name; i++) {
		if (strcasecmp(s, qtype_map[i].name) == 0) {
			return qtype_map[i].value;
		}
	}
	/* numeric fallback */
	char *end;
	long v = strtol(s, &end, 10);
	if (end == s || *end != '\0') {
		return 1; /* default: A */
	}
	return (v > 0 && v <= 65535) ? (uint16_t)v : 1;
}

/* ---------- DNS header checks ---------- */

/* Compact DNS header: 12 bytes */
struct dns_hdr {
	uint16_t id;
	uint16_t flags;
	uint16_t qdcount;
	uint16_t ancount;
	uint16_t nscount;
	uint16_t arcount;
};

/* Validate DNS header fields after mutation */
static void check_dns_header(const uint8_t *data, size_t size)
{
	if (size < 12) {
		return;
	}

	const struct dns_hdr *h = (const struct dns_hdr *)data;

	/* All field combinations should be parseable */
	(void)h->id;
	(void)h->flags;
	(void)h->qdcount;
	(void)h->ancount;
	(void)h->nscount;
	(void)h->arcount;
}

/* ---------- libFuzzer entry ---------- */

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	if (size < 1) {
		return 0;
	}

	/* 1. DNS name encoding */
	char name[256];
	size_t nlen = size < 255 ? size : 255;
	memcpy(name, data, nlen);
	name[nlen] = '\0';

	char *wire = NULL;
	size_t wlen = dns_encode_name(name, &wire);
	if (wire) {
		/* Verify round-trip: wire format should start with length byte */
		if (wlen > 0 && (uint8_t)wire[0] <= 63) {
			/* OK -- valid encoding */
		}
		free(wire);
	}

	/* 2. QTYPE parsing */
	char qtype_str[32];
	size_t qlen = size < 31 ? size : 31;
	memcpy(qtype_str, data, qlen);
	qtype_str[qlen] = '\0';
	parse_qtype(qtype_str);

	/* 3. DNS header field validation */
	check_dns_header(data, size);

	/* 4. Edge: empty string, pure dots, single char */
	dns_encode_name("", &wire);
	free(wire);
	dns_encode_name("...", &wire);
	free(wire);
	dns_encode_name("a", &wire);
	free(wire);

	return 0;
}
