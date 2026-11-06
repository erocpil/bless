/*
 * fuzz_dist.c -- libFuzzer harness for bless distribution engine.
 *
 * Tests distribute() (weighted proportional allocation with largest
 * remainder) and make_power_of_2().  These are pure-C functions with
 * no DPDK dependency -- links directly against ../src/dist.c.
 *
 * Build (local):
 *   clang -fsanitize=fuzzer,address -I ../src -o fuzz_dist \
 *         fuzz_dist.c ../src/dist.c
 *
 * Run:
 *   ./fuzz_dist -max_len=256 -runs=100000
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* Prototypes from dist.c -- the fuzzer links against the real code. */
int distribute(const uint32_t *weights, uint32_t n, uint64_t total,
	       uint64_t *result);
unsigned int make_power_of_2(unsigned int n);

#define MAX_WEIGHTS 64

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	if (size < 8) {
		return 0;
	}

	/* 1. make_power_of_2 -- safe for any uint32 */
	uint32_t p2_input = 0;
	memcpy(&p2_input, data, size < 4 ? size : 4);
	make_power_of_2(p2_input);

	/* 2. distribute() -- exercise the allocation algorithm.
	 *
	 * Derive parameters from fuzz data:
	 *   data[0]      = n_weights (1..63, clamped)
	 *   data[1..2]   = total (at least n_weights)
	 *   data[3..]    = weight array (N * 4 bytes)
	 */
	uint32_t n = (data[0] % (MAX_WEIGHTS - 1)) + 1;
	if (size < 4 + n * 4) {
		/* Not enough data for the full weight array -- scale down */
		n = (size - 4) / 4;
		if (n < 1) {
			return 0;
		}
	}

	uint32_t weights[MAX_WEIGHTS];
	for (uint32_t i = 0; i < n && (4 + i * 4 + 4) <= size; i++) {
		memcpy(&weights[i], &data[4 + i * 4], 4);
		if (weights[i] == 0) {
			weights[i] = 1; /* avoid zero-weight */
		}
	}

	/* total: at least n (to guarantee 1 per category), up to 1M */
	uint64_t total;
	uint16_t raw_total;
	memcpy(&raw_total, &data[1], 2);
	total = n + (raw_total % 1000000);

	/* result buffer on stack */
	uint64_t result[MAX_WEIGHTS];

	int rc = distribute(weights, n, total, result);
	if (rc != 0) {
		return 0; /* gracefully reject invalid fuzz inputs */
	}

	/* 3. Verify invariants:
	 *    a) Sum of result == total
	 *    b) Each result[i] >= 1
	 */
	uint64_t sum = 0;
	for (uint32_t i = 0; i < n; i++) {
		sum += result[i];
		if (result[i] < 1) {
			abort(); /* invariant violation */
		}
	}
	if (sum != total) {
		abort(); /* invariant violation */
	}

	return 0;
}
