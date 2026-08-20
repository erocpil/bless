/**
 * @file worker_handshake.c
 * @brief Lightweight TCP handshake engine.
 *
 * Implements a per-worker SYN -> SYN-ACK -> ACK state machine
 * for connection-stateful entropy injection.  Each burst generates
 * random 5-tuples, initiates TCP handshakes, and completes them.
 * Timeout cleanup generates RST packets for stale entries.
 *
 * Data structures:
 *  - Open-addressing hash table (HS_HT_SIZE = 262K buckets)
 *  - Commutative XOR key for direction-independent matching
 *  - Per-entry state machine: 1=SYN_SENT, 2=SYN_RCVD, 3=ESTABLISHED
 *  - Chunked cleanup (HS_HT_SIZE/64 per iteration) with RST generation
 */

#include "bless.h"            /* LOG_*, rte_*, bless_create_pktmbuf_pool,
			       * bless_mbufs_tcp */
#include "log.h"             /* LOG_INFO, LOG_ERR, LOG_META, etc. */
#include "worker.h"           /* worker, handshake_ctx, handshake_ctxs[],
			       * worker_check_state */
#include "worker_handshake.h" /* tcp_hs_fill, TCP_PKT_SIZE */
#include "cnode.h"            /* Cnode, RANDOM_IP_SRC, RANDOM_TCP_SRC, ... */
#include "define.h"           /* fast_rand_next, STATE_*, MIN/MAX macros */
#include "token_bucket.h"     /* token_bucket_available, token_bucket_consume */
#include "rate_psd.h"         /* rate_psd_account */
#include "entropy.h"          /* entropy sampling */
#include "flow_entropy.h"     /* flow_sampler_record */
#include "metric.h"           /* metric_tx_account */

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

/* ── Hash table helpers ───────────────────────────────────────── */

/** Commutative 5-tuple XOR key -- direction-independent. */
static inline uint64_t hs_make_key(uint32_t a, uint32_t b,
				   uint16_t c, uint16_t d)
{
	return ((uint64_t)(a ^ b)) | (((uint64_t)(c ^ d)) << 32);
}

static inline uint32_t hs_key_idx(uint64_t key)
{
	return (uint32_t)(key >> (64 - HS_HT_LOG2));
}

/** Match an entry against a 5-tuple (accounting for direction reversal).
 *  Returns 1 = direct, -1 = reverse, 0 = no match. */
static inline int hs_match(const struct hs_entry *e,
			   uint32_t sa, uint32_t da, uint16_t sp, uint16_t dp)
{
	if (e->src_ip == sa && e->dst_ip == da &&
	    e->src_port == sp && e->dst_port == dp) {
		return 1;
	}
	if (e->src_ip == da && e->dst_ip == sa &&
	    e->src_port == dp && e->dst_port == sp) {
		return -1;
	}
	return 0;
}

/** Insert or update entry.  Returns pointer or NULL if table full.
 *  Reuses tombstone slots (key == HS_KEY_TOMBSTONE) as empty. */
static inline struct hs_entry *
hs_insert(struct handshake_ctx *ctx,
	  uint32_t sa, uint32_t da, uint16_t sp, uint16_t dp,
	  uint8_t state, uint32_t my_seq, uint32_t my_ack)
{
	uint64_t key = hs_make_key(sa, da, sp, dp);
	uint32_t idx = hs_key_idx(key);
	for (uint32_t i = 0; i < HS_HT_SIZE; i++) {
		uint32_t probe = (idx + i) & HS_HT_MASK;
		if (ctx->table[probe].key == 0 ||
		    ctx->table[probe].key == HS_KEY_TOMBSTONE) {
			if (ctx->table[probe].key != HS_KEY_TOMBSTONE) {
				ctx->count++;
			}
			if (ctx->count > ctx->conn_max) {
				ctx->conn_max = ctx->count;
			}
			struct hs_entry *e = &ctx->table[probe];
			e->key      = key;
			e->src_ip   = sa;  e->dst_ip   = da;
			e->src_port = sp;  e->dst_port = dp;
			e->state    = state;
			e->my_seq   = my_seq;  e->my_ack = my_ack;
			e->tsc      = rte_rdtsc();
			e->creation_tsc = e->tsc;
			return e;
		}
		if (ctx->table[probe].key == key &&
		    hs_match(&ctx->table[probe], sa, da, sp, dp)) {
			/* update existing */
			struct hs_entry *e = &ctx->table[probe];
			e->state = state;
			e->my_seq = my_seq; e->my_ack = my_ack;
			e->tsc = rte_rdtsc();
			return e;
		}
	}
	return NULL; /* table full */
}

/** Lookup entry by 5-tuple.  Returns pointer or NULL.
 *  Skips tombstone slots (deleted entries) to preserve probe chains. */
static inline struct hs_entry *
hs_lookup(struct handshake_ctx *ctx,
	  uint32_t sa, uint32_t da, uint16_t sp, uint16_t dp)
{
	uint64_t key = hs_make_key(sa, da, sp, dp);
	uint32_t idx = hs_key_idx(key);
	for (uint32_t i = 0; i < HS_HT_SIZE; i++) {
		uint32_t probe = (idx + i) & HS_HT_MASK;
		if (ctx->table[probe].key == 0) {
			return NULL; /* empty: end of probe chain */
		}
		if (ctx->table[probe].key == HS_KEY_TOMBSTONE) {
			continue; /* deleted: skip, keep probing */
		}
		if (ctx->table[probe].key == key &&
		    hs_match(&ctx->table[probe], sa, da, sp, dp)) {
			return &ctx->table[probe];
		}
	}
	return NULL;
}

/** Mark entry deleted (tombstone).  Preserves probe chain integrity
 *  for entries that collided and probed past this slot. */
static inline void hs_remove(struct hs_entry *e)
{
	e->key = HS_KEY_TOMBSTONE;
}

/** Scan chunk of hash table for timed-out entries.
 *
 *  RST packets are built and queued in ctx->rst_mbufs for the main
 *  loop to TX-burst after cleanup.  Call once per loop iteration.
 *  Scans HS_HT_SIZE / 64 entries each call to amortise cost. */
static inline void
hs_cleanup_chunk(struct handshake_ctx *ctx,
		 uint64_t now, uint64_t timeout_cycles,
		 struct rte_mempool *pool,
		 const struct rte_ether_addr *mac_src,
		 const struct rte_ether_addr *mac_dst)
{
	ctx->n_rst = 0;
	uint32_t chunk = HS_HT_SIZE / 64;
	for (uint32_t i = 0; i < chunk; i++) {
		uint32_t idx = (ctx->cleanup_cursor + i) & HS_HT_MASK;
		struct hs_entry *e = &ctx->table[idx];
		if (e->key != 0 && (now - e->tsc) > timeout_cycles) {
			/* build RST packet */
			if (ctx->n_rst < 64) {
				struct rte_mbuf *m = rte_pktmbuf_alloc(pool);
				if (m) {
					rte_pktmbuf_append(m, TCP_PKT_SIZE);
					tcp_hs_fill(m, mac_src, mac_dst,
						rte_cpu_to_be_32(e->src_ip),
						rte_cpu_to_be_32(e->dst_ip),
						rte_cpu_to_be_16(e->src_port),
						rte_cpu_to_be_16(e->dst_port),
						rte_cpu_to_be_32(e->my_seq + 1),
						0, RTE_TCP_RST_FLAG);
										ctx->rst_mbufs[ctx->n_rst++] = m;
									}
								}
								e->key = HS_KEY_TOMBSTONE;
								ctx->count--;
								ctx->timed_out++;
			flow_sampler_record(&ctx->flow_sampler,
				e->src_ip, e->dst_ip, e->src_port, e->dst_port,
				2, now - e->creation_tsc);
		}
	}
	ctx->cleanup_cursor = (ctx->cleanup_cursor + chunk) & HS_HT_MASK;
}

/* ── Handshake worker mode ────────────────────────────────────── */

/**
 * Lightweight TCP handshake mode: SYN -> SYN-ACK -> ACK.
 *
 * Five phases per loop iteration:
 *  1. TX: mix of SYN (handshake init) and stateless TCP traffic,
 *     rate-limited by CPS token bucket and hs_mix_ratio.
 *  2. RX: parse incoming IPv4/TCP, match against hash table,
 *     respond with SYN-ACK (to server SYNs) or ACK (to SYN-ACKs).
 *  3. TX responses (SYN-ACKs / ACKs).
 *  4. Timeout cleanup: scan a chunk of the hash table, send RSTs.
 *  5. Batch delay.
 *
 * Requires RX-capable device (no PCAP iface=lo TX-only). */
int
worker_func_handshake(struct worker *w)
{
	LOG_INFO("core %u handshake mode", w->cv.core);
	if (!w) { LOG_ERR("Not a worker"); return -1; }

	struct bless_conf *conf = &w->conf;
	int64_t num = conf->num < 0 ? INT64_MAX : conf->num;
	atomic_int *state = conf->state;
	uint16_t batch = conf->batch > 2048 ? 2048 : conf->batch;
	struct base_core_view *cv = &w->cv;
	uint16_t port = cv->port;
	uint16_t txq = cv->txq;
	uint16_t rxq = cv->rxq;
	Cnode *cnode = &w->cnode;
	uint32_t lcore_id = rte_lcore_id();

	/* Verify device has RX capability for bidirectional handshake */
	struct rte_eth_dev_info dev_info;
	rte_eth_dev_info_get(port, &dev_info);
	if (dev_info.max_rx_queues == 0) {
		LOG_ERR("handshake mode requires RX capability, but port %u has 0 RX queues"
			" (PCAP iface=lo is TX-only; use rx_iface=lo or tap device)",
			port);
		return -1;
	}

	/* rate: default = batch */
	uint32_t hs_rate = conf->hs_rate ? conf->hs_rate : batch;
	uint64_t timeout_cycles = conf->hs_timeout_us
		? (conf->hs_timeout_us * rte_get_timer_hz() / 1000000)
		: (10ULL * rte_get_timer_hz()); /* default: 10s */

	/* allocate handshake context (hugepage-backed hash table ~10MB) */
	struct handshake_ctx *hs = rte_zmalloc("hs_ctx",
		sizeof(struct handshake_ctx), 0);
	if (!hs) {
		LOG_ERR("handshake: rte_zmalloc(hs_ctx) failed -- lcore %u disabled",
			lcore_id);
		return -1;
	}
	w->hs_ctx = hs;
	handshake_ctxs[lcore_id] = hs;
	hs->cps_start_tsc = rte_rdtsc();
	flow_sampler_init(&hs->flow_sampler, 4);

	/* mbuf pool + arrays — declare before allocations so EXIT can free(NULL) */
	struct rte_mempool *pool = NULL;
	struct rte_mbuf **syn_mbufs = NULL;
	struct rte_mbuf **resp_mbufs = NULL;
	struct rte_mbuf **rx_mbufs = NULL;
	struct entropy_5tuple *sample_tuples = NULL;
	uint16_t *sample_positions = NULL;

	pool = bless_create_pktmbuf_pool(batch * 4, "hs_pool");
	if (!pool) {
		LOG_ERR("handshake: mbuf pool alloc failed -- lcore %u disabled",
			lcore_id);
		goto EXIT;
	}

	syn_mbufs = rte_malloc(NULL,
		batch * sizeof(void *), RTE_CACHE_LINE_SIZE);
	resp_mbufs = rte_malloc(NULL,
		batch * sizeof(void *), RTE_CACHE_LINE_SIZE);
	rx_mbufs = rte_malloc(NULL,
		batch * sizeof(void *), RTE_CACHE_LINE_SIZE);
	sample_tuples = rte_malloc(NULL,
		batch * sizeof(*sample_tuples), RTE_CACHE_LINE_SIZE);
	sample_positions = rte_malloc(NULL,
		batch * sizeof(*sample_positions), RTE_CACHE_LINE_SIZE);
	if (!syn_mbufs || !resp_mbufs || !rx_mbufs || !sample_tuples ||
	    !sample_positions) {
		LOG_ERR("handshake: mbuf array alloc failed -- lcore %u disabled",
			lcore_id);
		goto EXIT;
	}

	LOG_META("cpu %u lcore %u core %u port %u txq %u rxq %u",
		 sched_getcpu(), lcore_id, cv->core,
		 cv->port, txq, rxq);

	while (1) {
		if (worker_check_state(state, cv, num, &w->sampler) == CHECK_EXIT) {
			goto EXIT;
		}

		uint64_t now = rte_rdtsc();

		/* Phase 1: TX -- mix of SYNs and stateless traffic */
		int hs_slots = (batch * conf->hs_mix_ratio) / 1000;
		if (hs_slots < 1) {
			hs_slots = 1;
		}
		int sl_slots = batch - hs_slots;
		if (sl_slots < 0) {
			sl_slots = 0;
		}
		int n_syn = MIN((int)hs_slots, (int)hs_rate);
		/* CPS rate limiter */
		if (w->cps_bucket.cir > 0) {
			uint64_t cps_avail = token_bucket_available(&w->cps_bucket);
			if (cps_avail < (uint64_t)n_syn) {
				n_syn = (int)cps_avail;
			}
		}
		if (num < n_syn + sl_slots) {
			n_syn = (int)MIN((uint32_t)n_syn, (uint32_t)num);
			sl_slots = (int)MAX(0, num - n_syn);
		}

		int total_tx = n_syn + sl_slots;
		uint16_t sample_count = 0;
		if (total_tx > (int)batch) {
			total_tx = batch;
		}

		/* allocate all mbufs */
		int alloc_ok = 0;
		for (int i = 0; i < total_tx; i++) {
			syn_mbufs[i] = rte_pktmbuf_alloc(pool);
			if (!syn_mbufs[i]) {
				break;
			}
			alloc_ok++;
		}
		total_tx = alloc_ok;
		n_syn = MIN(n_syn, total_tx);
		sl_slots = total_tx - n_syn;

		/* fill SYNs (handshake) */
		for (int i = 0; i < n_syn; i++) {
			rte_pktmbuf_append(syn_mbufs[i], TCP_PKT_SIZE);

			uint32_t src_ip = RANDOM_IP_SRC(cnode);
			uint32_t dst_ip = RANDOM_IP_DST(cnode);
			uint16_t src_port = RANDOM_TCP_SRC(cnode);
			uint16_t dst_port = RANDOM_TCP_DST(cnode);
			uint32_t my_seq = fast_rand_next();

			tcp_hs_fill(syn_mbufs[i],
				(struct rte_ether_addr *)cnode->ether.src,
				(struct rte_ether_addr *)cnode->ether.dst,
				src_ip, dst_ip, src_port, dst_port,
				rte_cpu_to_be_32(my_seq), 0,
				RTE_TCP_SYN_FLAG);

			hs_insert(hs, src_ip, dst_ip, src_port, dst_port,
				  1, my_seq, 0);
		}

		/* fill stateless traffic (uses existing bless_mbufs_tcp
		 * with flag diversity) */
		if (sl_slots > 0 && n_syn < total_tx) {
			bless_mbufs_tcp(&syn_mbufs[n_syn], sl_slots, cnode);

			for (int i = 0; i < sl_slots; i++) {
				uint16_t pos = (uint16_t)(n_syn + i);
				if (!entropy_sampler_should_sample(&w->sampler, pos)) {
					continue;
				}
				sample_positions[sample_count] = pos;
				entropy_extract_5tuple(syn_mbufs[pos], 2,
					&sample_tuples[sample_count]);
				sample_count++;
			}
		}

		if (total_tx > 0) {
			uint16_t sent = rte_eth_tx_burst(port, txq,
				syn_mbufs, total_tx);
			uint64_t tx_delta = sent
				? entropy_sampler_tx_delta(&w->sampler, rte_rdtsc()) : 0;
			for (uint16_t si = 0; si < sample_count; si++) {
				if (sample_positions[si] < sent) {
					entropy_sampler_commit(&w->sampler,
							       &sample_tuples[si], tx_delta);
				}
			}
			entropy_sampler_advance(&w->sampler, sent);
			if (sent) {
				uint64_t hs_txb = 0;
				for (uint16_t hi = 0; hi < sent; hi++)
					hs_txb += syn_mbufs[hi]->pkt_len;
				metric_tx_account(sent, hs_txb);
				rate_psd_account(&w->psd, sent, rte_rdtsc());
			}
			hs->syn_sent += MIN(sent, (uint16_t)n_syn);
			/* consume CPS tokens */
			if (w->cps_bucket.cir > 0) {
				token_bucket_consume(&w->cps_bucket,
						     MIN(sent, (uint16_t)n_syn));
			}
			num -= sent;
		}

		/* Phase 2: RX -- parse & respond */
		uint16_t nb_rx = rte_eth_rx_burst(port, rxq, rx_mbufs, batch);
		int n_resp = 0;

		for (uint16_t i = 0; i < nb_rx; i++) {
			struct rte_mbuf *m = rx_mbufs[i];
			struct rte_ether_hdr *eth =
				rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
			/* PCAP may capture frames injected by this same process.
			 * They are not peer traffic and must not complete a local
			 * handshake against our own state table. */
			if (rte_is_same_ether_addr(&eth->src_addr,
						   (struct rte_ether_addr *)cnode->ether.src)) {
				goto rx_free;
			}
			if (rte_be_to_cpu_16(eth->ether_type) != RTE_ETHER_TYPE_IPV4) {
				goto rx_free;
			}
			struct rte_ipv4_hdr *ip =
				(struct rte_ipv4_hdr *)(eth + 1);
			if (ip->next_proto_id != IPPROTO_TCP) {
				goto rx_free;
			}
			struct rte_tcp_hdr *tcp =
				(struct rte_tcp_hdr *)(ip + 1);

			uint8_t flags = tcp->tcp_flags;
			struct hs_entry *e = hs_lookup(hs,
				ip->src_addr, ip->dst_addr,
				tcp->src_port, tcp->dst_port);

			if (flags & RTE_TCP_RST_FLAG) {
				if (e) { hs_remove(e); hs->count--; }
				/* flow entropy: RST received */
				if (e) {
					flow_sampler_record(&hs->flow_sampler,
							    e->src_ip, e->dst_ip, e->src_port, e->dst_port,
							    3, now - e->creation_tsc);
				}
				hs->rst_recv++;
				goto rx_free;
			}

			if (flags & RTE_TCP_SYN_FLAG) {
				/* pure SYN (no ACK) -> initiate new responder entry */
				if (!(flags & RTE_TCP_ACK_FLAG) && !e) {
					uint32_t my_seq = fast_rand_next();
					uint32_t peer_seq_be = tcp->sent_seq;

					struct rte_mbuf *rm;
					if (n_resp < batch &&
					    (rm = rte_pktmbuf_alloc(pool))) {
						rte_pktmbuf_append(rm, TCP_PKT_SIZE);
						/* swap MAC for response */
						tcp_hs_fill(rm,
							&eth->dst_addr,
							&eth->src_addr,
							ip->dst_addr,
							ip->src_addr,
							tcp->dst_port,
							tcp->src_port,
							rte_cpu_to_be_32(my_seq),
							rte_cpu_to_be_32(
								rte_be_to_cpu_32(peer_seq_be) + 1),
							RTE_TCP_SYN_FLAG | RTE_TCP_ACK_FLAG);
						resp_mbufs[n_resp++] = rm;
					}

					hs_insert(hs,
						ip->dst_addr, ip->src_addr,
						tcp->dst_port, tcp->src_port,
						2, my_seq,
						rte_be_to_cpu_32(peer_seq_be) + 1);
					hs->syn_recv++;
					hs->synack_sent++;
				}
				goto rx_free;
			}

			if (flags & RTE_TCP_ACK_FLAG) {
				if (e && e->state == 1) {
					/* SYN-ACK responding to our SYN */
					if (conf->latency_hist_enable) {
						uint64_t hz = rte_get_tsc_hz();
						uint64_t delta = now - e->creation_tsc;
						uint32_t us = hz > 0
							? (uint32_t)(delta * 1000000ULL / hz)
							: (uint32_t)(delta >> 12);
						latency_hist_add(&w->sampler.lat_hist, us);
					}
					struct rte_mbuf *rm;
					if (n_resp < batch &&
					    (rm = rte_pktmbuf_alloc(pool))) {
						rte_pktmbuf_append(rm, TCP_PKT_SIZE);
						/* ACK to complete handshake */
						tcp_hs_fill(rm,
							&eth->dst_addr,
							&eth->src_addr,
							ip->dst_addr,
							ip->src_addr,
							tcp->dst_port,
							tcp->src_port,
							tcp->recv_ack,
							rte_cpu_to_be_32(
								rte_be_to_cpu_32(tcp->sent_seq) + 1),
							RTE_TCP_ACK_FLAG);
						resp_mbufs[n_resp++] = rm;
					}
					/* flow entropy: ESTABLISHED */
					flow_sampler_record(&hs->flow_sampler,
						e->src_ip, e->dst_ip, e->src_port, e->dst_port,
						1, now - e->creation_tsc);
					e->state = 3; /* ESTABLISHED */
					e->tsc = now;
					hs->synack_recv++;
					hs->established++;
					hs->ack_sent++;
					goto rx_free;
				}
				if (e && e->state == 2) {
					/* ACK completing our SYN-ACK handshake */
					/* flow entropy: ESTABLISHED */
					flow_sampler_record(&hs->flow_sampler,
						e->src_ip, e->dst_ip, e->src_port, e->dst_port,
						1, now - e->creation_tsc);
					e->state = 3;
					e->tsc = now;
					hs->established++;
					goto rx_free;
				}
			}

rx_free:
			rte_pktmbuf_free(m);
		}

		/* Phase 3: TX responses (SYN-ACKs / ACKs) */
		if (n_resp) {
			uint16_t sent = rte_eth_tx_burst(port, txq,
				resp_mbufs, n_resp);
			(void)sent;
		}

		/* Phase 4: timeout cleanup + RST */
		hs_cleanup_chunk(hs, rte_rdtsc(), timeout_cycles,
			pool,
			(struct rte_ether_addr *)cnode->ether.src,
			(struct rte_ether_addr *)cnode->ether.dst);
		if (hs->n_rst) {
			uint16_t sent = rte_eth_tx_burst(port, txq,
				hs->rst_mbufs, hs->n_rst);
			hs->rst_sent += sent;
			hs->n_rst = 0;
		}

		/* Phase 5: batch delay */
		if (unlikely(conf->batch_delay_us)) {
			rte_delay_us(conf->batch_delay_us);
		}
	}

EXIT:
	LOG_INFO("core %u handshake exit (syn_sent=%lu estab=%lu)",
		 cv->core, hs->syn_sent, hs->established);
	/* cleanup heap allocations */
	handshake_ctxs[lcore_id] = NULL;
	rte_free(sample_positions);
	rte_free(sample_tuples);
	rte_free(rx_mbufs);
	rte_free(resp_mbufs);
	rte_free(syn_mbufs);
	rte_mempool_free(pool);
	rte_free(hs);
	w->hs_ctx = NULL;
	return 0;
}
