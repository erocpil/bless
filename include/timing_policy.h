#ifndef BLESS_TIMING_POLICY_H
#define BLESS_TIMING_POLICY_H

#include <stdint.h>

#define PACING_HIST_FINE_US 256U
#define PACING_HIST_MID_US 1024U
#define PACING_HIST_MID_STEP_US 8U
#define PACING_HIST_MID_BUCKETS \
	((PACING_HIST_MID_US - PACING_HIST_FINE_US) / PACING_HIST_MID_STEP_US)
#define PACING_HIST_LOG_BUCKETS 32U
#define PACING_HIST_BUCKETS \
	(PACING_HIST_FINE_US + PACING_HIST_MID_BUCKETS + PACING_HIST_LOG_BUCKETS)

/* Rebase a missed deadline on the actual submission time.  This prevents an
 * overloaded worker from accumulating an indefinitely stale schedule and
 * silently collapsing randomized pacing into a busy loop. */
static inline uint64_t
timing_next_deadline(uint64_t previous_deadline, uint64_t now,
		     uint64_t interval)
{
	if (!interval) {
		return 0;
	}
	uint64_t base = previous_deadline;
	if (!previous_deadline ||
	    (now > previous_deadline && now - previous_deadline >= interval)) {
		base = now;
	}
	return base + interval;
}

/* batch_delay and pps_rate are both upper bounds on sending speed, so the
 * larger (stricter) interval wins. */
static inline uint64_t
timing_effective_interval_us(uint64_t batch_delay_us, uint32_t pps_rate,
			     uint16_t batch)
{
	uint64_t rate_interval = pps_rate
		? (uint64_t)batch * 1000000ULL / pps_rate : 0;
	return rate_interval > batch_delay_us ? rate_interval : batch_delay_us;
}

/* Mixed-resolution histogram: 1 us buckets below 256 us, 8 us buckets below
 * 1024 us, then powers of two. Percentiles return a bucket upper bound capped
 * by the largest observed value, preserving pN <= max. */
static inline unsigned
timing_histogram_bucket(uint64_t value, uint64_t cycles_per_us)
{
	if (!cycles_per_us) {
		cycles_per_us = 1;
	}
	uint64_t us = value / cycles_per_us;
	if (us < PACING_HIST_FINE_US) {
		return (unsigned)us;
	}
	if (us < PACING_HIST_MID_US) {
		return PACING_HIST_FINE_US +
		       (unsigned)((us - PACING_HIST_FINE_US) /
				  PACING_HIST_MID_STEP_US);
	}

	uint64_t scaled = us / PACING_HIST_MID_US;
	unsigned log_bin = scaled > 0
		? 63U - (unsigned)__builtin_clzll(scaled) : 0;
	if (log_bin >= PACING_HIST_LOG_BUCKETS) {
		log_bin = PACING_HIST_LOG_BUCKETS - 1;
	}
	return PACING_HIST_FINE_US + PACING_HIST_MID_BUCKETS + log_bin;
}

static inline uint64_t
timing_histogram_upper_cycles(unsigned bucket, uint64_t cycles_per_us)
{
	if (!cycles_per_us) {
		cycles_per_us = 1;
	}
	uint64_t upper_us;
	if (bucket < PACING_HIST_FINE_US) {
		upper_us = (uint64_t)bucket + 1;
	} else if (bucket < PACING_HIST_FINE_US + PACING_HIST_MID_BUCKETS) {
		upper_us = PACING_HIST_FINE_US +
			(uint64_t)(bucket - PACING_HIST_FINE_US + 1) *
			PACING_HIST_MID_STEP_US;
	} else {
		unsigned shift = bucket - PACING_HIST_FINE_US -
			PACING_HIST_MID_BUCKETS + 1;
		upper_us = shift < 63 ? (uint64_t)PACING_HIST_MID_US << shift
			: UINT64_MAX;
	}
	if (upper_us == UINT64_MAX || upper_us > UINT64_MAX / cycles_per_us) {
		return UINT64_MAX;
	}
	return upper_us * cycles_per_us - 1;
}

static inline uint64_t
timing_histogram_percentile(const uint64_t bins[PACING_HIST_BUCKETS],
			    uint64_t total, unsigned percentile,
			    uint64_t cycles_per_us, uint64_t observed_max)
{
	if (!total) {
		return 0;
	}
	uint64_t goal = (total * percentile + 99) / 100;
	uint64_t seen = 0;
	for (unsigned i = 0; i < PACING_HIST_BUCKETS; i++) {
		seen += bins[i];
		if (seen >= goal) {
			uint64_t upper = timing_histogram_upper_cycles(i,
				cycles_per_us);
			return upper < observed_max ? upper : observed_max;
		}
	}
	return observed_max;
}

#endif /* BLESS_TIMING_POLICY_H */
