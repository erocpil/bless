#ifndef BLESS_CPU_USAGE_H
#define BLESS_CPU_USAGE_H

#include <stdbool.h>
#include <stdint.h>

struct cpu_usage_values {
	double process_cpu_cores;
	double enabled_lcore_utilization_ratio;
	double legacy_busy_pct;
};

static inline bool
cpu_usage_counter_delta(uint64_t current, uint64_t previous, uint64_t *delta)
{
	if (current < previous) {
		return false;
	}
	*delta = current - previous;
	return true;
}

static inline struct cpu_usage_values
cpu_usage_from_deltas(unsigned long long cpu_ns,
		      unsigned long long wall_ns,
		      unsigned int enabled_lcores)
{
	struct cpu_usage_values values = {0};
	unsigned int denominator = enabled_lcores ? enabled_lcores : 1;

	if (!wall_ns) {
		return values;
	}

	values.process_cpu_cores = (double)cpu_ns / (double)wall_ns;
	values.enabled_lcore_utilization_ratio =
		values.process_cpu_cores / (double)denominator;
	values.legacy_busy_pct =
		values.enabled_lcore_utilization_ratio * 100.0;
	if (values.legacy_busy_pct > 100.0) {
		values.legacy_busy_pct = 100.0;
	}
	return values;
}

#endif
