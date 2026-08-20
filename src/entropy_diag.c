#include "entropy_diag.h"

#include <math.h>
#include <string.h>

double
entropy_min_sample_target(double population_target, size_t samples)
{
	if (population_target <= 0.0 || !samples) {
		return 0.0;
	}

	double p = exp2(-population_target);
	double categories = ceil(1.0 / p);
	double log_limit = log(0.05) - log(categories);
	size_t first = (size_t)floor((double)samples * p) + 1;

	for (size_t count = first; count <= samples; count++) {
		double q = (double)count / (double)samples;
		double divergence = q * log(q / p);
		if (q < 1.0) {
			divergence += (1.0 - q) * log((1.0 - q) / (1.0 - p));
		}
		if (-(double)samples * divergence <= log_limit) {
			return -log2(q);
		}
	}
	return 0.0;
}

double
entropy_weighted_pool_targets(const uint32_t *weights,
	const uint32_t *pool_sizes, size_t count, double *shannon_upper)
{
	double total = 0.0;
	for (size_t i = 0; i < count; i++)
		total += weights[i];
	if (!total) {
		if (shannon_upper) {
			*shannon_upper = 0.0;
		}
		return 0.0;
	}

	double pmax = 0.0, h = 0.0;
	for (size_t i = 0; i < count; i++) {
		if (!weights[i]) {
			continue;
		}
		double mix = (double)weights[i] / total;
		double pool = pool_sizes[i] ? pool_sizes[i] : 1;
		pmax += mix / pool;
		h -= mix * log2(mix);
		h += mix * log2(pool);
	}
	if (shannon_upper) {
		*shannon_upper = h;
	}
	return -log2(fmin(1.0, pmax));
}

size_t
entropy_count_distinct_u32(const uint32_t *values, size_t count)
{
	size_t distinct = 0;
	for (size_t i = 0; i < count; i++) {
		size_t j = 0;
		for (; j < i; j++)
			if (values[j] == values[i]) {
				break;
			}
		if (j == i) {
			distinct++;
		}
	}
	return distinct;
}

void
entropy_min_diag_fill_sorted(struct entropy_min_diagnostic *d, double measured,
	const void *sorted, size_t n, size_t width,
	int configured, double configured_target)
{
	memset(d, 0, sizeof(*d));
	d->measured = measured;
	d->samples = n;
	if (!n) {
		d->state = ENTROPY_DIAG_INACTIVE;
		return;
	}
	const unsigned char *values = sorted;
	uint64_t run = 1;
	d->distinct = 1;
	d->max_count = 1;
	for (size_t i = 1; i < n; i++) {
		if (memcmp(values + (i - 1) * width, values + i * width, width) == 0) {
			run++;
		} else {
			if (run > d->max_count) {
				d->max_count = run;
			}
			run = 1;
			d->distinct++;
		}
	}
	if (run > d->max_count) {
		d->max_count = run;
	}
	d->max_probability = (double)d->max_count / (double)n;
	d->baseline_source = configured ? ENTROPY_BASELINE_CONFIGURED
		: ENTROPY_BASELINE_OBSERVED_SUPPORT;
	d->population_target = configured ? configured_target
		: log2((double)d->distinct);
	d->target = entropy_min_sample_target(d->population_target, n);
	d->gap_bits = fmax(0.0, d->target - measured);
	d->dominance_ratio = exp2(d->gap_bits);
	d->attainment = d->target > 0.0 ? fmin(1.0, measured / d->target) : 1.0;
	if (n < ENTROPY_DIAG_MIN_SAMPLES) {
		d->state = ENTROPY_DIAG_INSUFFICIENT_SAMPLES;
	} else if (!configured) {
		d->state = ENTROPY_DIAG_INFORMATIONAL;
	} else if (d->attainment >= 0.90) {
		d->state = ENTROPY_DIAG_GOOD;
	} else if (d->attainment >= 0.70) {
		d->state = ENTROPY_DIAG_DEGRADED;
	} else {
		d->state = ENTROPY_DIAG_POOR;
	}
}
