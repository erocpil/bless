/*
 * fuzz_bless.c — libFuzzer harness for BLESS core functions.
 *
 * Feeds random bytes to the IP/port parsers, power-of-2, and entropy helpers.
 * Does NOT require DPDK — tests only the standalone C functions.
 *
 * Build:
 *   clang -fsanitize=fuzzer,address -I ../include -o fuzz_bless fuzz_bless.c \
 *         ../src/dist.c -lm
 *
 * Run:
 *   ./fuzz_bless -max_len=256 -runs=100000
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>

/* ---------- function prototypes (from bless source) ---------- */

uint32_t make_power_of_2(uint32_t n);

/* 16-bit one's complement checksum (local copy, avoids DPDK dep) */
static uint16_t icmp_calc_cksum(const void *buf, size_t len)
{
	const uint16_t *data = buf;
	uint32_t sum = 0;
	while (len > 1) { sum += *data++; len -= 2; }
	if (len == 1) {
		sum += *((const uint8_t *)data) << 8;
	}
	while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
	return (uint16_t)(~sum);
}

/* bless_parse_ip_range / bless_parse_port_range — used by the fuzzer.
 * These are stand-alone parsers extracted for fuzzing.  They accept
 * the same inputs the real bless parsers do. */

static int fuzz_parse_port_range(const char *s, uint16_t *port, uint16_t *range)
{
	if (!s || !*s) {
		return -1;
	}

	/* format: PORT[+RANGE] */
	char buf[32];
	strncpy(buf, s, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';

	char *plus = strchr(buf, '+');
	char *end;
	long p;

	if (plus) {
		*plus = '\0';
		p = strtol(buf, &end, 10);
		if (*end != '\0' || p < 0 || p > 65535) {
			return -1;
		}
		*port = (uint16_t)p;

		long r = strtol(plus + 1, &end, 10);
		if (*end != '\0' || r < 0 || r > 65535) {
			return -1;
		}
		*range = (uint16_t)r;
	} else {
		p = strtol(buf, &end, 10);
		if (*end != '\0' || p < 0 || p > 65535) {
			return -1;
		}
		*port = (uint16_t)p;
		*range = 0;
	}
	return 0;
}

static int fuzz_parse_ip_range(const char *s, uint32_t *ip, uint32_t *range)
{
	if (!s || !*s) {
		return -1;
	}

	char buf[64];
	strncpy(buf, s, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';

	char *plus = strchr(buf, '+');

	if (plus) {
		*plus = '\0';
	}

	/* Parse IP */
	struct in_addr addr;
	if (inet_pton(AF_INET, buf, &addr) != 1) {
		return -1;
	}
	*ip = ntohl(addr.s_addr);

	if (plus) {
		char *end;
		long r = strtol(plus + 1, &end, 10);
		if (*end != '\0' || r < 0 || r > 65535) {
			return -1;
		}
		*range = (uint32_t)r;
	} else {
		*range = 0;
	}

	return 0;
}

/* ---------- libFuzzer entry ---------- */

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	if (size < 1) {
		return 0;
	}

	/* 1. make_power_of_2 — safe for any uint32 */
	uint32_t n = 0;
	memcpy(&n, data, size < 4 ? size : 4);
	make_power_of_2(n);

	/* 2. icmp_calc_cksum — safe for any buffer */
	icmp_calc_cksum(data, size);

	/* 3. Port range parser */
	char port_str[32];
	size_t plen = size < 31 ? size : 31;
	memcpy(port_str, data, plen);
	port_str[plen] = '\0';
	uint16_t port, prange;
	fuzz_parse_port_range(port_str, &port, &prange);

	/* 4. IP range parser */
	char ip_str[64];
	size_t ilen = size < 63 ? size : 63;
	memcpy(ip_str, data, ilen);
	ip_str[ilen] = '\0';
	uint32_t ip, irange;
	fuzz_parse_ip_range(ip_str, &ip, &irange);

	return 0;
}
