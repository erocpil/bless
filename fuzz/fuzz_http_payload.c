/*
 * fuzz_http_payload.c -- libFuzzer harness for bless HTTP payload builder.
 *
 * Tests equivalent logic to build_http_payload() (src/proto_http.c) and
 * mutation_http helpers (src/mutation_http.c).  The production functions
 * depend on DPDK headers (rte_mbuf) and cannot be linked in a fuzz-only
 * build — all implementations here are LOCAL COPIES of the same logic.
 *
 * Regression risk: if production code changes without updating this
 * local copy, fuzz may pass while production has a bug.  Mitigations:
 * (1) production fuzz coverage comes from fuzz_bless (links src/dist.c);
 * (2) unit tests exercise the real HTTP mutation path via the DPDK
 * smoke test in CI.  When production proto_http.c or mutation_http.c
 * changes, review and update the corresponding local copy here.
 *
 * Build (local):
 *   clang -fsanitize=fuzzer,address -o fuzz_http_payload fuzz_http_payload.c
 *
 * Run:
 *   ./fuzz_http_payload -max_len=512 -runs=100000
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* minimal PRNG for method/path/host selection */
static uint32_t fuzz_rand_state = 0xdeadbeef;

static uint32_t fuzz_rand(void)
{
	fuzz_rand_state ^= fuzz_rand_state << 13;
	fuzz_rand_state ^= fuzz_rand_state >> 17;
	fuzz_rand_state ^= fuzz_rand_state << 5;
	return fuzz_rand_state;
}

/* ---------- rand_csv (from proto_http.c) ---------- */

static const char *rand_csv(const char *csv, const char *default_val)
{
	if (!csv || !csv[0]) {
		return default_val;
	}

	/* Count items */
	int n = 1;
	for (const char *p = csv; *p; p++)
		if (*p == ',') {
			n++;
		}

	int pick = fuzz_rand() % n;
	int cur = 0;
	const char *start = csv;
	const char *p;
	for (p = csv; *p; p++) {
		if (*p == ',') {
			if (cur == pick) {
				break;
			}
			cur++;
			start = p + 1;
		}
	}

	static char buf[256];
	size_t len = (size_t)(p - start);
	if (len > 255) {
		len = 255;
	}
	memcpy(buf, start, len);
	buf[len] = '\0';
	return buf;
}

/* ---------- build_http_payload (from proto_http.c) ---------- */

#define DEF_METHODS "GET"
#define DEF_PATHS   "/"
#define DEF_HOSTS   "example.com"

static uint16_t build_http_payload(char *buf, uint16_t max_payload,
	const char *methods, const char *paths, const char *hosts)
{
	const char *method = rand_csv(methods, DEF_METHODS);
	const char *path   = rand_csv(paths,   DEF_PATHS);
	const char *host   = rand_csv(hosts,   DEF_HOSTS);

	int len = snprintf(buf, max_payload,
		"%s %s HTTP/1.1\r\n"
		"Host: %s\r\n"
		"User-Agent: bless/1.0\r\n"
		"Accept: */*\r\n"
		"Connection: close\r\n"
		"\r\n",
		method, path, host);

	return (len > 0 && (uint16_t)len < max_payload) ? (uint16_t)len : 0;
}

/* ---------- HTTP mutations (from mutation_http.c) ---------- */

/* malformed: replace HTTP/1.1 with HTTP/9.9 */
static void mut_http_malformed(char *buf)
{
	/* find "HTTP/1.1" and replace with "HTTP/9.9" */
	char *p = strstr(buf, "HTTP/1.1");
	if (p) {
		p[5] = '9';
		p[7] = '9';
	}
}

/* method_invalid: replace method with garbage */
static void mut_http_method_invalid(char *buf, uint16_t *plen)
{
	static const char *bad[] = {"FLURB", "XXXX", "\xff\xfeGET", "G"};
	int pick = fuzz_rand() % 4;
	size_t mlen = strlen(bad[pick]);

	/* find the first space (end of method) */
	char *sp = memchr(buf, ' ', *plen);
	if (sp && sp > buf) {
		size_t old_len = (size_t)(sp - buf);
		int diff = (int)mlen - (int)old_len;
		if (diff != 0) {
			memmove(sp + diff, sp, *plen - (size_t)(sp - buf));
			*plen = (uint16_t)(*plen + diff);
		}
		memcpy(buf, bad[pick], mlen);
	}
}

/* host_overflow: inject ~400-byte host header */
static void mut_http_host_overflow(char *buf, uint16_t *plen)
{
	(void)buf;
	(void)plen;
	/* This mutation works on the raw mbuf, not the payload string.
	 * For the fuzz harness, just verify we don't crash. */
}

/* crlf_missing: replace \r\n with \n */
static void mut_http_crlf_missing(char *buf, uint16_t *plen)
{
	for (uint16_t i = 0; i < *plen; i++) {
		if (buf[i] == '\r' && (i + 1 < *plen) && buf[i + 1] == '\n') {
			/* shift everything left by 1 */
			memmove(&buf[i], &buf[i + 1], *plen - i - 1);
			(*plen)--;
		}
	}
}

/* ---------- libFuzzer entry ---------- */

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	if (size < 3) {
		return 0;
	}

	fuzz_rand_state = ((uint32_t)data[0] << 24)
		| ((uint32_t)data[1] << 16)
		| ((uint32_t)data[2] << 8)
		| UINT32_C(0xdead);

	char payload[512];
	char methods[128];
	char paths[128];
	char hosts[128];

	/* build inputs from fuzz data */
	size_t off = 3;
	size_t mlen;
	if (off >= size) {
		mlen = 0;
	} else {
		mlen = (size - off >= 127) ? 127 : (size - off);
		if (mlen == 0) {
			mlen = 1;
		}
	}
	if (mlen > 0) {
		memcpy(methods, &data[off], mlen);
		methods[mlen] = '\0';
	} else {
		methods[0] = '\0';
	}
		off += mlen;
		size_t plen2 = 0;
		if (off < size) {
			plen2 = (size - off >= 127) ? 127 : (size - off);
			if (plen2 == 0) {
				plen2 = 1;
			}
			memcpy(paths, &data[off], plen2);
			paths[plen2] = '\0';
		} else {
			paths[0] = '\0';
		}

	size_t hlen = (size > 0) ? size / 3 : 4;
	if (hlen > 127) {
		hlen = 127;
	}
	memcpy(hosts, data, hlen);
	hosts[hlen] = '\0';

	/* 1. Normal build */
	uint16_t plen = build_http_payload(payload, sizeof(payload),
		methods, paths, hosts);

	/* 2. Apply mutations */
	if (plen > 0) {
		mut_http_malformed(payload);
		mut_http_method_invalid(payload, &plen);
		mut_http_crlf_missing(payload, &plen);
		mut_http_host_overflow(payload, &plen);
	}

	/* 3. Re-build with mutated state */
	build_http_payload(payload, sizeof(payload), methods, paths, hosts);

	return 0;
}
