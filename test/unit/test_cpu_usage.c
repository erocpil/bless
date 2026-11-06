#include <math.h>
#include <stdio.h>

#include "cpu_usage.h"

static int failed;

#define CHECK(name, condition) do { \
	if (condition) printf("PASS: %s\n", name); \
	else { fprintf(stderr, "FAIL: %s\n", name); failed++; } \
} while (0)

int main(void)
{
	struct cpu_usage_values one_of_five =
		cpu_usage_from_deltas(1000000000ULL, 1000000000ULL, 5);
	CHECK("one process core", fabs(one_of_five.process_cpu_cores - 1.0) < 1e-12);
	CHECK("one of five ratio", fabs(one_of_five.enabled_lcore_utilization_ratio - 0.2) < 1e-12);
	CHECK("legacy one of five percent", fabs(one_of_five.legacy_busy_pct - 20.0) < 1e-12);

	struct cpu_usage_values three_of_five =
		cpu_usage_from_deltas(3000000000ULL, 1000000000ULL, 5);
	CHECK("three process cores", fabs(three_of_five.process_cpu_cores - 3.0) < 1e-12);
	CHECK("three of five ratio", fabs(three_of_five.enabled_lcore_utilization_ratio - 0.6) < 1e-12);

	struct cpu_usage_values extra_threads =
		cpu_usage_from_deltas(6000000000ULL, 1000000000ULL, 5);
	CHECK("ratio can exceed one", fabs(extra_threads.enabled_lcore_utilization_ratio - 1.2) < 1e-12);
	CHECK("legacy value remains capped", fabs(extra_threads.legacy_busy_pct - 100.0) < 1e-12);

	struct cpu_usage_values zero_wall = cpu_usage_from_deltas(1, 0, 0);
	CHECK("zero wall is safe", zero_wall.process_cpu_cores == 0.0);

	struct cpu_usage_values zero_lcores =
		cpu_usage_from_deltas(1000000000ULL, 1000000000ULL, 0);
	CHECK("zero enabled lcores fallback",
		fabs(zero_lcores.enabled_lcore_utilization_ratio - 1.0) < 1e-12);

	uint64_t delta = 0;
	CHECK("monotonic counter delta",
		cpu_usage_counter_delta(125, 100, &delta) && delta == 25);
	delta = 99;
	CHECK("counter regression resets baseline",
		!cpu_usage_counter_delta(90, 100, &delta));
	delta = 99;
	CHECK("equal counters yield zero delta",
		cpu_usage_counter_delta(100, 100, &delta) && delta == 0);
	return failed ? 1 : 0;
}
