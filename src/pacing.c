#include "pacing.h"
#include "server.h"
#include "timing_policy.h"
#include "metric.h"
#include <string.h>
#include <rte_cycles.h>
#include <rte_pause.h>
#include <rte_mbuf_dyn.h>

static struct pacing_ctx *workers[RTE_MAX_LCORE];

static void update_max(atomic_uint_fast64_t *max_value, uint64_t value)
{
	uint64_t old = atomic_load(max_value);
	while (value > old && !atomic_compare_exchange_weak(
		max_value, &old, value)) { }
}

static void record(struct pacing_ctx *c, uint64_t overshoot, uint64_t duration)
{
	unsigned overshoot_bin = timing_histogram_bucket(overshoot,
		c->cycles_per_us);
	unsigned duration_bin = timing_histogram_bucket(duration,
		c->cycles_per_us);
	/* Publish maxima before counts. The reader drains bins before maxima, so
	 * every drained count is bounded by the max from the same or a wider set. */
	update_max(&c->overshoot_max, overshoot);
	update_max(&c->duration_max, duration);
	atomic_fetch_add(&c->overshoot_bins[overshoot_bin], 1);
	atomic_fetch_add(&c->duration_bins[duration_bin], 1);
}

void pacing_init(struct pacing_ctx *ctx, uint16_t port, uint16_t queue)
{
	memset(ctx, 0, sizeof(*ctx));
	ctx->port = port;
	ctx->queue = queue;
	ctx->cycles_per_us = rte_get_tsc_hz() / 1000000ULL;
	if (!ctx->cycles_per_us) {
		ctx->cycles_per_us = 1;
	}
	for (unsigned i = 0; i < PACING_HIST_BUCKETS; i++) {
		atomic_init(&ctx->overshoot_bins[i], 0);
		atomic_init(&ctx->duration_bins[i], 0);
	}
	atomic_init(&ctx->overshoot_max, 0);
	atomic_init(&ctx->duration_max, 0);
	atomic_init(&ctx->target_interval_tsc, 0);
}

void pacing_register(unsigned lcore, struct pacing_ctx *ctx)
{
	if (lcore < RTE_MAX_LCORE) {
		workers[lcore] = ctx;
	}
}

uint16_t pacing_submit(struct pacing_ctx *ctx, struct rte_mbuf **mbufs,
		       uint16_t count, uint64_t interval_us)
{
	uint64_t enter_tsc = rte_rdtsc();
	uint64_t deadline = ctx->next_deadline_tsc;
	while (deadline && rte_rdtsc() < deadline)
		rte_pause();
	uint64_t submit_tsc = rte_rdtsc();
	uint16_t sent = rte_eth_tx_burst(ctx->port, ctx->queue, mbufs, count);
	uint64_t complete_tsc = rte_rdtsc();
	if (deadline) {
		uint64_t overshoot = submit_tsc > deadline
			? submit_tsc - deadline : 0;
		record(ctx, overshoot, complete_tsc - submit_tsc);
	}
	/* Account the pacing busy-wait (enter->submit) and the PMD submit
	 * (submit->complete) as separate TSC budgets so per-packet "real
	 * send" cost can exclude the idle spin.  build is accounted by the
	 * worker; submit here is the rte_eth_tx_burst() duration. */
	metric_tx_timing_account(0, submit_tsc - enter_tsc,
				 complete_tsc - submit_tsc);
	uint64_t interval = interval_us * rte_get_tsc_hz() / 1000000ULL;
	atomic_store(&ctx->target_interval_tsc, interval);
	ctx->next_deadline_tsc = timing_next_deadline(deadline, complete_tsc,
		interval);
	return sent;
}

void pacing_fill_snapshot(struct stats_snapshot *s)
{
	uint64_t overshoot_bins[PACING_HIST_BUCKETS] = {0};
	uint64_t duration_bins[PACING_HIST_BUCKETS] = {0};
	uint64_t overshoot_samples = 0, duration_samples = 0;
	uint64_t overshoot_max = 0, duration_max = 0, target = 0;
	uint64_t cycles_per_us = 1;
	for (unsigned lc = 0; lc < RTE_MAX_LCORE; lc++) {
		struct pacing_ctx *ctx = workers[lc];
		if (!ctx) {
			continue;
		}
		cycles_per_us = ctx->cycles_per_us;
		for (unsigned i = 0; i < PACING_HIST_BUCKETS; i++) {
			uint64_t count = atomic_exchange(
				&ctx->overshoot_bins[i], 0);
			overshoot_bins[i] += count;
			overshoot_samples += count;
			count = atomic_exchange(&ctx->duration_bins[i], 0);
			duration_bins[i] += count;
			duration_samples += count;
		}
		uint64_t worker_max = atomic_exchange(&ctx->overshoot_max, 0);
		if (worker_max > overshoot_max) {
			overshoot_max = worker_max;
		}
		worker_max = atomic_exchange(&ctx->duration_max, 0);
		if (worker_max > duration_max) {
			duration_max = worker_max;
		}
		target = atomic_load(&ctx->target_interval_tsc);
	}
	double cpu = (double)cycles_per_us;
	s->tx_submit_target_us = target / cpu;
	s->tx_submit_overshoot_p50_us = timing_histogram_percentile(
		overshoot_bins, overshoot_samples, 50, cycles_per_us,
		overshoot_max) / cpu;
	s->tx_submit_overshoot_p99_us = timing_histogram_percentile(
		overshoot_bins, overshoot_samples, 99, cycles_per_us,
		overshoot_max) / cpu;
	s->tx_submit_overshoot_max_us = overshoot_max / cpu;
	s->tx_burst_duration_p50_us = timing_histogram_percentile(
		duration_bins, duration_samples, 50, cycles_per_us,
		duration_max) / cpu;
	s->tx_burst_duration_p99_us = timing_histogram_percentile(
		duration_bins, duration_samples, 99, cycles_per_us,
		duration_max) / cpu;
	s->tx_burst_duration_max_us = duration_max / cpu;
	s->tx_submit_samples = overshoot_samples;
}

int pacing_probe_mlx5(uint16_t port, struct pacing_hw_caps *caps)
{
	if (!caps) {
		return -1;
	}
	memset(caps, 0, sizeof(*caps));
	struct rte_eth_dev_info info;
	memset(&info, 0, sizeof(info));
	if (rte_eth_dev_info_get(port, &info) != 0 || !info.driver_name) {
		return -1;
	}
	caps->mlx5_driver = strstr(info.driver_name, "mlx5") != NULL;
	if (!caps->mlx5_driver) {
		return 0;
	}
#if defined(RTE_MBUF_DYNFIELD_TIMESTAMP_NAME) && defined(RTE_MBUF_DYNFLAG_TX_TIMESTAMP_NAME)
	int field = rte_mbuf_dynfield_lookup(RTE_MBUF_DYNFIELD_TIMESTAMP_NAME, NULL);
	if (field < 0) {
		const struct rte_mbuf_dynfield field_desc = {
			.name = RTE_MBUF_DYNFIELD_TIMESTAMP_NAME,
			.size = sizeof(uint64_t),
			.align = __alignof__(uint64_t),
		};
		field = rte_mbuf_dynfield_register(&field_desc);
	}
	int flag = rte_mbuf_dynflag_lookup(RTE_MBUF_DYNFLAG_TX_TIMESTAMP_NAME, NULL);
	if (flag < 0) {
		const struct rte_mbuf_dynflag flag_desc = {
			.name = RTE_MBUF_DYNFLAG_TX_TIMESTAMP_NAME,
		};
		flag = rte_mbuf_dynflag_register(&flag_desc);
	}
	caps->tx_timestamp_field = field >= 0;
	caps->tx_timestamp_flag = flag >= 0;
#endif
	/* Deliberately false until TSC/PHC -> mlx5 clock calibration lands.  Merely
	 * registering the standard mbuf metadata must never enable scheduling. */
	caps->clock_calibrated = 0;
	return 0;
}
