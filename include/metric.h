#ifndef __METRICS_H__
#define __METRICS_H__

/**
 * @file metric.h
 * @brief Per-second telemetry snapshot and Prometheus exposition.
 *
 * The metrics thread wakes every timer_period seconds, collects
 * per-port statistics via rte_eth_stats_get(), merges per-worker
 * sampler data, and formats the result for the /metrics endpoint.
 */

#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <time.h>

#include <rte_lcore.h>
#include <rte_telemetry.h>
#include <rte_debug.h>
#include <rte_ethdev.h>

#include "bless.h"
#include "cJSON.h"
int bless_handle_system(const char *cmd, const char *params, struct rte_tel_data *info);
char * encode_all_ports_json(uint32_t port_mask);
char * encode_cmdReply_to_json(const char *reply);
char * encode_log_to_json(const char *log_text);
char * encode_stats_to_json(uint32_t port_mask, char *log_text,
			const struct stats_snapshot *ent);
size_t encode_stats_to_text(uint32_t port_mask, char *msg, size_t len,
			const struct stats_snapshot *ent);
void metric_set_cbfn(void*(*metric_cbfn)());

/* observe helpers */
void compute_rate_metrics(struct stats_snapshot *s, uint32_t port_mask);
void sample_cpu_usage(struct stats_snapshot *s);
void sample_memory_usage(struct stats_snapshot *s);
void sample_runtime_noise(struct stats_snapshot *s);

/* Software TX counters -- call from worker data-plane after rte_eth_tx_burst().
 * Needed for virtual PMDs (net_null, net_pcap) that don't update hardware counters. */
void metric_tx_account(uint64_t count, uint64_t bytes);

/* TX hot-path timing breakdown (TSC cycles), accumulated by workers.
 * build  = packet construction (bless_mbufs + mutation + shuffle)
 * wait   = pacing busy-wait (deadline spin / rate-limiter delay)
 * submit = rte_eth_tx_burst() duration
 * Zero-valued terms are skipped.  Per-packet "real send" cost is
 * (build + submit) / packets; wait is pacing idle that must be excluded
 * when attributing send work. */
void metric_tx_timing_account(uint64_t build_cycles, uint64_t wait_cycles,
			      uint64_t submit_cycles);

#endif
