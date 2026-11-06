#ifndef __ENTROPY_STATS_H__
#define __ENTROPY_STATS_H__

/**
 * @file entropy_stats.h
 * @brief Offline entropy computation: Shannon, min-entropy, mutual information.
 *
 * Drains the per-worker entropy_sampler ring buffers and computes
 * empirical entropy across 20+ dimensions (protocol, IP, port,
 * packet size, TCP flags, VXLAN tunnel fields, timing deltas,
 * joint 5-tuple, and pairwise mutual information).
 *
 * Pure computation -- no dependency on the worker data plane.
 */

#include "server.h"   /* stats_snapshot */
#include "config.h"    /* dist_ratio */
#include "cnode.h"     /* Cnode */

struct bless_conf;

/**
 * Compute empirical entropy statistics from all active workers.
 *
 * Drains the per-lcore entropy_sampler[] ring buffers (skipping the
 * main lcore), sorts each dimension, and computes:
 *  - Shannon entropy H(X) and min-entropy H_inf(X) per dimension
 *  - Joint 5-tuple entropy
 *  - Pairwise mutual information I(X;Y) across 12 dimension pairs
 *  - Config-derived theoretical maximum per dimension
 *  - Latency histogram aggregation (p50/p95/p99/p999 in µs)
 *  - EMA-diagonal smoothing of entropy time series
 *
 * Results are written into the provided stats_snapshot.
 *
 * @param s       Output snapshot (all entropy fields filled).
 * @param dr      Distribution ratios from YAML config (read-only).
 * @param cnode   Packet template configuration (read-only).
 * @param bconf   Bless config (for max-entropy derivation and
 *                mi_smoothed[] EMA state).
 */
void compute_entropy_stats(struct stats_snapshot *s,
			   const struct dist_ratio *dr,
			   const struct Cnode *cnode,
			   struct bless_conf *bconf);

#endif /* __ENTROPY_STATS_H__ */
