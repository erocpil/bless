#ifndef BLESS_ENTROPY_MATH_H
#define BLESS_ENTROPY_MATH_H

#include <math.h>
#include <stddef.h>
#include <stdint.h>

static inline int cmp_u32(const void *a, const void *b)
{
	uint32_t va = *(const uint32_t *)a, vb = *(const uint32_t *)b;
	return (va > vb) - (va < vb);
}

static inline int cmp_u16(const void *a, const void *b)
{
	uint16_t va = *(const uint16_t *)a, vb = *(const uint16_t *)b;
	return (va > vb) - (va < vb);
}

static inline int cmp_u64(const void *a, const void *b)
{
	uint64_t va = *(const uint64_t *)a, vb = *(const uint64_t *)b;
	return (va > vb) - (va < vb);
}

/* vals must be sorted with cmp. */
static inline double
shannon_from_sorted(const void *vals, size_t n, size_t elem_sz,
		    int (*cmp)(const void *, const void *), double *min_h)
{
	if (n < 2) {
		if (min_h) {
			*min_h = 0.0;
		}
		return 0.0;
	}

	double h = 0.0;
	double max_probability = 0.0;
	const char *p = (const char *)vals;
	size_t run = 1;

	for (size_t i = 1; i < n; i++) {
		if (cmp(p + (i - 1) * elem_sz, p + i * elem_sz) == 0) {
			run++;
		} else {
			double probability = (double)run / (double)n;
			h -= probability * log2(probability);
			if (probability > max_probability) {
				max_probability = probability;
			}
			run = 1;
		}
	}

	double probability = (double)run / (double)n;
	h -= probability * log2(probability);
	if (probability > max_probability) {
		max_probability = probability;
	}

	if (min_h) {
		*min_h = -log2(max_probability);
	}
	return h;
}

#endif
