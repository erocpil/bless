#ifndef __PACING_H__
#define __PACING_H__
#include <stdatomic.h>
#include <stdint.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>
#include "timing_policy.h"
struct stats_snapshot;
struct pacing_hw_caps {
	int mlx5_driver;
	int tx_timestamp_field;
	int tx_timestamp_flag;
	int clock_calibrated;
};
struct pacing_ctx {
	uint16_t port, queue;
	uint64_t next_deadline_tsc;
	uint64_t cycles_per_us;
	atomic_uint_fast64_t overshoot_bins[PACING_HIST_BUCKETS];
	atomic_uint_fast64_t duration_bins[PACING_HIST_BUCKETS];
	atomic_uint_fast64_t overshoot_max, duration_max;
	atomic_uint_fast64_t target_interval_tsc;
};
void pacing_init(struct pacing_ctx *, uint16_t, uint16_t);
void pacing_register(unsigned, struct pacing_ctx *);
uint16_t pacing_submit(struct pacing_ctx *, struct rte_mbuf **, uint16_t, uint64_t);
void pacing_fill_snapshot(struct stats_snapshot *);
int pacing_probe_mlx5(uint16_t port, struct pacing_hw_caps *caps);
#endif
