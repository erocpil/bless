/*
 * fuzz_ip_range.c — libFuzzer harness for bless IP/port range parsers.
 *
 * Tests the production functions bless_parse_ip_range(),
 * bless_parse_port_range(), and bless_parse_ipv6_range() from
 * src/parse.c with arbitrary byte sequences.  These are DPDK-
 * independent and linked directly — no local copies.
 *
 * Build (local):
 *   clang -fsanitize=fuzzer,address -I include -o fuzz_ip_range \
 *         fuzz_ip_range.c src/parse.c
 *
 * Run:
 *   ./fuzz_ip_range -max_len=128 -runs=100000
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* Production functions from src/parse.c (DPDK-independent) */
int bless_parse_ip_range(const char *data, uint32_t *ip, int64_t *range);
int bless_parse_port_range(const char *data, uint16_t *port, int32_t *range);
int bless_parse_ipv6_range(const char *data, uint8_t addr[16], int64_t *range);

/* ---------- libFuzzer entry ---------- */

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	if (size < 1) {
		return 0;
	}

	char buf[128];
	size_t len = size < 127 ? size : 127;
	memcpy(buf, data, len);
	buf[len] = '\0';

	/* 1. IPv4 range */
	uint32_t ip;
	int64_t  ipv4_range;
	bless_parse_ip_range(buf, &ip, &ipv4_range);

	/* 2. Port range */
	uint16_t port;
	int32_t  port_range;
	bless_parse_port_range(buf, &port, &port_range);

	/* 3. IPv6 range */
	uint8_t ipv6[16];
	int64_t ipv6_range;
	bless_parse_ipv6_range(buf, ipv6, &ipv6_range);

	/* 4. Edge cases: empty, bare '+', overflow */
	bless_parse_ip_range("", &ip, &ipv4_range);
	bless_parse_ip_range("+", &ip, &ipv4_range);
	bless_parse_port_range("", &port, &port_range);
	bless_parse_port_range("+", &port, &port_range);
	bless_parse_ipv6_range("", ipv6, &ipv6_range);

	return 0;
}
