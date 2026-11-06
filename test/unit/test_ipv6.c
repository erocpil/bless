/*
 * test_ipv6.c -- Unit tests for IPv6 utility functions.
 *
 * Covers:
 *   - bless_parse_ipv6_range()  (valid + edge + invalid cases)
 *
 * All functions are pure C with no DPDK dependency and link the production
 * parser, so range-validation regressions are tested directly.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "bless_parse.h"

/* ---------- test harness ---------- */

static int n_pass = 0, n_fail = 0;

#define TEST(name) do { printf("\n=== %s ===\n", name); } while(0)
#define ASSERT(cond, fmt, ...) do { \
    if (!(cond)) { \
        printf("  FAIL: " fmt "\n", ##__VA_ARGS__); \
        n_fail++; \
    } else { \
        printf("  PASS: " fmt "\n", ##__VA_ARGS__); \
        n_pass++; \
    } \
} while(0)

/* ---------- helper ---------- */

/* Compare two 16-byte IPv6 addresses. */
static int ipv6_eq(const uint8_t a[16], const uint8_t b[16])
{
    return memcmp(a, b, 16) == 0;
}

/* ====================== tests ====================== */

static void test_ipv6_parse_basic(void)
{
    uint8_t addr[16];
    int64_t range;

    TEST("parse_ipv6_range (basic)");

    /* bare address, no range */
    ASSERT(bless_parse_ipv6_range("2001:db8::1", addr, &range) == 0,
           "bare '2001:db8::1' OK");
    ASSERT(range == 0, "  range=0");

    /* address with range */
    ASSERT(bless_parse_ipv6_range("2001:db8::1+100", addr, &range) == 0,
           "'2001:db8::1+100' OK");
    ASSERT(range == 100, "  range=100");

    /* range=0 explicitly */
    ASSERT(bless_parse_ipv6_range("fe80::1+0", addr, &range) == 0,
           "'fe80::1+0' OK");
    ASSERT(range == 0, "  range=0");
}

static void test_ipv6_parse_roundtrip(void)
{
    uint8_t addr1[16], addr2[16];
    int64_t range;

    TEST("parse_ipv6_range (roundtrip)");

    /* Same address, different representations -- inet_pton normalises */
    ASSERT(bless_parse_ipv6_range("::1", addr1, &range) == 0,
           "'::1' OK");
    ASSERT(bless_parse_ipv6_range("0:0:0:0:0:0:0:1", addr2, &range) == 0,
           "'0:0:0:0:0:0:0:1' OK");
    ASSERT(ipv6_eq(addr1, addr2),
           "  ::1 == 0:0:0:0:0:0:0:1 (canonical equivalence)");
}

static void test_ipv6_parse_edge_cases(void)
{
    uint8_t addr[16];
    int64_t range;

    TEST("parse_ipv6_range (edge cases)");

    /* all-zeros — note: "::" (strlen=2) is rejected by the <3 guard */
    ASSERT(bless_parse_ipv6_range("::1", addr, &range) == 0,
           "'::1' OK (shortest len-3 IPv6)");

    /* full expanded form */
    ASSERT(bless_parse_ipv6_range("2001:0db8:0000:0000:0000:ff00:0042:8329",
                            addr, &range) == 0,
           "full expanded form OK");

    /* IPv4-mapped IPv6 */
    ASSERT(bless_parse_ipv6_range("::ffff:192.0.2.128", addr, &range) == 0,
           "'::ffff:192.0.2.128' OK");

    /* link-local */
    ASSERT(bless_parse_ipv6_range("fe80::1", addr, &range) == 0,
           "'fe80::1' OK");

    /* multicast */
    ASSERT(bless_parse_ipv6_range("ff02::1", addr, &range) == 0,
           "'ff02::1' OK");
}

static void test_ipv6_parse_range_boundary(void)
{
    uint8_t addr[16];
    int64_t range;

    TEST("parse_ipv6_range (range boundary)");

    /* large but valid range */
    ASSERT(bless_parse_ipv6_range("2001:db8::1+65535", addr, &range) == 0,
           "'...::1+65535' OK");
    ASSERT(range == 65535, "  range=65535");

    /* zero base with large range */
    ASSERT(bless_parse_ipv6_range("::1+1000000", addr, &range) == 0,
           "'::1+1000000' OK");
    ASSERT(range == 1000000, "  range=1000000");
}

static void test_ipv6_parse_invalid(void)
{
    uint8_t addr[16];
    int64_t range;

    TEST("parse_ipv6_range (invalid)");

    /* empty / too short */
    ASSERT(bless_parse_ipv6_range("", addr, &range) == -1, "empty -> -1");
    ASSERT(bless_parse_ipv6_range("ab", addr, &range) == -1, "'ab' -> -1 (too short)");

    /* not an IPv6 address */
    ASSERT(bless_parse_ipv6_range("not_an_ip", addr, &range) == -1,
           "'not_an_ip' -> -1");

    /* IPv4 address -- inet_pton(AF_INET6) rejects */
    ASSERT(bless_parse_ipv6_range("192.168.1.1", addr, &range) == -1,
           "'192.168.1.1' -> -1 (not IPv6)");

    /* bad hex */
    ASSERT(bless_parse_ipv6_range("2001:db8::gggg", addr, &range) == -1,
           "'2001:db8::gggg' -> -1");

    /* trailing garbage */
    ASSERT(bless_parse_ipv6_range("2001:db8::1/64", addr, &range) == -1,
           "'2001:db8::1/64' -> -1 (no CIDR supported)");

    /* bad range */
    ASSERT(bless_parse_ipv6_range("2001:db8::1+abc", addr, &range) == -1,
           "'...::1+abc' -> -1");

    /* empty range after + */
    ASSERT(bless_parse_ipv6_range("2001:db8::1+", addr, &range) == -1,
           "'...::1+' -> -1 (empty range)");

    /* overflow range */
    ASSERT(bless_parse_ipv6_range("2001:db8::1+99999999999999999999",
                            addr, &range) == -1,
           "overflow range -> -1");
}

/* ====================== main ====================== */

int main(void)
{
    printf("BLESS IPv6 unit tests\n");
    printf("=======================\n");

    test_ipv6_parse_basic();
    test_ipv6_parse_roundtrip();
    test_ipv6_parse_edge_cases();
    test_ipv6_parse_range_boundary();
    test_ipv6_parse_invalid();

    printf("\n=======================\n");
    printf("Results: %d passed, %d failed\n", n_pass, n_fail);
    return n_fail ? 1 : 0;
}
