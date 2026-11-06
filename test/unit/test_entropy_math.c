#include "entropy_math.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

int main(void)
{
	uint32_t skewed[] = {1, 1, 1, 1, 1, 1, 2, 3};
	double min_h = 0.0;
	double shannon = shannon_from_sorted(skewed, 8, sizeof(skewed[0]),
		cmp_u32, &min_h);

	assert(fabs(min_h - -log2(6.0 / 8.0)) < 1e-12);
	assert(min_h <= shannon);

	uint32_t uniform[] = {1, 2, 3, 4};
	shannon = shannon_from_sorted(uniform, 4, sizeof(uniform[0]),
		cmp_u32, &min_h);
	assert(fabs(shannon - 2.0) < 1e-12);
	assert(fabs(min_h - 2.0) < 1e-12);

	puts("entropy math: PASS");
	return 0;
}
