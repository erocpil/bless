/* Unit tests for dist.c — weighted distribution + power-of-2 helpers.
 * Compile and run with:
 *   gcc -I include -o /tmp/test_dist test/unit/test_dist.c src/dist.c && /tmp/test_dist
 * No DPDK dependency required. */

#include "dist.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned int n_pass = 0;
static unsigned int n_fail = 0;

#define TEST_EQ(a, b, msg) do {                                         \
	if ((a) != (b)) {                                               \
		fprintf(stderr, "FAIL %s:%d: " msg ": got %ld, expected %ld\n", \
			__FILE__, __LINE__, (long)(a), (long)(b));      \
		n_fail++;                                               \
	} else {                                                        \
		n_pass++;                                               \
	}                                                               \
} while(0)

#define TEST_OK(expr, msg) do {                                         \
	if ((expr) != 0) {                                              \
		fprintf(stderr, "FAIL %s:%d: " msg ": rc=%d\n",         \
			__FILE__, __LINE__, (int)(expr));               \
		n_fail++;                                               \
	} else {                                                        \
		n_pass++;                                               \
	}                                                               \
} while(0)

/* ── make_power_of_2 ────────────────────────────────────────────────── */

static void test_make_power_of_2(void)
{
	printf("=== make_power_of_2 ===\n");

	TEST_EQ(make_power_of_2(0),   0u,   "0 -> 0");
	TEST_EQ(make_power_of_2(1),   1u,   "1 -> 1");
	TEST_EQ(make_power_of_2(2),   2u,   "2 -> 2");
	TEST_EQ(make_power_of_2(3),   4u,   "3 -> 4");
	TEST_EQ(make_power_of_2(4),   4u,   "4 -> 4");
	TEST_EQ(make_power_of_2(5),   8u,   "5 -> 8");
	TEST_EQ(make_power_of_2(7),   8u,   "7 -> 8");
	TEST_EQ(make_power_of_2(8),   8u,   "8 -> 8");
	TEST_EQ(make_power_of_2(9),   16u,  "9 -> 16");
	TEST_EQ(make_power_of_2(15),  16u,  "15 -> 16");
	TEST_EQ(make_power_of_2(16),  16u,  "16 -> 16");
	TEST_EQ(make_power_of_2(100), 128u, "100 -> 128");
	TEST_EQ(make_power_of_2(255), 256u, "255 -> 256");
	TEST_EQ(make_power_of_2(256), 256u, "256 -> 256");
	TEST_EQ(make_power_of_2(1023), 1024u, "1023 -> 1024");
	TEST_EQ(make_power_of_2(1u<<15), 1u<<15, "32768 -> 32768");
	TEST_EQ(make_power_of_2((1u<<15)+1), 1u<<16, "32769 -> 65536");

	/* overflow: n > 2^31 returns 0 */
	TEST_EQ(make_power_of_2(1u << 31), 1u << 31, "2^31 -> 2^31");
	TEST_EQ(make_power_of_2((1u << 31) + 1), 0u, "2^31+1 -> 0 (overflow)");
	TEST_EQ(make_power_of_2(UINT32_MAX), 0u, "UINT32_MAX -> 0 (overflow)");
}

/* ── distribute — valid inputs ──────────────────────────────────────── */

static void test_distribute_equal(void)
{
	printf("=== distribute (equal weights) ===\n");
	uint32_t weights[4] = {1, 1, 1, 1};
	uint64_t result[4] = {0};
	unsigned int n = 4;
	uint64_t total = 100;

	int rc = distribute(weights, n, total, result);
	TEST_EQ(rc, 0, "distribute should succeed");

	uint64_t sum = 0;
	for (unsigned int i = 0; i < n; i++) sum += result[i];
	TEST_EQ(sum, total, "total sum should equal total");

	for (unsigned int i = 0; i < n; i++)
		TEST_EQ(result[i], 25u, "equal: each gets total/n");
}

static void test_distribute_skewed(void)
{
	printf("=== distribute (skewed weights) ===\n");
	uint32_t weights[3] = {70, 20, 10};
	uint64_t result[3] = {0};
	unsigned int n = 3;
	uint64_t total = 100;

	int rc = distribute(weights, n, total, result);
	TEST_EQ(rc, 0, "distribute should succeed");

	uint64_t sum = 0;
	for (unsigned int i = 0; i < n; i++) sum += result[i];
	TEST_EQ(sum, total, "total sum should equal total");

	/* 70:20:10 split of 97 (100-3 base): 97*70/100=67 rem=90,
	 * 97*20/100=19 rem=40, 97*10/100=9 rem=70. Leftover=2 →
	 * largest remainders: idx0(90), idx2(70). */
	TEST_EQ(result[0], 69u, "weight 70 -> 69");
	TEST_EQ(result[1], 20u, "weight 20 -> 20");
	TEST_EQ(result[2], 11u, "weight 10 -> 11");
}

static void test_distribute_extreme(void)
{
	printf("=== distribute (extreme: 90/5/5) ===\n");
	uint32_t weights[3] = {90, 5, 5};
	uint64_t result[3] = {0};
	unsigned int n = 3;
	uint64_t total = 1000;

	int rc = distribute(weights, n, total, result);
	TEST_EQ(rc, 0, "distribute should succeed");

	uint64_t sum = 0;
	for (unsigned int i = 0; i < n; i++) sum += result[i];
	TEST_EQ(sum, total, "total sum should equal total");

	TEST_EQ(result[0], 898u, "weight 90 -> 898");
	TEST_EQ(result[1], 51u,  "weight 5 -> 51");
	TEST_EQ(result[2], 51u,  "weight 5 -> 51");
}

static void test_distribute_small_total(void)
{
	printf("=== distribute (small total, equal to n) ===\n");
	uint32_t weights[3] = {50, 30, 20};
	uint64_t result[3] = {0};
	unsigned int n = 3;
	uint64_t total = 3;

	int rc = distribute(weights, n, total, result);
	TEST_EQ(rc, 0, "distribute should succeed");

	uint64_t sum = 0;
	for (unsigned int i = 0; i < n; i++) sum += result[i];
	TEST_EQ(sum, total, "total sum should equal total");
	TEST_EQ(result[0], 1u, "minimal: each gets 1");
	TEST_EQ(result[1], 1u, "minimal: each gets 1");
	TEST_EQ(result[2], 1u, "minimal: each gets 1");
}

static void test_distribute_single(void)
{
	printf("=== distribute (single category) ===\n");
	uint32_t weights[1] = {1};
	uint64_t result[1] = {0};
	unsigned int n = 1;
	uint64_t total = 500;

	int rc = distribute(weights, n, total, result);
	TEST_EQ(rc, 0, "distribute should succeed");
	TEST_EQ(result[0], total, "single category gets all");
}

static void test_distribute_deterministic(void)
{
	printf("=== distribute (determinism) ===\n");
	uint32_t weights[4] = {7, 3, 5, 1};
	uint64_t r1[4] = {0}, r2[4] = {0};

	distribute(weights, 4, 1000, r1);
	distribute(weights, 4, 1000, r2);

	for (int i = 0; i < 4; i++)
		TEST_EQ(r1[i], r2[i], "same inputs → same outputs");
}

/* ── distribute — error paths ───────────────────────────────────────── */

static void test_distribute_errors(void)
{
	printf("=== distribute (error paths) ===\n");
	uint32_t w[2] = {1, 1};
	uint64_t r[2];

	/* NULL arguments */
	TEST_EQ(distribute(NULL, 2, 10, r), DIST_ERR_NULL, "NULL weights");
	TEST_EQ(distribute(w, 2, 10, NULL), DIST_ERR_NULL, "NULL result");

	/* n == 0 */
	TEST_EQ(distribute(w, 0, 10, r), DIST_ERR_ZERO_N, "n == 0");

	/* total < n */
	TEST_EQ(distribute(w, 2, 1, r), DIST_ERR_TOTAL_SMALL, "total < n");

	/* zero sum */
	uint32_t wz[3] = {0, 0, 0};
	TEST_EQ(distribute(wz, 3, 10, r), DIST_ERR_ZERO_SUM, "zero weight sum");

	/* n > DIST_MAX_N boundary */
	uint32_t w64[DIST_MAX_N];
	uint64_t r64[DIST_MAX_N];
	for (unsigned i = 0; i < DIST_MAX_N; i++)
		w64[i] = 1;

	/* n == DIST_MAX_N (64) — should pass */
	TEST_EQ(distribute(w64, DIST_MAX_N, DIST_MAX_N, r64),
		DIST_OK, "n == DIST_MAX_N (max)");

	/* n == DIST_MAX_N + 1 (65) — should reject */
	uint32_t w65[DIST_MAX_N + 1];
	uint64_t r65_out[DIST_MAX_N + 1];
	for (unsigned i = 0; i <= DIST_MAX_N; i++)
		w65[i] = 1;
	TEST_EQ(distribute(w65, DIST_MAX_N + 1, DIST_MAX_N + 1, r65_out),
		DIST_ERR_N_TOO_LARGE, "n == DIST_MAX_N+1 rejected");
}

static void test_distribute_large(void)
{
	printf("=== distribute (large values, stress) ===\n");
	uint32_t weights[4] = {1000, 2000, 3000, 4000};
	uint64_t result[4] = {0};

	int rc = distribute(weights, 4, UINT32_MAX, result);
	TEST_EQ(rc, 0, "distribute large total should succeed");

	uint64_t sum = 0;
	for (int i = 0; i < 4; i++) {
		TEST_EQ(result[i] > 0, 1, "each category gets at least 1");
		sum += result[i];
	}
	TEST_EQ(sum, (uint64_t)UINT32_MAX, "sum matches total");
}

int main(void)
{
	printf("BLESS dist.c unit tests\n");
	printf("=======================\n\n");

	test_make_power_of_2();
	printf("\n");
	test_distribute_equal();
	printf("\n");
	test_distribute_skewed();
	printf("\n");
	test_distribute_extreme();
	printf("\n");
	test_distribute_small_total();
	printf("\n");
	test_distribute_single();
	printf("\n");
	test_distribute_deterministic();
	printf("\n");
	test_distribute_errors();
	printf("\n");
	test_distribute_large();
	printf("\n");

	printf("=======================\n");
	printf("Results: %u passed, %u failed\n", n_pass, n_fail);

	return n_fail > 0 ? 1 : 0;
}
