#include "entropy_diag.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>

static int failures;

#define CHECK(expr) do { if (!(expr)) { \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); failures++; \
} } while (0)

int main(void)
{
	struct entropy_min_diagnostic d;
	entropy_min_diag_fill_sorted(&d, 0.0, NULL, 0, sizeof(uint32_t), 0, 0.0);
	CHECK(d.state == ENTROPY_DIAG_INACTIVE);

	uint32_t small[] = { 1, 1, 2, 2 };
	entropy_min_diag_fill_sorted(&d, 1.0, small, 4, sizeof(small[0]), 1, 1.0);
	CHECK(d.state == ENTROPY_DIAG_INSUFFICIENT_SAMPLES);
	CHECK(d.distinct == 2 && d.max_count == 2);
	CHECK(fabs(d.max_probability - 0.5) < 1e-12);
	CHECK(d.target <= d.population_target);

	uint32_t skewed[64];
	for (unsigned i = 0; i < 64; i++) skewed[i] = i < 32 ? 0 : i;
	entropy_min_diag_fill_sorted(&d, 1.0, skewed, 64, sizeof(skewed[0]), 1, 4.0);
	CHECK(d.state == ENTROPY_DIAG_POOR);
	CHECK(d.population_target == 4.0);
	CHECK(d.target < d.population_target);

	uint32_t uniform[64];
	for (unsigned i = 0; i < 64; i++) uniform[i] = i;
	entropy_min_diag_fill_sorted(&d, 6.0, uniform, 64, sizeof(uniform[0]), 0, 0.0);
	CHECK(d.state == ENTROPY_DIAG_INFORMATIONAL);
	CHECK(d.baseline_source == ENTROPY_BASELINE_OBSERVED_SUPPORT);
	CHECK(fabs(d.population_target - 6.0) < 1e-12);
	CHECK(d.target < d.population_target);
	CHECK(entropy_min_sample_target(10.0, 650) < 10.0);
	CHECK(entropy_min_sample_target(10.0, 650) > 6.0);

	uint32_t weights[] = {20, 40, 40};
	uint32_t pools[] = {1, 1024, 2};
	double shannon_upper = 0.0;
	double population = entropy_weighted_pool_targets(weights, pools, 3,
		&shannon_upper);
	CHECK(fabs(population - -log2(0.2 + 0.4 / 1024.0 + 0.4 / 2.0)) < 1e-12);
	CHECK(shannon_upper > population);

	uint32_t repeated[] = { 7, 7, 9, 11, 9, 7 };
	CHECK(entropy_count_distinct_u32(repeated, 6) == 3);
	CHECK(entropy_count_distinct_u32(repeated, 0) == 0);

	printf("entropy diagnostics: %s\n", failures ? "FAIL" : "PASS");
	return failures ? 1 : 0;
}
