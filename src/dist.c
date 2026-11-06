/* dist.c -- checked-integer proportional distribution and power-of-2 helpers.
 *
 * Pure integer arithmetic — no floating-point, no undefined conversions.
 * Pure C, no DPDK dependency — designed for standalone unit testing. */

#include "dist.h"

#include <stddef.h>
#include <string.h>

/* ── internal helpers ───────────────────────────────────────────────── */

/** Multiply a by b (uint32_t), write result to out.  Returns 0 on success,
 *  -1 on overflow.  Stateless — usable from fuzz targets. */
static inline int mul_u64_u32_safe(uint64_t a, uint32_t b, uint64_t *out)
{
	if (b == 0) {
		*out = 0;
		return 0;
	}
	if (a > UINT64_MAX / b) {
		return -1;
	}
	*out = a * b;
	return 0;
}

/* ── public API ─────────────────────────────────────────────────────── */

int distribute(const uint32_t *weights, uint32_t n, uint64_t total,
	       uint64_t *result)
{
	/* --- argument validation --- */
	if (!weights || !result) {
		return DIST_ERR_NULL;
	}
	if (n == 0) {
		return DIST_ERR_ZERO_N;
	}
	if (n > DIST_MAX_N) {
		return DIST_ERR_N_TOO_LARGE;
	}
	if (total < n) {
		return DIST_ERR_TOTAL_SMALL;
	}

	/* --- compute weight sum (uint64_t to avoid overflow) --- */
	uint64_t sum = 0;
	for (uint32_t i = 0; i < n; i++)
		sum += weights[i];
	if (sum == 0) {
		return DIST_ERR_ZERO_SUM;
	}

	/* --- Round 1: every category gets at least 1 --- */
	for (uint32_t i = 0; i < n; i++)
		result[i] = 1;
	uint64_t remaining = total - n;

	/* --- Round 2: integer-proportional allocation --- */
	uint64_t base[DIST_MAX_N];   /* intermediate allocation */
	uint64_t rem[DIST_MAX_N];    /* remainders for tie-breaking */
	uint64_t allocated = 0;

	for (uint32_t i = 0; i < n; i++) {
		uint64_t prod;
		if (mul_u64_u32_safe(remaining, weights[i], &prod) != 0) {
			return DIST_ERR_OVERFLOW;
		}
		base[i] = prod / sum;
		rem[i]  = prod % sum;
		result[i] += base[i];
		allocated  += base[i];
	}

	/* --- Round 3: assign leftovers to largest remainders --- */
	uint64_t leftover = remaining - allocated;
	while (leftover > 0) {
		uint32_t best = 0;
		for (uint32_t i = 1; i < n; i++) {
			if (rem[i] > rem[best]) {
				best = i;
			}
		}
		/* Every remainder is < sum, so the largest is always < sum.
		 * We mark consumed slots with a sentinel that cannot be the
		 * largest in any comparison. */
		result[best]++;
		rem[best] = 0;   /* cannot be > any other remainder */
		leftover--;
	}

	return 0;
}

unsigned int make_power_of_2(unsigned int n)
{
	if (n == 0) {
		return 0;
	}

	/* n > 2^31: the next power of 2 would overflow unsigned int.
	 * Return 0 as an error sentinel — caller must check. */
	if (n > (1u << 31)) {
		return 0;
	}

	n--;
	n |= n >> 1;
	n |= n >> 2;
	n |= n >> 4;
	n |= n >> 8;
	n |= n >> 16;
	return n + 1;
}
