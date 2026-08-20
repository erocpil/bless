#include "entropy_sampler_policy.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int
main(void)
{
	uint32_t first = 100;
	uint64_t overwritten;

	assert(entropy_sampler_read_window(150, &first, 64, &overwritten) == 50);
	assert(first == 100);
	assert(overwritten == 0);

	uint8_t registers[ENTROPY_FLOW_HLL_REGISTERS];
	memset(registers, 0, sizeof(registers));
	assert(entropy_flow_hll_estimate(registers) == 0.0);
	for (uint64_t i = 0; i < 10000; i++) {
		uint64_t hash = entropy_sampler_mix64(i);
		uint32_t reg = hash & (ENTROPY_FLOW_HLL_REGISTERS - 1);
		uint8_t rank = entropy_flow_hll_rank(hash);
		if (rank > registers[reg]) {
			registers[reg] = rank;
		}
	}
	double estimate = entropy_flow_hll_estimate(registers);
	assert(estimate > 9000.0 && estimate < 11000.0);
	assert(entropy_flow_hll_bounded_estimate(registers, 9000) == 9000.0);
	assert(entropy_flow_hll_bounded_estimate(registers, 12000) == estimate);

	assert(!entropy_sampler_select(10, 20, 0));
	for (uint64_t i = 0; i < 128; i++)
		assert(entropy_sampler_select(i, 7, 1));

	uint32_t selected = 0;
	for (uint64_t i = 0; i < 17000; i++)
		selected += entropy_sampler_select(i, 42, 17);
	assert(selected > 850 && selected < 1150);

	first = 100;
	assert(entropy_sampler_read_window(200, &first, 64, &overwritten) == 64);
	assert(first == 136);
	assert(overwritten == 36);

	/* Unsigned index arithmetic also handles producer-index wraparound. */
	first = UINT32_MAX - 10;
	assert(entropy_sampler_read_window(9, &first, 64, &overwritten) == 20);
	assert(overwritten == 0);

	puts("entropy sampler policy: PASS");
	return 0;
}
