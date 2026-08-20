#ifndef __WORKER_H__
#define __WORKER_H__

/**
 * @file worker.h
 * @brief Per-lcore worker abstraction -- modes, state machine,
 *        entropy sampling, and handshake context.
 *
 * Each worker runs one of the registered worker_func[] modes
 * (tx-only, rx-only, fwd, flow, handshake) inside a tight loop
 * driven by a global atomic state (INIT -> RUNNING -> STOP -> EXIT).
 */

#include <rte_lcore.h>

#include "base.h"
#include "config.h"   /* dist_ratio */
#include "cnode.h"    /* Cnode */
#include "entropy.h"  /* entropy_sampler */
#include "flow_entropy.h"
#include "rate_psd.h"
#include "token_bucket.h"
#include "pacing.h"

/* handshake mode hash table */#define HS_HT_LOG2    18               /* 262K buckets (must be ≤ 20) */
#define HS_HT_SIZE    (1u << HS_HT_LOG2)
#define HS_HT_MASK    (HS_HT_SIZE - 1u)
#define HS_KEY_TOMBSTONE UINT64_MAX    /* deleted slot marker */

/** Per-entry connection state for lightweight TCP handshake mode. */
struct hs_entry {
	uint64_t key;           /* commutative 5-tuple XOR (0 = empty) */
	uint32_t src_ip;        /* canonical src (initiator side) */
	uint32_t dst_ip;        /* canonical dst */
	uint16_t src_port;
	uint16_t dst_port;
	uint8_t  state;         /* 1=SYN_SENT, 2=SYN_RCVD, 3=ESTABLISHED */
	uint32_t my_seq;        /* local seq (host byte order) */
	uint32_t my_ack;        /* local ack (host byte order) */
	uint64_t tsc;           /* last activity TSC */
	uint64_t creation_tsc;  /* creation TSC (for lifetime computation) */
};

/** Per-worker handshake runtime state. */
struct handshake_ctx {
	struct hs_entry table[HS_HT_SIZE];
	uint32_t count;                 /* active entries */

	/* stats counters (atomics: worker writes, master reads) */
	uint64_t syn_sent;
	uint64_t syn_recv;
	uint64_t synack_sent;
	uint64_t synack_recv;
	uint64_t ack_sent;
	uint64_t established;
	uint64_t rst_sent;
	uint64_t rst_recv;
	uint64_t timed_out;
	uint32_t conn_max;              /* high-water mark */
	uint64_t cps_start_tsc;         /* reference TSC for CPS */
	uint64_t syn_sent_at_start;     /* syn_sent at cps_start_tsc */
	uint32_t cleanup_cursor;        /* chunked cleanup position */

	/* RST burst buffer -- filled by hs_cleanup_chunk, drained by main loop */
	uint32_t n_rst;
	struct rte_mbuf *rst_mbufs[64];

	/* per-worker flow entropy sampler */
	struct flow_sampler flow_sampler;
};

struct worker {
	int mode;
	struct rte_mbuf **mbufs;
	struct rte_mbuf **rx_mbufs;

	/* thread name */
	struct bless_conf conf;
	struct base_core_view cv;
	Cnode cnode;
	cpu_set_t cpuset;
	char name[256];

	/* per-worker runtime entropy sampler */
	struct entropy_sampler sampler;

	/* per-worker rate PSD sampler (frequency-domain TX rate) */
	struct rate_psd psd;

	/* per-worker handshake state (NULL if not handshake mode) */
	struct handshake_ctx *hs_ctx;

	/* per-worker token buckets (PPS / BPS / CPS) */
	struct token_bucket pps_bucket;
	struct token_bucket bps_bucket;
	struct token_bucket cps_bucket;
	struct pacing_ctx pacing;
};

/** Per-worker main loop: TX-only, RX-only, or forward mode.
 *
 *  Reads commands via WebSocket, applies jitter, handles interleave,
 *  and generates stats every timer_period.  Blocks on state transitions.
 *
 *  @param conf  Opaque pointer to bless_conf for this worker. */
void worker_loop(void *conf);

/** Master-lcore main loop.
 *
 *  Sets up the worker threadpool, broadcasts stats via WebSocket,
 *  and runs the signal-driven state machine (RUNNING -> STOPPED -> EXIT).
 *
 *  @param conf  Opaque pointer to bless_conf for this worker. */
void worker_main_loop(void *conf);

/** Aggregate per-port stats into a double-buffered snapshot.
 *
 *  Fills RX/TX counters, rate, DPDK eth stats, entropy metrics,
 *  latency histogram buckets, and handshake-mode CPS / success-rate.
 *
 *  @param enabled_port_mask  Bitmask of enabled ports
 *  @param dr                 Distribution ratios (read-only)
 *  @param cnode              Configuration node (read-only)
 *  @param bconf              Bless conf (for stats pointers) */
void worker_generate_stats(uint32_t enabled_port_mask,
			   const struct dist_ratio *dr, const Cnode *cnode,
			   struct bless_conf *bconf);

void worker_effective_config_snapshot(struct stats_snapshot *s);

/** WebSocket user callback: handle incoming data frames.
 *
 *  @param user  Opaque user context
 *  @param data  Payload
 *  @param size  Payload size */
struct mg_connection;
void ws_user_func(struct mg_connection *conn, void *user,
		  void *data, size_t size);

/** Log a worker's configuration fields for debugging. */
void worker_show(const struct worker *w);

/** Check the global state machine and finite packet count.
 *
 *  Returns CHECK_EXIT if the worker should exit, CHECK_CONTINUE
 *  otherwise.  Sleeps on STOPPED / INIT states, re-seeds PRNG on
 *  RUNNING transition, and handles finite --num countdown.
 *
 *  Called from every worker mode main loop.
 *
 *  @param state  Pointer to the global atomic state.
 *  @param cv     Core view for logging.
 *  @param num    Remaining packet count (INT64_MAX = infinite).
 *  @param sampler Per-worker sampler whose phase is refreshed on restart.
 *  @return       CHECK_EXIT or CHECK_CONTINUE. */
enum { CHECK_CONTINUE = 0, CHECK_EXIT = 1 };
int worker_check_state(atomic_int *state, struct base_core_view *cv,
		       int64_t num, struct entropy_sampler *sampler);

/* per-lcore handshake ctx registry (master drains for stats) */
extern struct handshake_ctx *handshake_ctxs[RTE_MAX_LCORE];

#endif
