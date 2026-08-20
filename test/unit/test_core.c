/*
 * test_core.c — Unit tests for standalone utility functions.
 *
 * Covers:
 *   - make_power_of_2()       (from dist.c)
 *   - bless_parse_ip_range()  (from parse.c)
 *   - bless_parse_port_range()(from parse.c)
 *   - random_delay_jitter()   (from define.h — static inline, tested via copy)
 *   - exp_random()            (from define.h — static inline, tested via copy)
 *
 * Compiles without DPDK: all functions tested are pure C.
 * Links against the real src/parse.c and src/dist.c — no more code
 * duplication between test and production.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <arpa/inet.h>
#include <errno.h>
#include <math.h>

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

/* ---------- make_power_of_2 (declared in dist.h, defined in dist.c) ---------- */
unsigned int make_power_of_2(unsigned int n);

/* ---------- random_delay_jitter / exp_random (static inline in define.h) ---------- */
/* These are static inline in define.h — re-implementing here for testability
 * since static inline cannot be linked externally from a header. */

static inline uint64_t random_delay_jitter(uint64_t base, uint64_t jitter)
{
    if (jitter == 0) {
        return base;
    }
    if (base <= jitter) {
        uint64_t r = (uint64_t)random() % (base + jitter + 1);
        return r;
    }
    int64_t lo = (int64_t)base - (int64_t)jitter;
    if (lo < 0) {
        lo = 0;
    }
    uint64_t span = (uint64_t)((int64_t)base + (int64_t)jitter - lo + 1);
    uint64_t r = (uint64_t)random() % span;
    return lo + r;
}

static inline uint64_t exp_random(uint64_t mean_us)
{
    if (mean_us == 0) {
        return 0;
    }
    double u = (double)random() / ((double)RAND_MAX + 1.0);
    if (u <= 0.0) {
        u = 1e-10;
    }
    return (uint64_t)(-(double)mean_us * log(u));
}

/* ====================== tests ====================== */

static void test_make_power_of_2(void)
{
    TEST("make_power_of_2");

    ASSERT(make_power_of_2(0) == 0, "0 -> 0");
    ASSERT(make_power_of_2(1) == 1, "1 -> 1");
    ASSERT(make_power_of_2(2) == 2, "2 -> 2");
    ASSERT(make_power_of_2(3) == 4, "3 -> 4");
    ASSERT(make_power_of_2(4) == 4, "4 -> 4");
    ASSERT(make_power_of_2(5) == 8, "5 -> 8");
    ASSERT(make_power_of_2(16) == 16, "16 -> 16");
    ASSERT(make_power_of_2(17) == 32, "17 -> 32");
    ASSERT(make_power_of_2(255) == 256, "255 -> 256");
    ASSERT(make_power_of_2(256) == 256, "256 -> 256");
    ASSERT(make_power_of_2(257) == 512, "257 -> 512");
    ASSERT(make_power_of_2(65535) == 65536, "65535 -> 65536");
    ASSERT(make_power_of_2(65536) == 65536, "65536 -> 65536");
}

static void test_bless_parse_port_range(void)
{
    uint16_t port;
    int32_t range;

    TEST("parse_port_range (valid)");

    ASSERT(bless_parse_port_range("80", &port, &range) == 0, "bare '80' OK");
    ASSERT(port == 80, "  port=80");
    ASSERT(range == 0, "  range=0");

    ASSERT(bless_parse_port_range("8080+10", &port, &range) == 0, "'8080+10' OK");
    ASSERT(port == 8080, "  port=8080");
    ASSERT(range == 10, "  range=10");

    ASSERT(bless_parse_port_range("0+255", &port, &range) == 0, "'0+255' OK");
    ASSERT(port == 0, "  port=0");
    ASSERT(range == 255, "  range=255");

    ASSERT(bless_parse_port_range("65535+0", &port, &range) == 0, "'65535+0' OK");
    ASSERT(port == 65535, "  port=65535");
    ASSERT(range == 0, "  range=0");

    TEST("parse_port_range (invalid)");
    ASSERT(bless_parse_port_range("", &port, &range) == -1, "empty -> -1");
    ASSERT(bless_parse_port_range("abc", &port, &range) == -1, "'abc' -> -1");
    ASSERT(bless_parse_port_range("80+abc", &port, &range) == -1, "'80+abc' -> -1");
    ASSERT(bless_parse_port_range("-1", &port, &range) == -1, "'-1' -> -1");
    ASSERT(bless_parse_port_range("70000", &port, &range) == -1, "'70000' -> -1 (overflow)");
    ASSERT(bless_parse_port_range("80+", &port, &range) == -1, "'80+' -> -1 (empty range)");
}

static void test_bless_parse_ip_range(void)
{
    uint32_t ip;
    int64_t range;

    TEST("parse_ip_range (valid)");
    ASSERT(bless_parse_ip_range("172.16.0.1", &ip, &range) == 0, "bare '172.16.0.1' OK");
    ASSERT(range == 0, "  range=0");
    ASSERT(ntohl(ip) == ((172u<<24)|(16u<<16)|1), "  ip correct");

    ASSERT(bless_parse_ip_range("10.0.0.1+99", &ip, &range) == 0, "'10.0.0.1+99' OK");
    ASSERT(range == 99, "  range=99");
    ASSERT(ntohl(ip) == ((10u<<24)|1), "  ip correct");

    ASSERT(bless_parse_ip_range("0.0.0.0+255", &ip, &range) == 0, "'0.0.0.0+255' OK");
    ASSERT(range == 255, "  range=255");
    ASSERT(ip == 0, "  ip=0");

    ASSERT(bless_parse_ip_range("255.255.255.255", &ip, &range) == 0,
           "'255.255.255.255' OK");
    ASSERT(range == 0, "  range=0");

    TEST("parse_ip_range (invalid)");
    ASSERT(bless_parse_ip_range("", &ip, &range) == -1, "empty -> -1");
    ASSERT(bless_parse_ip_range("bad", &ip, &range) == -1, "'bad' -> -1");
    ASSERT(bless_parse_ip_range("256.256.256.256", &ip, &range) == -1,
           "'256.256.256.256' -> -1");
    ASSERT(bless_parse_ip_range("172.16.0.1+abc", &ip, &range) == -1,
           "'172.16.0.1+abc' -> -1");
    ASSERT(bless_parse_ip_range("172.16.0.1+", &ip, &range) == -1,
           "'172.16.0.1+' -> -1 (empty range)");
    ASSERT(bless_parse_ip_range("172.16.0.1+99999999999999999999", &ip, &range) == -1,
           "overflow range -> -1");
    ASSERT(bless_parse_ip_range("...", &ip, &range) == -1, "'...' -> -1");
}

static void test_delay_jitter(void)
{
    TEST("random_delay_jitter");
    /* base=100, jitter=20 => range [80, 120] */
    uint64_t min_val = UINT64_MAX, max_val = 0;
    for (int i = 0; i < 1000; i++) {
        uint64_t v = random_delay_jitter(100, 20);
        if (v < min_val) {
            min_val = v;
        }
        if (v > max_val) {
            max_val = v;
        }
        ASSERT(v >= 80 && v <= 120, "  jitter(100,20)=%lu in [80,120]", v);
        if (n_fail) {
            break; /* stop on first failure */
        }
    }
    ASSERT(min_val <= 85, "  min observed <= 85 (got %lu)", min_val);
    ASSERT(max_val >= 115, "  max observed >= 115 (got %lu)", max_val);

    /* jitter=0 => always base */
    ASSERT(random_delay_jitter(100, 0) == 100, "  jitter(100,0)=100");

    /* base < jitter */
    for (int i = 0; i < 100; i++) {
        uint64_t v = random_delay_jitter(5, 20);
        ASSERT(v <= 25, "  jitter(5,20)=%lu in [0,25]", v);
        if (n_fail) {
            break;
        }
    }

    TEST("exp_random (statistical)");
    double sum = 0;
    int n = 10000;
    for (int i = 0; i < n; i++) {
        sum += (double)exp_random(100);
    }
    double mean = sum / n;
    /* Exponential with mean 100: sample mean should be ~100 (±20 for 10k) */
    ASSERT(mean > 80 && mean < 120,
           "  exp_random(100) mean ~ %.1f (expected ~100)", mean);

    ASSERT(exp_random(0) == 0, "  exp_random(0) = 0");
}

/* ====================== main ====================== */

int main(void)
{
    printf("BLESS core unit tests\n");
    printf("=======================\n");

    test_make_power_of_2();
    test_bless_parse_port_range();
    test_bless_parse_ip_range();
    test_delay_jitter();

    printf("\n=======================\n");
    printf("Results: %d passed, %d failed\n", n_pass, n_fail);
    return n_fail ? 1 : 0;
}
