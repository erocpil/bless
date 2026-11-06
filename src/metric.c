#include "metric.h"
#include "cpu_usage.h"
#include "server.h"
#include <dirent.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>

/**
 * @file metric.c
 * @brief Runtime telemetry aggregation and Prometheus exposition.
 *
 * Collects per-port / per-worker stats (packet counters, latency
 * histograms, flow-state, entropy MI values) and serves them at
 * the /metrics endpoint.  All hot-path updates are lock-free
 * (atomic / per-lcore data structures).
 */

/* previous cumulative counters (not snapshot-bound) */
static uint64_t _prev_ipackets, _prev_opackets;
static uint64_t _prev_ibytes, _prev_obytes;
static uint64_t _prev_imissed;
static uint64_t _prev_tsc;
static uint64_t _prev_tx_build_cycles, _prev_tx_wait_cycles,
	_prev_tx_submit_cycles;

/* Software TX counters -- incremented by workers after rte_eth_tx_burst().
 * Virtual PMDs (net_null, net_pcap) don't update rte_eth_stats.opackets /
 * .obytes in hardware; these software counters fill the gap. */
static atomic_uint_fast64_t _sw_opackets;
static atomic_uint_fast64_t _sw_obytes;

/* TX hot-path timing breakdown (TSC cycles), accumulated by workers.
 * build  = packet construction, wait = pacing busy-wait,
 * submit = rte_eth_tx_burst() duration.  See metric_tx_timing_account(). */
static atomic_uint_fast64_t _tx_build_cycles;
static atomic_uint_fast64_t _tx_wait_cycles;
static atomic_uint_fast64_t _tx_submit_cycles;

static const char *const entropy_min_dim_names[ENTROPY_MIN_DIM_COUNT] = {
	"protocol", "src_ip", "dst_ip", "src_port", "dst_port", "pkt_size",
	"tcp_flags", "delta_tsc", "outer_src_ip", "outer_dst_ip", "vni",
	"total_5tuple"
};

static const char *entropy_baseline_name(uint8_t source)
{
	switch (source) {
	case ENTROPY_BASELINE_CONFIGURED: return "configured";
	case ENTROPY_BASELINE_OBSERVED_SUPPORT: return "observed-support";
	case ENTROPY_BASELINE_MIXED: return "mixed";
	default: return "unavailable";
	}
}

static const char *entropy_diag_state_name(uint8_t state)
{
	switch (state) {
	case ENTROPY_DIAG_INSUFFICIENT_SAMPLES: return "insufficient-samples";
	case ENTROPY_DIAG_INFORMATIONAL: return "informational";
	case ENTROPY_DIAG_GOOD: return "good";
	case ENTROPY_DIAG_DEGRADED: return "degraded";
	case ENTROPY_DIAG_POOR: return "poor";
	default: return "inactive";
	}
}

/** Record one successful TX burst (called from worker data-plane).
 *  count = number of mbufs sent, bytes = sum of pkt_len. */
void metric_tx_account(uint64_t count, uint64_t bytes)
{
	atomic_fetch_add(&_sw_opackets, count);
	atomic_fetch_add(&_sw_obytes,   bytes);
}

void metric_tx_timing_account(uint64_t build_cycles, uint64_t wait_cycles,
			      uint64_t submit_cycles)
{
	if (build_cycles) {
		atomic_fetch_add(&_tx_build_cycles, build_cycles);
	}
	if (wait_cycles) {
		atomic_fetch_add(&_tx_wait_cycles, wait_cycles);
	}
	if (submit_cycles) {
		atomic_fetch_add(&_tx_submit_cycles, submit_cycles);
	}
}

void compute_rate_metrics(struct stats_snapshot *s, uint32_t port_mask)
{
	uint64_t ipackets = 0, opackets = 0;
	uint64_t ibytes = 0, obytes = 0;
	uint64_t imissed = 0;

	uint16_t portid;
	RTE_ETH_FOREACH_DEV(portid) {
		if ((port_mask & (1u << portid)) == 0) {
			continue;
		}
		struct rte_eth_stats stats;
		if (rte_eth_stats_get(portid, &stats) != 0) {
			continue;
		}
		ipackets += stats.ipackets;
		opackets += stats.opackets;
		ibytes   += stats.ibytes;
		obytes   += stats.obytes;
		imissed  += stats.imissed;
	}

	/* Virtual PMDs don't update hardware TX counters -- fall back
	 * to software counters collected via metric_tx_account(). */
	uint64_t hw_opackets = opackets;
	uint64_t hw_obytes   = obytes;
	uint64_t sw_op = atomic_load(&_sw_opackets);
	uint64_t sw_ob = atomic_load(&_sw_obytes);
	uint64_t eff_opackets = hw_opackets > sw_op ? hw_opackets : sw_op;
	uint64_t eff_obytes   = hw_obytes   > sw_ob ? hw_obytes   : sw_ob;

	uint64_t now = rte_rdtsc();
	uint64_t build_cyc = atomic_load(&_tx_build_cycles);
	uint64_t wait_cyc = atomic_load(&_tx_wait_cycles);
	uint64_t submit_cyc = atomic_load(&_tx_submit_cycles);
	if (_prev_tsc == 0) {
		/* first call – seed prev, zero rates */
		goto store;
	}

	double dt_sec = (double)(now - _prev_tsc) / (double)rte_get_tsc_hz();
	if (dt_sec < 0.000001) {
		dt_sec = 0.000001; /* avoid division by zero */
	}

	s->rx_mpps = (double)(ipackets - _prev_ipackets) / dt_sec / 1e6;
	s->rx_gbps = (double)(ibytes   - _prev_ibytes)   * 8.0 / dt_sec / 1e9;

	s->tx_mpps = (double)(eff_opackets - _prev_opackets) / dt_sec / 1e6;
	s->tx_gbps = (double)(eff_obytes   - _prev_obytes)   * 8.0 / dt_sec / 1e9;

	/* Per-packet TX timing breakdown.  The windowed packet count is the
	 * same delta that drives tx_mpps, so cycles-per-packet is computed
	 * over the matching window.  build + submit = real send cost (the
	 * pacing busy-wait is excluded); wait_ratio quantifies that idle. */
	uint64_t d_pkts = eff_opackets - _prev_opackets;
	uint64_t d_build = build_cyc - _prev_tx_build_cycles;
	uint64_t d_wait = wait_cyc - _prev_tx_wait_cycles;
	uint64_t d_submit = submit_cyc - _prev_tx_submit_cycles;
	if (d_pkts > 0) {
		s->tx_build_cycles_per_pkt = (double)d_build / (double)d_pkts;
		s->tx_submit_cycles_per_pkt = (double)d_submit / (double)d_pkts;
		s->tx_cycles_per_pkt =
			(double)(d_build + d_submit) / (double)d_pkts;
	} else {
		s->tx_build_cycles_per_pkt = 0.0;
		s->tx_submit_cycles_per_pkt = 0.0;
		s->tx_cycles_per_pkt = 0.0;
	}
	uint64_t d_total = d_build + d_wait + d_submit;
	s->tx_wait_ratio = d_total > 0 ? (double)d_wait / (double)d_total : 0.0;

	uint64_t rx_total = (ipackets - _prev_ipackets)
		+ (imissed - _prev_imissed);
	if (rx_total > 0) {
		s->rx_loss_rate = (double)(imissed - _prev_imissed)
			/ (double)rx_total;
	}

store:
	_prev_ipackets = ipackets;
	_prev_opackets = eff_opackets;
	_prev_ibytes   = ibytes;
	_prev_obytes   = eff_obytes;
	_prev_imissed  = imissed;
	_prev_tsc      = now;
	_prev_tx_build_cycles  = build_cyc;
	_prev_tx_wait_cycles   = wait_cyc;
	_prev_tx_submit_cycles = submit_cyc;
}

/* Sum utime+stime (clock ticks) across all threads via /proc/self/task.
 * This matches the /proc-based accounting used by the external validation
 * script and is unaffected by the batched per-thread CPU accounting that
 * CLOCK_PROCESS_CPUTIME_ID exhibits on nohz_full (isolcpus) hosts.
 *
 * Returns total ticks, or UINT64_MAX if /proc cannot be read. */
static uint64_t proc_self_cpu_ticks(void)
{
	DIR *d = opendir("/proc/self/task");
	if (!d) {
		return UINT64_MAX;
	}

	uint64_t total = 0;
	struct dirent *ent;
	while ((ent = readdir(d)) != NULL) {
		if (ent->d_name[0] == '.') {
			continue;
		}
		char path[64];
		snprintf(path, sizeof(path), "/proc/self/task/%s/stat",
			 ent->d_name);
		FILE *f = fopen(path, "r");
		if (!f) {
			continue;
		}
		char line[1024];
		if (fgets(line, sizeof(line), f)) {
			/* comm may contain spaces/parens — anchor past the
			 * last ')'.  Fields after it start at proc stat
			 * field 3 (state); utime/stime are fields 14/15
			 * (index 11/12 in the split remainder). */
			char *rp = strrchr(line, ')');
			if (rp) {
				unsigned long long utime = 0, stime = 0;
				if (sscanf(rp + 2,
					   "%*c %*d %*d %*d %*d %*d "
					   "%*u %*lu %*lu %*lu %*lu "
					   "%llu %llu",
					   &utime, &stime) == 2) {
					total += utime + stime;
				}
			}
		}
		fclose(f);
	}
	closedir(d);
	return total;
}

void sample_cpu_usage(struct stats_snapshot *s)
{
	static uint64_t prev_cpu_ticks = 0, prev_wall_ns = 0;
	static double last_busy_pct = 0.0;
	static double last_process_cpu_cores = 0.0;
	static double last_enabled_lcore_utilization_ratio = 0.0;
	static uint32_t last_enabled_lcores = 1;

	/* Every generated snapshot must carry the last complete >=1s sample.
	 * Stats are double-buffered and may be generated every 100ms, so leaving
	 * these fields untouched would alternate stale values between buffers. */
	s->process_cpu_cores = last_process_cpu_cores;
	s->enabled_lcores = last_enabled_lcores;
	s->enabled_lcore_utilization_ratio =
		last_enabled_lcore_utilization_ratio;
	s->cpu_busy_pct = last_busy_pct;

	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	uint64_t wall_ns = (uint64_t)ts.tv_sec * 1000000000ULL
		+ (uint64_t)ts.tv_nsec;

	uint64_t cpu_ticks = proc_self_cpu_ticks();
	if (cpu_ticks == UINT64_MAX) {
		/* /proc read failed — keep the previous value rather than
		 * report a misleading 0. */
		return;
	}

	if (prev_wall_ns == 0) {
		/* first call — seed the baseline, report 0 */
		prev_cpu_ticks = cpu_ticks;
		prev_wall_ns = wall_ns;
		last_enabled_lcores = rte_lcore_count();
		if (!last_enabled_lcores) {
			last_enabled_lcores = 1;
		}
		s->enabled_lcores = last_enabled_lcores;
		return;
	}

	/* Summing /proc/self/task counters is not monotonic when a thread exits:
	 * its accumulated ticks disappear from the next sum.  Reset the baseline
	 * instead of allowing the unsigned subtraction to wrap. */
	uint64_t d_cpu;
	if (wall_ns < prev_wall_ns ||
	    !cpu_usage_counter_delta(cpu_ticks, prev_cpu_ticks, &d_cpu)) {
		prev_cpu_ticks = cpu_ticks;
		prev_wall_ns = wall_ns;
		return;
	}

	uint64_t d_wall = wall_ns - prev_wall_ns;
	if (d_wall == 0) {
		return;
	}

	/* Recompute only on a >=1s window.  Under nohz_full (isolcpus) the
	 * kernel batches per-thread CPU accounting, so sub-second deltas
	 * miss the busy-polling lcore threads entirely.  A 1s window lets
	 * the batched updates land and be averaged. */
	if (d_wall >= 1000000000ULL) {
		long clk_tck = sysconf(_SC_CLK_TCK);
		if (clk_tck <= 0) {
			clk_tck = 100;
		}
		/* TODO(metric-semantics): the divisor is rte_lcore_count() — the
	 * number of *enabled* lcores (here 5: 0-4), NOT the number of
	 * lcores actually doing work.  With one busy-polling worker plus an
	 * idle control lcore this reads ~1.0 core / 5 = 20%, which a reader
	 * may mistake for "80% idle" when the single TX worker is in fact
	 * fully busy.  A clearer absolute metric is the uncapped core count
	 * (cpu_sec / wall_sec, no division by cores), exposed by the external
	 * validation script as process_cpu_cores_used.  Keep the existing
	 * normalized definition for backward compatibility, but do not read
	 * it as spare capacity. */
		unsigned cores = rte_lcore_count();
		if (!cores) {
			cores = 1;
		}
		uint64_t cpu_ns = d_cpu * 1000000000ULL / clk_tck;
		struct cpu_usage_values usage =
			cpu_usage_from_deltas(cpu_ns, d_wall, cores);
		last_process_cpu_cores = usage.process_cpu_cores;
		last_enabled_lcores = cores;
		last_enabled_lcore_utilization_ratio =
			usage.enabled_lcore_utilization_ratio;
		last_busy_pct = usage.legacy_busy_pct;

		prev_cpu_ticks = cpu_ticks;
		prev_wall_ns = wall_ns;
	}

	s->process_cpu_cores = last_process_cpu_cores;
	s->enabled_lcores = last_enabled_lcores;
	s->enabled_lcore_utilization_ratio =
		last_enabled_lcore_utilization_ratio;
	s->cpu_busy_pct = last_busy_pct;
}

void sample_memory_usage(struct stats_snapshot *s)
{
	FILE *f = fopen("/proc/self/status", "r");
	if (!f) {
		return;
	}
	char line[256];
	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "VmRSS: %lu kB", &s->mem_rss_kb) == 1) {
			break;
		}
	}
	fclose(f);
}

void sample_runtime_noise(struct stats_snapshot *s)
{
	s->cpu_freq_min_khz = UINT64_MAX;
	s->cpu_freq_max_khz = 0;
	for (unsigned lc = 0; lc < RTE_MAX_LCORE; lc++) {
		if (!rte_lcore_is_enabled(lc)) {
			continue;
		}
		unsigned cpu = rte_lcore_to_cpu_id(lc);
		char path[160];
		snprintf(path, sizeof(path),
			 "/sys/devices/system/cpu/cpu%u/cpufreq/scaling_cur_freq", cpu);
		FILE *f = fopen(path, "r");
		uint64_t freq;
		if (f && fscanf(f, "%lu", &freq) == 1) {
			if (freq < s->cpu_freq_min_khz) {
				s->cpu_freq_min_khz = freq;
			}
			if (freq > s->cpu_freq_max_khz) {
				s->cpu_freq_max_khz = freq;
			}
		}
		if (f) {
			fclose(f);
		}
	}
	if (s->cpu_freq_min_khz == UINT64_MAX) {
		s->cpu_freq_min_khz = 0;
	}
	FILE *status = fopen("/proc/self/status", "r");
	if (!status) {
		return;
	}
	char line[256];
	while (fgets(line, sizeof(line), status)) {
		(void)sscanf(line, "voluntary_ctxt_switches: %lu",
			&s->voluntary_ctx_switches);
		(void)sscanf(line, "nonvoluntary_ctxt_switches: %lu",
			&s->involuntary_ctx_switches);
	}
	fclose(status);
}

#define APPEND(fmt, ...) do {                                     \
	if (len >= max_len) {                                         \
		return max_len;                                           \
	}                                                             \
	int n = snprintf(msg + len, max_len - len, fmt, __VA_ARGS__); \
	if (n < 0) {                                                  \
		return len;                                               \
	}                                                             \
	if ((size_t)n >= max_len - len) {                             \
		len = max_len;                                            \
		return len;                                               \
	}                                                             \
	len += (size_t)n;                                             \
} while (0)

void* (*cbfn)(void) = NULL;

void metric_set_cbfn(void*(*metric_cbfn)())
{
	cbfn = metric_cbfn;
}

void metric_cpuset_to_str(const cpu_set_t *set, char *buf, size_t len)
{
	int first = 1;
	int start = -1;
	size_t off = 0;

	for (int cpu = 0; cpu <= CPU_SETSIZE; cpu++) {
		int is_set = (cpu < CPU_SETSIZE) && CPU_ISSET(cpu, set);

		if (is_set) {
			if (start < 0) {
				start = cpu;
			}
		} else if (start >= 0) {
			int end = cpu - 1;
			int n;

			if (!first) {
				n = snprintf(buf + off, len - off, ",");
			} else {
				n = 0;
			}

			off += n;

			if (start == end) {
				off += snprintf(buf + off, len - off, "%d", start);
			} else {
				off += snprintf(buf + off, len - off, "%d-%d", start, end);
			}

			first = 0;
			start = -1;
		}
	}
}

int bless_handle_system(const char *cmd, const char *params __rte_unused, struct rte_tel_data *d)
{
	RTE_SET_USED(cmd);
	RTE_SET_USED(params);

	struct system_status *sysstat = cbfn();
	rte_tel_data_start_dict(d);
	rte_tel_data_add_dict_int(d, "ppid", sysstat->ppid);
	rte_tel_data_add_dict_int(d, "pid", sysstat->pid);

	struct rte_tel_data *arr;
	arr = rte_tel_data_alloc();
	rte_tel_data_start_array(arr, RTE_TEL_UINT_VAL);
	for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
		if (CPU_ISSET(cpu, &sysstat->cpuset)) {
			rte_tel_data_add_array_uint(arr, cpu);
		}
	}
	rte_tel_data_add_dict_container(d, "cpu_affinity", arr, 0);

	return 0;
}

size_t encode_stats_to_text(uint32_t port_mask, char *msg, size_t max_len,
		const struct stats_snapshot *ent)
{
	size_t len = 0;
	uint16_t portid;
	RTE_ETH_FOREACH_DEV(portid) {
		if ((port_mask & (1u << portid)) == 0) {
			continue;
		}
		struct rte_eth_stats stats;
		if (rte_eth_stats_get(portid, &stats) != 0) {
			return 0;
		}

		APPEND("dpdk_ipackets{port=\"%u\"} %" PRIu64 "\n",
				portid, stats.ipackets);
		APPEND("dpdk_opackets{port=\"%u\"} %" PRIu64 "\n",
				portid, stats.opackets);
		APPEND("dpdk_ibytes{port=\"%u\"} %" PRIu64 "\n",
				portid, stats.ibytes);
		APPEND("dpdk_obytes{port=\"%u\"} %" PRIu64 "\n",
				portid, stats.obytes);
		APPEND("dpdk_imissed{port=\"%u\"} %" PRIu64 "\n",
				portid, stats.imissed);
		APPEND("dpdk_ierrors{port=\"%u\"} %" PRIu64 "\n",
				portid, stats.ierrors);
		APPEND("dpdk_oerrors{port=\"%u\"} %" PRIu64 "\n",
				portid, stats.oerrors);
		APPEND("dpdk_rx_nobuf{port=\"%u\"} %" PRIu64 "\n",
				portid, stats.rx_nombuf);
	}

	/* entropy metrics (runtime-sampled, unified label-based format) */
	if (ent) {
		APPEND("bless_entropy{type=\"protocol\"} %f\n",      ent->entropy_protocol);
		APPEND("bless_entropy{type=\"src_ip\"} %f\n",        ent->entropy_src_ip);
		APPEND("bless_entropy{type=\"dst_ip\"} %f\n",        ent->entropy_dst_ip);
		APPEND("bless_entropy{type=\"src_port\"} %f\n",      ent->entropy_src_port);
		APPEND("bless_entropy{type=\"dst_port\"} %f\n",      ent->entropy_dst_port);
		APPEND("bless_entropy{type=\"pkt_size\"} %f\n",      ent->entropy_pkt_size);
		APPEND("bless_entropy{type=\"vxlan_encap\"} %f\n",   ent->entropy_vxlan_encap);
		APPEND("bless_entropy{type=\"outer_src_ip\"} %f\n",  ent->entropy_outer_src_ip);
		APPEND("bless_entropy{type=\"outer_dst_ip\"} %f\n",  ent->entropy_outer_dst_ip);
		APPEND("bless_entropy{type=\"outer_src_port\"} %f\n", ent->entropy_outer_src_port);
		APPEND("bless_entropy{type=\"vni\"} %f\n",           ent->entropy_vni);
		APPEND("bless_entropy{type=\"tcp_flags\"} %f\n",     ent->entropy_tcp_flags);
		APPEND("bless_entropy{type=\"total_5tuple\"} %f\n",  ent->entropy_total_5tuple);

		/* P0a: timing entropy */
		APPEND("bless_entropy{type=\"delta_tsc\"} %f\n",    ent->entropy_delta_tsc);

		/* P0b: distinct flow count */
		APPEND("bless_flow{type=\"distinct\"} %.0f\n",      ent->flow_distinct);
		APPEND("bless_flow{type=\"total\"} %.0f\n",          ent->flow_total);
		APPEND("bless_flow{type=\"ratio\"} %f\n",            ent->flow_ratio);
		APPEND("bless_sampler_samples %.0f\n", ent->sampler_samples);
		APPEND("bless_sampler_overwritten_total %.0f\n",
			ent->sampler_overwritten);
		APPEND("bless_sampler_overwritten_window %.0f\n",
			ent->sampler_overwritten_window);

		/* flow-level entropy (handshake mode) */
		APPEND("bless_flow_entropy{type=\"5tuple\"} %f\n",    ent->flow_entropy_5tuple);
		APPEND("bless_flow_entropy{type=\"lifetime\"} %f\n",  ent->flow_entropy_lifetime);
		APPEND("bless_flow_entropy{type=\"event\"} %f\n",     ent->flow_entropy_event);
		APPEND("bless_flow_entropy_samples{type=\"5tuple\"} %.0f\n", ent->flow_count);
		APPEND("bless_flow_entropy_samples{type=\"lifetime\"} %.0f\n", ent->flow_lifetime_count);
		APPEND("bless_flow_entropy_samples{type=\"event\"} %.0f\n", ent->flow_event_count);

		/* P0c: usage ratio (max possible) */
		APPEND("bless_entropy_max{type=\"src_ip\"} %f\n",    ent->max_src_ip);
		APPEND("bless_entropy_max{type=\"dst_ip\"} %f\n",    ent->max_dst_ip);
		APPEND("bless_entropy_max{type=\"src_port\"} %f\n",  ent->max_src_port);
		APPEND("bless_entropy_max{type=\"dst_port\"} %f\n",  ent->max_dst_port);
		APPEND("bless_entropy_max{type=\"outer_src_ip\"} %f\n", ent->max_outer_src_ip);
		APPEND("bless_entropy_max{type=\"outer_dst_ip\"} %f\n", ent->max_outer_dst_ip);
		APPEND("bless_entropy_max{type=\"vni\"} %f\n",          ent->max_vni);

		/* P1: min-entropy per dimension */
		APPEND("bless_entropy_min{type=\"protocol\"} %f\n",  ent->entropy_min_protocol);
		APPEND("bless_entropy_min{type=\"src_ip\"} %f\n",    ent->entropy_min_src_ip);
		APPEND("bless_entropy_min{type=\"dst_ip\"} %f\n",    ent->entropy_min_dst_ip);
		APPEND("bless_entropy_min{type=\"src_port\"} %f\n",  ent->entropy_min_src_port);
		APPEND("bless_entropy_min{type=\"dst_port\"} %f\n",  ent->entropy_min_dst_port);
		APPEND("bless_entropy_min{type=\"pkt_size\"} %f\n",  ent->entropy_min_pkt_size);
		APPEND("bless_entropy_min{type=\"tcp_flags\"} %f\n", ent->entropy_min_tcp_flags);
		APPEND("bless_entropy_min{type=\"delta_tsc\"} %f\n", ent->entropy_min_delta_tsc);
		APPEND("bless_entropy_min{type=\"outer_src_ip\"} %f\n", ent->entropy_min_outer_src_ip);
		APPEND("bless_entropy_min{type=\"outer_dst_ip\"} %f\n", ent->entropy_min_outer_dst_ip);
		APPEND("bless_entropy_min{type=\"vni\"} %f\n",          ent->entropy_min_vni);
		APPEND("bless_entropy_min{type=\"total_5tuple\"} %f\n", ent->entropy_min_total_5tuple);
		for (unsigned i = 0; i < ENTROPY_MIN_DIM_COUNT; i++) {
			const struct entropy_min_diagnostic *d = &ent->min_diag[i];
			APPEND("bless_entropy_min_target{type=\"%s\",source=\"%s\"} %f\n",
				entropy_min_dim_names[i], entropy_baseline_name(d->baseline_source),
				d->target);
			APPEND("bless_entropy_min_population_target{type=\"%s\"} %f\n",
				entropy_min_dim_names[i], d->population_target);
			APPEND("bless_entropy_min_gap_bits{type=\"%s\"} %f\n",
				entropy_min_dim_names[i], d->gap_bits);
			APPEND("bless_entropy_min_attainment_ratio{type=\"%s\"} %f\n",
				entropy_min_dim_names[i], d->attainment);
			APPEND("bless_entropy_min_dominance_ratio{type=\"%s\"} %f\n",
				entropy_min_dim_names[i], d->dominance_ratio);
			APPEND("bless_entropy_min_samples{type=\"%s\"} %.0f\n",
				entropy_min_dim_names[i], (double)d->samples);
			APPEND("bless_entropy_min_distinct{type=\"%s\"} %.0f\n",
				entropy_min_dim_names[i], (double)d->distinct);
			APPEND("bless_entropy_min_max_probability{type=\"%s\"} %f\n",
				entropy_min_dim_names[i], d->max_probability);
			APPEND("bless_entropy_min_state{type=\"%s\",state=\"%s\"} 1\n",
				entropy_min_dim_names[i], entropy_diag_state_name(d->state));
		}

		/* P1: joint 5-tuple entropy */
		APPEND("bless_entropy{type=\"joint_5tuple\"} %f\n",  ent->entropy_joint_5tuple);

		/* handshake mode stats */
		if (ent->hs_syn_sent > 0 || ent->hs_conn_current > 0) {
			APPEND("bless_hs_syn_sent_total %.0f\n",      ent->hs_syn_sent);
			APPEND("bless_hs_syn_recv_total %.0f\n",      ent->hs_syn_recv);
			APPEND("bless_hs_synack_sent_total %.0f\n",   ent->hs_synack_sent);
			APPEND("bless_hs_synack_recv_total %.0f\n",   ent->hs_synack_recv);
			APPEND("bless_hs_ack_sent_total %.0f\n",      ent->hs_ack_sent);
			APPEND("bless_hs_established_total %.0f\n",    ent->hs_established);
			APPEND("bless_hs_rst_sent_total %.0f\n",       ent->hs_rst_sent);
			APPEND("bless_hs_rst_recv_total %.0f\n",       ent->hs_rst_recv);
			APPEND("bless_hs_timed_out_total %.0f\n",      ent->hs_timed_out);
			APPEND("bless_hs_conn_current %.0f\n",         ent->hs_conn_current);
			APPEND("bless_hs_conn_max %.0f\n",             ent->hs_conn_max);
			APPEND("bless_hs_success_rate %f\n",           ent->hs_success_rate);
			APPEND("bless_hs_cps %f\n",                    ent->hs_cps);
		}
	}

	/* observe: throughput, loss, CPU, memory (always present) */
	APPEND("bless_rx_mpps %f\n",     ent->rx_mpps);
	APPEND("bless_tx_mpps %f\n",     ent->tx_mpps);
	APPEND("bless_rx_gbps %f\n",     ent->rx_gbps);
	APPEND("bless_tx_gbps %f\n",     ent->tx_gbps);
	APPEND("bless_rx_loss_rate %f\n", ent->rx_loss_rate);
	APPEND("%s", "# HELP bless_process_cpu_cores Process CPU seconds consumed per wall-clock second.\n");
	APPEND("%s", "# TYPE bless_process_cpu_cores gauge\n");
	APPEND("bless_process_cpu_cores %f\n", ent->process_cpu_cores);
	APPEND("%s", "# HELP bless_enabled_lcores Enabled DPDK lcores used as the CPU normalization denominator.\n");
	APPEND("%s", "# TYPE bless_enabled_lcores gauge\n");
	APPEND("bless_enabled_lcores %u\n", ent->enabled_lcores);
	APPEND("%s", "# HELP bless_enabled_lcore_utilization_ratio Process CPU cores divided by enabled DPDK lcores.\n");
	APPEND("%s", "# TYPE bless_enabled_lcore_utilization_ratio gauge\n");
	APPEND("bless_enabled_lcore_utilization_ratio %f\n",
		ent->enabled_lcore_utilization_ratio);
	APPEND("%s", "# HELP bless_cpu_busy_pct Deprecated enabled-lcore utilization percentage; use bless_process_cpu_cores and bless_enabled_lcore_utilization_ratio.\n");
	APPEND("%s", "# TYPE bless_cpu_busy_pct gauge\n");
	APPEND("bless_cpu_busy_pct %f\n", ent->cpu_busy_pct);
	APPEND("bless_tx_build_cycles_per_pkt %f\n", ent->tx_build_cycles_per_pkt);
	APPEND("bless_tx_submit_cycles_per_pkt %f\n", ent->tx_submit_cycles_per_pkt);
	APPEND("bless_tx_cycles_per_pkt %f\n", ent->tx_cycles_per_pkt);
	APPEND("bless_tx_wait_ratio %f\n", ent->tx_wait_ratio);
	APPEND("bless_mem_rss_kb %.0f\n", (double)ent->mem_rss_kb);
	APPEND("bless_cpu_frequency_khz{type=\"min\"} %.0f\n", (double)ent->cpu_freq_min_khz);
	APPEND("bless_cpu_frequency_khz{type=\"max\"} %.0f\n", (double)ent->cpu_freq_max_khz);
	APPEND("bless_context_switches_total{type=\"voluntary\"} %.0f\n", (double)ent->voluntary_ctx_switches);
	APPEND("bless_context_switches_total{type=\"involuntary\"} %.0f\n", (double)ent->involuntary_ctx_switches);
	APPEND("bless_tx_submit_target_us %f\n", ent->tx_submit_target_us);
	APPEND("bless_tx_submit_overshoot_us{quantile=\"0.50\"} %f\n",
		ent->tx_submit_overshoot_p50_us);
	APPEND("bless_tx_submit_overshoot_us{quantile=\"0.99\"} %f\n",
		ent->tx_submit_overshoot_p99_us);
	APPEND("bless_tx_submit_overshoot_max_us %f\n",
		ent->tx_submit_overshoot_max_us);
	APPEND("bless_tx_burst_duration_us{quantile=\"0.50\"} %f\n",
		ent->tx_burst_duration_p50_us);
	APPEND("bless_tx_burst_duration_us{quantile=\"0.99\"} %f\n",
		ent->tx_burst_duration_p99_us);
	APPEND("bless_tx_burst_duration_max_us %f\n",
		ent->tx_burst_duration_max_us);
	APPEND("bless_tx_submit_samples %.0f\n", (double)ent->tx_submit_samples);
	APPEND("bless_psd_dominant_hz %f\n", ent->psd_dominant_hz);
	APPEND("bless_psd_strongest_peak_hz %f\n", ent->psd_strongest_peak_hz);
	APPEND("bless_psd_fundamental_hz %f\n", ent->psd_fundamental_hz);
	APPEND("bless_psd_spectral_flatness %f\n", ent->psd_spectral_flatness);
	APPEND("bless_psd_mean_ppms %f\n", ent->psd_mean_ppms);
	APPEND("bless_psd_variation_rms_ppms %f\n", ent->psd_variation_rms_ppms);
	APPEND("bless_psd_signal_valid %d\n", ent->psd_signal_valid);

	/* latency histogram percentiles */
	APPEND("bless_lat_p50_us %.0f\n",  ent->lat_p50);
	APPEND("bless_lat_p95_us %.0f\n",  ent->lat_p95);
	APPEND("bless_lat_p99_us %.0f\n",  ent->lat_p99);
	APPEND("bless_lat_p999_us %.0f\n", ent->lat_p999);
	APPEND("bless_lat_samples %.0f\n", (double)ent->lat_samples);

	return len;
}

static cJSON * encode_eth_stats(const struct rte_eth_stats *s)
{
	cJSON *obj = cJSON_CreateObject();

	cJSON_AddNumberToObject(obj, "ipackets", s->ipackets);
	cJSON_AddNumberToObject(obj, "opackets", s->opackets);
	cJSON_AddNumberToObject(obj, "ibytes",   s->ibytes);
	cJSON_AddNumberToObject(obj, "obytes",   s->obytes);
	cJSON_AddNumberToObject(obj, "imissed",  s->imissed);
	cJSON_AddNumberToObject(obj, "ierrors",  s->ierrors);
	cJSON_AddNumberToObject(obj, "oerrors",  s->oerrors);
	cJSON_AddNumberToObject(obj, "rx_nombuf", s->rx_nombuf);

	return obj;
}

static cJSON * encode_xstats(uint16_t portid)
{
	int n = rte_eth_xstats_get_names(portid, NULL, 0);
	if (n <= 0) {
		return NULL;
	}

	struct rte_eth_xstat_name *names = rte_malloc(NULL, sizeof(*names) * n, 0);
	struct rte_eth_xstat *values = rte_malloc(NULL, sizeof(*values) * n, 0);

	if (!names || !values) {
		goto fail;
	}

	if (rte_eth_xstats_get_names(portid, names, n) != n) {
		goto fail;
	}

	if (rte_eth_xstats_get(portid, values, n) != n) {
		goto fail;
	}

	/* root xstats object */
	cJSON *root = cJSON_CreateObject();

	for (int i = 0; i < n; i++) {
		cJSON_AddNumberToObject(root, names[i].name, values[i].value);
	}

	rte_free(names);
	rte_free(values);

	return root;

fail:
	rte_free(names);
	rte_free(values);

	return NULL;
}

static cJSON * encode_port(uint16_t portid)
{
	struct rte_eth_stats stats;
	if (rte_eth_stats_get(portid, &stats) != 0) {
		return NULL;
	}

	cJSON *port = cJSON_CreateObject();

	cJSON_AddItemToObject(port,
			"stats",
			encode_eth_stats(&stats));

	cJSON *xstats = encode_xstats(portid);
	if (xstats) {
		cJSON_AddItemToObject(port, "xstats", xstats);
	}

	return port;
}

char * encode_cmdReply_to_json(const char *reply)
{
	cJSON *root = cJSON_CreateObject();

	cJSON_AddStringToObject(root, "cmdReply", reply ? reply : "null");
	char *out = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);

	return out;   /* caller free() */
}

char * encode_log_to_json(const char *log_text)
{
	cJSON *root = cJSON_CreateObject();
	cJSON *log = cJSON_CreateObject();

	cJSON_AddItemToObject(root, "log", log);
	cJSON_AddStringToObject(log, "text", log_text ? log_text : "null");
	char *out = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);

	return out;   /* caller free() */
}

char * encode_stats_to_json(uint32_t port_mask, char *log_text,
		const struct stats_snapshot *ent)
{
	cJSON *root = cJSON_CreateObject();

	/* meta */
	cJSON *meta = cJSON_CreateObject();

	uint64_t cycles = rte_get_timer_cycles();
	uint64_t hz = rte_get_timer_hz();
	uint64_t sec  = cycles / hz;
	uint64_t rem  = cycles % hz;
	uint64_t timestamp_ns = sec * 1000000000ULL + rem * 1000000000ULL / hz;

	cJSON_AddNumberToObject(meta, "timestamp_ns", timestamp_ns);
	cJSON_AddStringToObject(meta, "source", "Bless Injector");
	cJSON_AddNumberToObject(meta, "schema_version", 7);
	cJSON_AddItemToObject(root, "meta", meta);

	cJSON *effective = cJSON_CreateObject();
	cJSON_AddNumberToObject(effective, "batch", ent ? ent->effective_batch : 0);
	cJSON_AddNumberToObject(effective, "traffic_model",
		ent ? ent->effective_traffic_model : 0);
	cJSON_AddNumberToObject(effective, "batch_delay_us",
		ent ? ent->effective_batch_delay_us : 0);
	cJSON_AddNumberToObject(effective, "batch_jitter_us",
		ent ? ent->effective_batch_jitter_us : 0);
	cJSON_AddNumberToObject(effective, "sample_interval",
		ent ? ent->effective_sample_interval : 0);
	cJSON_AddItemToObject(root, "effective_config", effective);

	/* ports */
	cJSON *ports = cJSON_CreateObject();
	cJSON_AddItemToObject(root, "ports", ports);

	uint16_t portid;
	RTE_ETH_FOREACH_DEV(portid) {
		if ((port_mask & (1u << portid)) == 0) {
			continue;
		}
		char key[8];
		snprintf(key, sizeof(key), "%u", portid);

		cJSON *port = encode_port(portid);
		if (port) {
			cJSON_AddItemToObject(ports, key, port);
		}
	}

	/* entropy (runtime-sampled, set by compute_entropy_stats) */
	if (ent) {
		cJSON *entropy = cJSON_CreateObject();
		cJSON_AddNumberToObject(entropy, "protocol",   ent->entropy_protocol);
		cJSON_AddNumberToObject(entropy, "src_ip",     ent->entropy_src_ip);
		cJSON_AddNumberToObject(entropy, "dst_ip",     ent->entropy_dst_ip);
		cJSON_AddNumberToObject(entropy, "src_port",   ent->entropy_src_port);
		cJSON_AddNumberToObject(entropy, "dst_port",   ent->entropy_dst_port);
		cJSON_AddNumberToObject(entropy, "pkt_size",   ent->entropy_pkt_size);
		cJSON_AddNumberToObject(entropy, "vxlan_encap", ent->entropy_vxlan_encap);
		cJSON_AddNumberToObject(entropy, "outer_src_ip", ent->entropy_outer_src_ip);
		cJSON_AddNumberToObject(entropy, "outer_dst_ip", ent->entropy_outer_dst_ip);
		cJSON_AddNumberToObject(entropy, "outer_src_port", ent->entropy_outer_src_port);
		cJSON_AddNumberToObject(entropy, "vni",       ent->entropy_vni);
		cJSON_AddNumberToObject(entropy, "tcp_flags", ent->entropy_tcp_flags);
		cJSON_AddNumberToObject(entropy, "total_5tuple", ent->entropy_total_5tuple);
		/* P0a */
		cJSON_AddNumberToObject(entropy, "delta_tsc",  ent->entropy_delta_tsc);
		/* P0b */
		cJSON_AddNumberToObject(entropy, "flow_distinct", ent->flow_distinct);
		cJSON_AddNumberToObject(entropy, "flow_total",    ent->flow_total);
		cJSON_AddNumberToObject(entropy, "flow_ratio",    ent->flow_ratio);
		cJSON_AddNumberToObject(entropy, "sampler_samples",
			ent->sampler_samples);
		cJSON_AddNumberToObject(entropy, "sampler_overwritten",
			ent->sampler_overwritten);
		cJSON_AddNumberToObject(entropy, "sampler_overwritten_window",
			ent->sampler_overwritten_window);
		cJSON_AddNumberToObject(entropy, "flow_entropy_5tuple", ent->flow_entropy_5tuple);
		cJSON_AddNumberToObject(entropy, "flow_entropy_lifetime", ent->flow_entropy_lifetime);
		cJSON_AddNumberToObject(entropy, "flow_entropy_event", ent->flow_entropy_event);
		cJSON_AddNumberToObject(entropy, "flow_count", ent->flow_count);
		cJSON_AddNumberToObject(entropy, "flow_lifetime_count", ent->flow_lifetime_count);
		cJSON_AddNumberToObject(entropy, "flow_event_count", ent->flow_event_count);
		/* P0c */
		cJSON_AddNumberToObject(entropy, "max_src_ip",    ent->max_src_ip);
		cJSON_AddNumberToObject(entropy, "max_dst_ip",    ent->max_dst_ip);
		cJSON_AddNumberToObject(entropy, "max_src_port",  ent->max_src_port);
		cJSON_AddNumberToObject(entropy, "max_dst_port",  ent->max_dst_port);
		cJSON_AddNumberToObject(entropy, "max_outer_src_ip", ent->max_outer_src_ip);
		cJSON_AddNumberToObject(entropy, "max_outer_dst_ip", ent->max_outer_dst_ip);
		cJSON_AddNumberToObject(entropy, "max_vni",          ent->max_vni);
		/* P1 min-entropy */
		cJSON_AddNumberToObject(entropy, "min_protocol",      ent->entropy_min_protocol);
		cJSON_AddNumberToObject(entropy, "min_src_ip",        ent->entropy_min_src_ip);
		cJSON_AddNumberToObject(entropy, "min_dst_ip",        ent->entropy_min_dst_ip);
		cJSON_AddNumberToObject(entropy, "min_src_port",      ent->entropy_min_src_port);
		cJSON_AddNumberToObject(entropy, "min_dst_port",      ent->entropy_min_dst_port);
		cJSON_AddNumberToObject(entropy, "min_pkt_size",      ent->entropy_min_pkt_size);
		cJSON_AddNumberToObject(entropy, "min_tcp_flags",    ent->entropy_min_tcp_flags);
		cJSON_AddNumberToObject(entropy, "min_delta_tsc",    ent->entropy_min_delta_tsc);
		cJSON_AddNumberToObject(entropy, "min_outer_src_ip", ent->entropy_min_outer_src_ip);
		cJSON_AddNumberToObject(entropy, "min_outer_dst_ip", ent->entropy_min_outer_dst_ip);
		cJSON_AddNumberToObject(entropy, "min_vni",          ent->entropy_min_vni);
		cJSON_AddNumberToObject(entropy, "min_total_5tuple",  ent->entropy_min_total_5tuple);
		cJSON *min_diagnostics = cJSON_CreateObject();
		for (unsigned i = 0; i < ENTROPY_MIN_DIM_COUNT; i++) {
			const struct entropy_min_diagnostic *d = &ent->min_diag[i];
			cJSON *item = cJSON_CreateObject();
			cJSON_AddNumberToObject(item, "measured", d->measured);
			cJSON_AddNumberToObject(item, "target", d->target);
			cJSON_AddNumberToObject(item, "population_target", d->population_target);
			cJSON_AddNumberToObject(item, "gap_bits", d->gap_bits);
			cJSON_AddNumberToObject(item, "attainment", d->attainment);
			cJSON_AddNumberToObject(item, "dominance_ratio", d->dominance_ratio);
			cJSON_AddNumberToObject(item, "max_probability", d->max_probability);
			cJSON_AddNumberToObject(item, "samples", (double)d->samples);
			cJSON_AddNumberToObject(item, "distinct", (double)d->distinct);
			cJSON_AddNumberToObject(item, "max_count", (double)d->max_count);
			cJSON_AddStringToObject(item, "baseline_source",
				entropy_baseline_name(d->baseline_source));
			cJSON_AddStringToObject(item, "state", entropy_diag_state_name(d->state));
			cJSON_AddItemToObject(min_diagnostics, entropy_min_dim_names[i], item);
		}
		cJSON_AddItemToObject(entropy, "min_diagnostics", min_diagnostics);
		/* P1 joint */
		cJSON_AddNumberToObject(entropy, "joint_5tuple",      ent->entropy_joint_5tuple);
		/* mutual information */
		cJSON_AddNumberToObject(entropy, "mi_sip_dip",   ent->mi_sip_dip);
		cJSON_AddNumberToObject(entropy, "mi_spt_dpt",   ent->mi_spt_dpt);
		cJSON_AddNumberToObject(entropy, "mi_proto_spt", ent->mi_proto_spt);
		cJSON_AddNumberToObject(entropy, "mi_size_dpt",  ent->mi_size_dpt);
		cJSON_AddNumberToObject(entropy, "mi_size_proto", ent->mi_size_proto);
		cJSON_AddNumberToObject(entropy, "mi_dtsc_proto", ent->mi_dtsc_proto);
		cJSON_AddNumberToObject(entropy, "mi_dtsc_flow",  ent->mi_dtsc_flow);
		cJSON_AddNumberToObject(entropy, "mi_tcpf_sz",   ent->mi_tcpf_sz);
		cJSON_AddNumberToObject(entropy, "mi_tcpf_spt",  ent->mi_tcpf_spt);
		cJSON_AddNumberToObject(entropy, "mi_tcpf_dpt",  ent->mi_tcpf_dpt);
		cJSON_AddNumberToObject(entropy, "mi_osip_odip", ent->mi_osip_odip);
		cJSON_AddNumberToObject(entropy, "mi_vni_osip",  ent->mi_vni_osip);
		/* upper bounds derived from observed marginal entropy */
		cJSON_AddNumberToObject(entropy, "mi_max_sip_dip",    ent->mi_max_sip_dip);
		cJSON_AddNumberToObject(entropy, "mi_max_spt_dpt",    ent->mi_max_spt_dpt);
		cJSON_AddNumberToObject(entropy, "mi_max_proto_spt",  ent->mi_max_proto_spt);
		cJSON_AddNumberToObject(entropy, "mi_max_size_dpt",   ent->mi_max_size_dpt);
		cJSON_AddNumberToObject(entropy, "mi_max_size_proto", ent->mi_max_size_proto);
		cJSON_AddNumberToObject(entropy, "mi_max_dtsc_proto", ent->mi_max_dtsc_proto);
		cJSON_AddNumberToObject(entropy, "mi_max_dtsc_flow",  ent->mi_max_dtsc_flow);
		cJSON_AddNumberToObject(entropy, "mi_max_tcpf_sz",    ent->mi_max_tcpf_sz);
		cJSON_AddNumberToObject(entropy, "mi_max_tcpf_spt",   ent->mi_max_tcpf_spt);
		cJSON_AddNumberToObject(entropy, "mi_max_tcpf_dpt",   ent->mi_max_tcpf_dpt);
		cJSON_AddNumberToObject(entropy, "mi_max_osip_odip",  ent->mi_max_osip_odip);
		cJSON_AddNumberToObject(entropy, "mi_max_vni_osip",   ent->mi_max_vni_osip);
		cJSON_AddItemToObject(root, "entropy", entropy);
	}

	/* rate PSD */
	{
		cJSON *psd = cJSON_CreateObject();
		cJSON_AddNumberToObject(psd, "dominant_hz",
		                        ent->psd_dominant_hz);
		cJSON_AddNumberToObject(psd, "strongest_peak_hz",
		                        ent->psd_strongest_peak_hz);
		cJSON_AddNumberToObject(psd, "fundamental_hz",
		                        ent->psd_fundamental_hz);
		cJSON_AddNumberToObject(psd, "spectral_flatness",
		                        ent->psd_spectral_flatness);
		cJSON_AddNumberToObject(psd, "mean_ppms", ent->psd_mean_ppms);
		cJSON_AddNumberToObject(psd, "variation_rms_ppms",
		                        ent->psd_variation_rms_ppms);
		cJSON_AddBoolToObject(psd, "signal_valid", ent->psd_signal_valid);
		cJSON *bins = cJSON_CreateArray();
		for (int i = 0; i < 256; i++)
			cJSON_AddItemToArray(bins,
			                     cJSON_CreateNumber(ent->psd_bins[i]));
		cJSON_AddItemToObject(psd, "bins", bins);
		cJSON_AddItemToObject(root, "psd", psd);
	}

	/* observe panel: throughput, loss, CPU, memory (always present) */
	{
		cJSON *obs = cJSON_CreateObject();
		cJSON_AddNumberToObject(obs, "rx_mpps",       ent->rx_mpps);
		cJSON_AddNumberToObject(obs, "tx_mpps",       ent->tx_mpps);
		cJSON_AddNumberToObject(obs, "rx_gbps",       ent->rx_gbps);
		cJSON_AddNumberToObject(obs, "tx_gbps",       ent->tx_gbps);
		cJSON_AddNumberToObject(obs, "rx_loss_rate",  ent->rx_loss_rate);
		cJSON_AddNumberToObject(obs, "process_cpu_cores",
			ent->process_cpu_cores);
		cJSON_AddNumberToObject(obs, "enabled_lcores",
			(double)ent->enabled_lcores);
		cJSON_AddNumberToObject(obs, "enabled_lcore_utilization_ratio",
			ent->enabled_lcore_utilization_ratio);
		cJSON_AddNumberToObject(obs, "cpu_busy_pct",  ent->cpu_busy_pct);
		cJSON_AddNumberToObject(obs, "tx_build_cycles_per_pkt",  ent->tx_build_cycles_per_pkt);
		cJSON_AddNumberToObject(obs, "tx_submit_cycles_per_pkt", ent->tx_submit_cycles_per_pkt);
		cJSON_AddNumberToObject(obs, "tx_cycles_per_pkt",        ent->tx_cycles_per_pkt);
		cJSON_AddNumberToObject(obs, "tx_wait_ratio",            ent->tx_wait_ratio);
		cJSON_AddNumberToObject(obs, "mem_rss_kb",    (double)ent->mem_rss_kb);
		cJSON_AddNumberToObject(obs, "cpu_freq_min_khz", (double)ent->cpu_freq_min_khz);
		cJSON_AddNumberToObject(obs, "cpu_freq_max_khz", (double)ent->cpu_freq_max_khz);
		cJSON_AddNumberToObject(obs, "voluntary_ctx_switches", (double)ent->voluntary_ctx_switches);
		cJSON_AddNumberToObject(obs, "involuntary_ctx_switches", (double)ent->involuntary_ctx_switches);
		cJSON_AddNumberToObject(obs, "tx_submit_target_us", ent->tx_submit_target_us);
		cJSON_AddNumberToObject(obs, "tx_submit_overshoot_p50_us",
			ent->tx_submit_overshoot_p50_us);
		cJSON_AddNumberToObject(obs, "tx_submit_overshoot_p99_us",
			ent->tx_submit_overshoot_p99_us);
		cJSON_AddNumberToObject(obs, "tx_submit_overshoot_max_us",
			ent->tx_submit_overshoot_max_us);
		cJSON_AddNumberToObject(obs, "tx_burst_duration_p50_us",
			ent->tx_burst_duration_p50_us);
		cJSON_AddNumberToObject(obs, "tx_burst_duration_p99_us",
			ent->tx_burst_duration_p99_us);
		cJSON_AddNumberToObject(obs, "tx_burst_duration_max_us",
			ent->tx_burst_duration_max_us);
		cJSON_AddNumberToObject(obs, "tx_submit_samples", (double)ent->tx_submit_samples);
		cJSON_AddNumberToObject(obs, "lat_p50_us",    ent->lat_p50);
		cJSON_AddNumberToObject(obs, "lat_p95_us",    ent->lat_p95);
		cJSON_AddNumberToObject(obs, "lat_p99_us",    ent->lat_p99);
		cJSON_AddNumberToObject(obs, "lat_p999_us",   ent->lat_p999);
		cJSON_AddNumberToObject(obs, "lat_samples",   (double)ent->lat_samples);
		cJSON_AddItemToObject(root, "observe", obs);
	}

	/* handshake stats (separate object) */
	if (ent->hs_syn_sent > 0 || ent->hs_conn_current > 0) {
		cJSON *hs = cJSON_CreateObject();
		cJSON_AddNumberToObject(hs, "syn_sent",      ent->hs_syn_sent);
		cJSON_AddNumberToObject(hs, "syn_recv",      ent->hs_syn_recv);
		cJSON_AddNumberToObject(hs, "synack_sent",   ent->hs_synack_sent);
		cJSON_AddNumberToObject(hs, "synack_recv",   ent->hs_synack_recv);
		cJSON_AddNumberToObject(hs, "ack_sent",      ent->hs_ack_sent);
		cJSON_AddNumberToObject(hs, "established",   ent->hs_established);
		cJSON_AddNumberToObject(hs, "rst_sent",      ent->hs_rst_sent);
		cJSON_AddNumberToObject(hs, "rst_recv",      ent->hs_rst_recv);
		cJSON_AddNumberToObject(hs, "timed_out",     ent->hs_timed_out);
		cJSON_AddNumberToObject(hs, "conn_current",  ent->hs_conn_current);
		cJSON_AddNumberToObject(hs, "conn_max",      ent->hs_conn_max);
		cJSON_AddNumberToObject(hs, "success_rate",  ent->hs_success_rate);
		cJSON_AddNumberToObject(hs, "cps",           ent->hs_cps);
		cJSON_AddItemToObject(root, "handshake", hs);
	}

	cJSON *log = cJSON_CreateObject();
	cJSON_AddItemToObject(root, "log", log);
	cJSON_AddStringToObject(log, "text", log_text ? log_text : "null");

	char *out = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);

	return out;   /* caller free() */
}
