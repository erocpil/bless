#ifndef __ENTROPY_DIAG_H__
#define __ENTROPY_DIAG_H__

#include <stddef.h>
#include <stdint.h>

#define ENTROPY_DIAG_MIN_SAMPLES 64

enum entropy_min_dim {
	ENTROPY_MIN_PROTOCOL = 0, ENTROPY_MIN_SRC_IP, ENTROPY_MIN_DST_IP,
	ENTROPY_MIN_SRC_PORT, ENTROPY_MIN_DST_PORT, ENTROPY_MIN_PKT_SIZE,
	ENTROPY_MIN_TCP_FLAGS, ENTROPY_MIN_DELTA_TSC, ENTROPY_MIN_OUTER_SRC_IP,
	ENTROPY_MIN_OUTER_DST_IP, ENTROPY_MIN_VNI, ENTROPY_MIN_TOTAL_5TUPLE,
	ENTROPY_MIN_DIM_COUNT
};

enum entropy_baseline_source {
	ENTROPY_BASELINE_UNAVAILABLE = 0, ENTROPY_BASELINE_CONFIGURED,
	ENTROPY_BASELINE_OBSERVED_SUPPORT, ENTROPY_BASELINE_MIXED
};

enum entropy_diagnostic_state {
	ENTROPY_DIAG_INACTIVE = 0, ENTROPY_DIAG_INSUFFICIENT_SAMPLES,
	ENTROPY_DIAG_INFORMATIONAL, ENTROPY_DIAG_GOOD,
	ENTROPY_DIAG_DEGRADED, ENTROPY_DIAG_POOR
};

struct entropy_min_diagnostic {
	double measured, target, population_target;
	double gap_bits, attainment, dominance_ratio;
	double max_probability;
	uint64_t samples, distinct, max_count;
	uint8_t baseline_source, state;
};

double entropy_min_sample_target(double population_target, size_t samples);
double entropy_weighted_pool_targets(const uint32_t *weights,
	const uint32_t *pool_sizes, size_t count, double *shannon_upper);
size_t entropy_count_distinct_u32(const uint32_t *values, size_t count);

void entropy_min_diag_fill_sorted(struct entropy_min_diagnostic *d,
	double measured, const void *sorted, size_t n, size_t width,
	int configured, double configured_target);

#endif
