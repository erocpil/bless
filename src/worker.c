#include "bless.h"
#include "cnode.h"
#include "define.h"
#include "worker.h"
#include "worker_handshake.h"
#include "entropy_stats.h"
#include "metric.h"
#include "log.h"
#include "server.h"
#include "bless_plugin.h"
#include <stdatomic.h>
#include <stdint.h>
#include <sched.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <math.h>
#include <stdlib.h>  /* free */
#include "rate_psd.h"
#include "timing_policy.h"
#include "runtime_field.h"

/* DO NOT USE ANY RTE_DEFINE_PER_LCORE() !!! */

/* per-lcore entropy sampler registry.
 * Sized at RTE_MAX_LCORE (typically 128, ~1 KB).  Standard DPDK practice --
 * could be dynamically allocated if lcore count becomes a constraint. */
struct entropy_sampler *entropy_samplers[RTE_MAX_LCORE];

/* per-lcore handshake ctx registry (NULL if not handshake mode).
 * Sized at RTE_MAX_LCORE for consistency with entropy_samplers. */
struct handshake_ctx *handshake_ctxs[RTE_MAX_LCORE];

static atomic_uint_fast16_t effective_batch;
static atomic_uchar effective_traffic_model;
static atomic_uint_fast64_t effective_batch_delay_us;
static atomic_uint_fast64_t effective_batch_jitter_us;
static atomic_uint_fast32_t effective_sample_interval;

/* Keep the sampler phase aligned with fast_rand_init()'s per-lcore stream. */
static uint64_t worker_sampler_seed(void)
{
	uint64_t master_seed = fast_rand_get_seed();
	if (!master_seed) {
		return 0;
	}
	uint64_t seed = master_seed + rte_lcore_id();
	return seed ? seed : 1;
}

void
worker_effective_config_snapshot(struct stats_snapshot *s)
{
	s->effective_batch = (uint16_t)atomic_load(&effective_batch);
	s->effective_traffic_model = atomic_load(&effective_traffic_model);
	s->effective_batch_delay_us = atomic_load(&effective_batch_delay_us);
	s->effective_batch_jitter_us = atomic_load(&effective_batch_jitter_us);
	s->effective_sample_interval = atomic_load(&effective_sample_interval);
}

#define swap_mac(eth_hdr) \
	do { \
		struct rte_ether_addr addr; \
		rte_ether_addr_copy(&eth_hdr->dst_addr, &addr); \
		rte_ether_addr_copy(&eth_hdr->src_addr, &eth_hdr->dst_addr); \
		rte_ether_addr_copy(&addr, &eth_hdr->src_addr); \
	} while (0)

static void worker_show_cpuset(const cpu_set_t *cpuset)
{
	for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
		if (CPU_ISSET(cpu, cpuset)) {
			printf("%d ", cpu);
		}
	}
	printf("\n");
}

void worker_show(const struct worker *w)
{
	LOG_INFO("worker         %p", w);
	LOG_SHOW("  mode         %d", w->mode);
	LOG_SHOW("  numa         %u", w->cv.numa);
	LOG_SHOW("  core         %u", w->cv.core);
	LOG_SHOW("  port         %u", w->cv.port);
	LOG_SHOW("  type         %u", w->cv.type);
	LOG_SHOW("  txq          %u", w->cv.txq);
	LOG_SHOW("  rxq          %u", w->cv.rxq);
	LOG_META_NNL("  cpuset       ");
	worker_show_cpuset(&w->cpuset);
	LOG_SHOW("  name         %s", w->name);
	LOG_SHOW("  mbufs        %p", w->mbufs);
	LOG_SHOW("  rx_mbufs     %p", w->rx_mbufs);
	LOG_SHOW("  conf         %p", &w->conf);
	LOG_SHOW("  core view    %p", &w->cv);
	LOG_SHOW("  cnode        %p", &w->cnode);
}

int worker_check_state(
		atomic_int *state, struct base_core_view *cv, int64_t num,
		struct entropy_sampler *sampler)
{
	uint64_t val = atomic_load_explicit(state, memory_order_acquire);
	uint64_t i = 0;

	if (val == STATE_EXIT) {
		return CHECK_EXIT;
	}

	if (unlikely(val != STATE_RUNNING)) {
		i = 0;
		while (unlikely(val == STATE_STOPPED)) {
			if (!i++) {
				LOG_INFO("Detect STOPPED @ core %u", cv->core);
			}
			rte_delay_ms(1000);
			val = atomic_load_explicit(state, memory_order_acquire);
		}
		while (unlikely(val == STATE_INIT)) {
			if (!i++) {
				LOG_INFO("Detect INIT @ core %u", cv->core);
			}
			rte_delay_ms(1000);
			val = atomic_load_explicit(state, memory_order_acquire);
		}
		if (val == STATE_EXIT) {
			LOG_INFO("Detect EXIT @ core %u", cv->core);
			return CHECK_EXIT;
		}
		if (val == STATE_RUNNING) {
			LOG_INFO("Detect START @ core %u %u %u", cv->core, cv->port, cv->rxq);
			/* Re-derive per-core PRNG seed from g_master_seed so
			 * runtime seed changes take effect on the next start. */
			fast_rand_reseed();
			entropy_sampler_set_seed(sampler, worker_sampler_seed());
		}
	}

	if (unlikely(num <= 0)) {
		sleep(1);
		atomic_store(state, STATE_EXIT);
		LOG_INFO("Finished: lcore_id %u @ core %u port %u rxq %u",
				rte_lcore_id(), cv->core, cv->port, cv->rxq);
		return CHECK_EXIT;
	}

	return CHECK_CONTINUE;
}


/* No-op worker: spins until STOP/EXIT signal.
 *
 * Used when an lcore is allocated but not assigned TX or RX duty. */
int worker_func_none(struct worker *w)
{
	LOG_WARN("core %u doing nothing", w->cv.core);

	atomic_int *state = w->conf.state;

	while (1) {
		if (worker_check_state(state, &w->cv, 1, &w->sampler) == CHECK_EXIT) {
			goto EXIT;
		}
	}

	LOG_INFO("core %u exit normally", w->cv.core);
	return 0;

EXIT:
	LOG_INFO("core %u exit", w->cv.port);
	return 0;
}

/* TX-only worker: constructs packets via bless_mbufs() and sends them.
 *
 * Per-burst flow:
 *   1. Checks global state (START/STOP/EXIT) and finite packet count.
 *   2. Synchronises PPS rate from the live conf (entropy-adaptive updates).
 *   3. Token-bucket rate-limits by PPS and BPS.
 *   4. For each packet: fast_rand() -> type dispatch table -> bless_mbufs()
 *      -> stage entropy samples selected for this burst.
 *   5. Applies batch_delay_us (constant or exponential for traffic model 1).
 *   6. Sends via rte_eth_tx_burst().
 *
 * Runtime configuration (pps_rate, wire_mtu, interleave_depth) is read
 * live from base->bconf, not from the per-worker snapshot. */
int worker_func_tx_only(struct worker *w)
{
	LOG_INFO("core %u tx only", w->cv.core);

	if (!w) {
		LOG_ERR("Not a worker");
		return -1;
	}

	struct bless_conf *conf = &w->conf;
	int64_t num = conf->num < 0 ? INT64_MAX : conf->num;
	atomic_int *state = conf->state;

	uint16_t batch = conf->batch;
	uint64_t batch_delay_us = conf->batch_delay_us;
	uint64_t batch_jitter_us = conf->batch_jitter_us;
	(void)batch_jitter_us;  /* used by traffic_model=0 only */
	if (batch > 2048) {
		batch = 2048;
		LOG_INFO("batch -> 2048");
	}
	if (batch > num) {
		batch = num;
	}
	struct base_core_view *cv = &w->cv;
	uint16_t nb_tx = batch;


	w->mbufs = rte_malloc(NULL, batch * sizeof(struct rte_mbuf *), RTE_CACHE_LINE_SIZE);
	if (unlikely(!w->mbufs)) {
		LOG_ERR("tx_only: mbufs alloc failed -- lcore %u disabled", w->cv.core);
		return -1;
	}
	struct rte_mbuf **mbufs = w->mbufs;
	struct rte_mbuf **sample_mbufs = rte_malloc(NULL,
		batch * sizeof(*sample_mbufs), RTE_CACHE_LINE_SIZE);
	struct entropy_5tuple *sample_tuples = rte_malloc(NULL,
		batch * sizeof(*sample_tuples), RTE_CACHE_LINE_SIZE);
	if (!sample_mbufs || !sample_tuples) {
		LOG_ERR("tx_only: sampler staging alloc failed -- lcore %u disabled",
			w->cv.core);
		return -1;
	}

	char *name = w->name;
	struct rte_mempool *pktmbuf_pool = bless_create_pktmbuf_pool(conf->batch << 1, name);

	if (-1 == bless_alloc_mbufs(pktmbuf_pool, mbufs, conf->batch)) {
		LOG_ERR("tx_only: bless_alloc_mbufs(%s) failed -- lcore %u disabled",
			name, w->cv.core);
		return -1;
	} /* bench_mode block moved below after cnode decl */

	LOG_META("cpu %u lcore %u core %u port %u txq %u tid %lu",
			sched_getcpu(), rte_lcore_id(), cv->core,
			cv->port, cv->txq, pthread_self());
	assert(cv->core == rte_lcore_id());

	Cnode *cnode = &w->cnode;

	/* bench template: pre-build one fixed packet, reuse via memcpy */
	struct rte_mbuf *template_mbuf = NULL;
	uint16_t template_pkt_len = 0;
	if (conf->bench_mode == 1) {
		template_mbuf = rte_pktmbuf_alloc(pktmbuf_pool);
		if (!template_mbuf) {
			LOG_ERR("bench: template alloc failed -- lcore %u", cv->core);
			return -1;
		}
		bless_mbufs_udp(&template_mbuf, 1, cnode);
		template_pkt_len = template_mbuf->pkt_len;
		LOG_INFO("bench template: pkt_len=%u", template_pkt_len);
	}
	uint64_t pps_sync_tsc = 0;   /* PPS rate sync from entropy-adapt */

	/* Pareto ON-OFF state (traffic_model == 2) */
	uint64_t on_off_rem = 0;     /* remaining bursts in current state */
	int       on_off_is_on = 1;  /* 1=ON (sending), 0=OFF (silent) */

	while (1) {
		uint16_t sample_count = 0;
		uint8_t traffic_model = conf->base->bconf->traffic_model;
		atomic_store(&effective_traffic_model, traffic_model);
		atomic_store(&effective_sample_interval,
			atomic_load_explicit(&w->sampler.sample_interval,
				memory_order_relaxed));
		if (worker_check_state(state, cv, num, &w->sampler) == CHECK_EXIT) {
			goto EXIT;
		}

		/* sync PPS rate from config (entropy-adaptive updates) */
		{
			uint64_t _now = rte_rdtsc();
			if (_now - pps_sync_tsc > rte_get_tsc_hz()) {
				/* Read pps_rate from base->bconf (live WS-updated),
				 * NOT from w->conf (immutable snapshot copy).
				 * Use _Atomic shadow to avoid data races. */
				uint32_t live_pps = runtime_control_load_pps_rate(
					&conf->base->bconf->runtime);
				if (live_pps != w->pps_bucket.cir) {
					token_bucket_set_rate(&w->pps_bucket,
							      live_pps);
				}
				pps_sync_tsc = _now;
			}
		}

		/* PPS / BPS token bucket rate limiters */
		uint64_t pps_avail = token_bucket_available(&w->pps_bucket);
		uint64_t bps_avail = token_bucket_available(&w->bps_bucket);
		uint16_t effective = nb_tx;
		if (pps_avail < (uint64_t)effective) {
			effective = (uint16_t)pps_avail;
		}
		if (effective > 0 && bps_avail < (uint64_t)effective * 64) {
			effective = (uint16_t)(bps_avail / 64);
		}

		if (effective == 0) {
			/* When pps_rate is active, cap delay so the rate limiter
			 * actually controls throughput instead of batch_delay_us. */
			uint64_t _rl_delay = w->pps_bucket.cir > 0
				? (uint64_t)batch * 1000000ULL / w->pps_bucket.cir : 0;
			uint64_t _eff_delay = batch_delay_us ? batch_delay_us : 1000;
			if (_rl_delay > 0 && _rl_delay < _eff_delay) {
				_eff_delay = _rl_delay > 0 ? _rl_delay : 1;
			}
			if (traffic_model == 1) {
				rte_delay_us(exp_random(_eff_delay));
			} else {
				rte_delay_us(_eff_delay);
			}
			continue;
		}

		/* Account packet construction (build): everything from here
		 * through the shuffle below is send work, distinct from the
		 * pacing busy-wait accounted inside pacing_submit(). */
		uint64_t build_start = rte_rdtsc();
		if (conf->bench_mode == 1) {
			/* bench template fast path: copy pre-built packet */
			const uint8_t *tmpl = rte_pktmbuf_mtod(template_mbuf, const uint8_t*);
			for (int j = 0; j < effective; j++) {
				rte_pktmbuf_reset(mbufs[j]);
				if (unlikely(!rte_pktmbuf_append(mbufs[j], template_pkt_len))) {
					rte_pktmbuf_free(mbufs[j]);
					mbufs[j] = mbufs[effective - 1];
					effective--;
					j--;
					continue;
				}
				rte_memcpy(rte_pktmbuf_mtod(mbufs[j], uint8_t*), tmpl, template_pkt_len);
			}
		} else {
			for (int j = 0; j < effective; j++) {
			int index = fast_rand_next() & conf->dist->mask;
			enum BLESS_TYPE type = conf->dist->data[index];
			int r = bless_mbufs(&mbufs[j], 1, type, (void*)cnode);
			if (!r) {
				LOG_ERR("Cannot bless_mbuf() -- freeing mbuf");
				rte_pktmbuf_free(mbufs[j]);
				/* swap with last to keep burst contiguous */
				mbufs[j] = mbufs[effective - 1];
				effective--;
				j--;
				continue;
			}
			if (entropy_sampler_should_sample(&w->sampler,
							(uint16_t)j)) {
				sample_mbufs[sample_count] = mbufs[j];
				entropy_extract_5tuple(mbufs[j], type,
					&sample_tuples[sample_count]);
				sample_count++;
			}
			/* optional erroneous mutation */
			if (cnode->erroneous.ratio > 0 && cnode->erroneous.n_mutation) {
				uint64_t tsc = fast_rand_next();
				tsc = tsc ^ (tsc >> 8);
				if ((tsc & 1023) < cnode->erroneous.ratio) {
					int n = tsc % cnode->erroneous.n_mutation;
					mutation_func func = cnode->erroneous.func[n];
					int r = func((void**)&mbufs[j], 1, (void*)cnode);
					if (!r) {
						LOG_WARN("Cannot mutate(%d) -- skipping", n);
					}
				}
			}
		}
		/* optional Fisher-Yates shuffle -- break spatial locality so that
		 * consecutive packets in the burst belong to different flows.
		 * This exercises the worst-case conntrack / session-table lookup
		 * (every packet -> cache miss) rather than the average case. */
		if (conf->interleave && effective > 1) {
			/* shuffle only the first `depth` fraction of the burst.
			 * depth=100 -> full shuffle (all elements).
			 * depth=50  -> shuffle half the burst, keep rest as-is. */
			int limit = (int)effective * (int)conf->interleave_depth / 100;
			if (limit >= effective) {
				limit = effective;
			}
			if (limit < 2) {
				limit = 2;
			}
			for (int si = limit - 1; si > 0; si--) {
				int sj = fast_rand_next() % (si + 1);
				struct rte_mbuf *tmp = mbufs[si];
				mbufs[si] = mbufs[sj];
				mbufs[sj] = tmp;
			}
		}
		} /* end bench else */
		metric_tx_timing_account(rte_rdtsc() - build_start, 0, 0);
		uint64_t interval_us = timing_effective_interval_us(
			batch_delay_us, (uint32_t)w->pps_bucket.cir, batch);
		if (traffic_model == 0 && batch_jitter_us) {
			interval_us = random_delay_jitter(interval_us, batch_jitter_us);
		} else if (traffic_model == 1) {
			interval_us = exp_random(interval_us ? interval_us : 1000);
		} else if (traffic_model == 2) {
			interval_us = pareto_random(interval_us ? interval_us : 1000,
						    conf->pareto_alpha);
		}
		uint16_t sent = pacing_submit(&w->pacing, mbufs, effective, interval_us);
		uint64_t tx_delta = sent
			? entropy_sampler_tx_delta(&w->sampler, rte_rdtsc()) : 0;
		for (uint16_t si = 0; si < sample_count; si++) {
			for (uint16_t pi = 0; pi < sent; pi++) {
				if (sample_mbufs[si] == mbufs[pi]) {
					entropy_sampler_commit(&w->sampler,
						&sample_tuples[si], tx_delta);
					break;
				}
			}
		}
		entropy_sampler_advance(&w->sampler, sent);
		if (sent) {
			token_bucket_consume(&w->pps_bucket, sent);
			uint64_t txb = 0;
			for (int i = 0; i < sent; i++)
				txb += mbufs[i]->pkt_len;
			token_bucket_consume(&w->bps_bucket, txb);
			metric_tx_account(sent, txb);
			rate_psd_account(&w->psd, sent, rte_rdtsc());
			num -= sent;
		}
		if (sent != effective) {
			rte_pktmbuf_free_bulk(&mbufs[sent], effective - sent);
		}

		/* Pareto ON-OFF state machine (traffic_model == 2) */
		if (traffic_model == 2) {
			if (on_off_rem == 0) {
				/* switch state, generate new duration (min 1 burst) */
				on_off_is_on = !on_off_is_on;
				uint64_t dur = pareto_random(
					on_off_is_on ? batch : 1, conf->pareto_alpha);
				on_off_rem = dur > 0 ? dur : 1;
			}
			if (!on_off_is_on) {
				/* OFF: skip sending */
				rte_delay_us(batch_delay_us ? batch_delay_us : 1000);
				on_off_rem--;
				continue;
			}
			on_off_rem--;
			}
			} /* end while(1) */

			LOG_INFO("core %u exit normally", cv->core);
			return 0;

			EXIT:
	LOG_INFO("core %u exit", cv->core);
	return 0;
}

/* RX-only worker: receives packets and free them -- no forwarding.
 *
 * Captures one-way latency from UDP payload (embedded TSC) when
 * latency_hist_enable is set.  All received mbufs are freed via
 * rte_pktmbuf_free_bulk(). */
int worker_func_rx_only(struct worker *w)
{
	LOG_INFO("core %u rx only", w->cv.core);

	struct base_core_view *cv = &w->cv;
	atomic_int *state = w->conf.state;
	uint16_t port = cv->port;
	uint16_t batch = w->conf.batch;
	uint16_t rxq = cv->rxq;
	struct bless_conf *conf = &w->conf;
	int64_t num = conf->num < 0 ? INT64_MAX : conf->num;

	w->rx_mbufs = rte_malloc(NULL, batch * sizeof(struct rte_mbuf *),
			RTE_CACHE_LINE_SIZE);
	if (unlikely(!w->rx_mbufs)) {
		LOG_ERR("rx_only: rx_mbufs alloc failed -- lcore %u disabled", w->cv.core);
		return -1;
	}

	struct rte_mbuf **rx_mbufs = w->rx_mbufs;

	while (1) {
		if (worker_check_state(state, cv, num, &w->sampler) == CHECK_EXIT) {
			goto EXIT;
		}
		const uint16_t nb_rx = rte_eth_rx_burst(port, rxq, rx_mbufs, batch);
		if (nb_rx) {
			/* latency histogram: extract embedded TSC from UDP/ICMP/TCP packets */
			if (conf->latency_hist_enable) {
				for (uint16_t ri = 0; ri < nb_rx; ri++) {
					const struct rte_ether_hdr *eth =
						rte_pktmbuf_mtod(rx_mbufs[ri], const struct rte_ether_hdr *);
					if (likely(eth->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4))) {
						const struct rte_ipv4_hdr *ip = (const struct rte_ipv4_hdr *)(eth + 1);
						uint64_t tx_tsc = 0;
						uint64_t now = rte_rdtsc();
						uint16_t l2_len = sizeof(struct rte_ether_hdr);
						uint16_t l3_len = sizeof(struct rte_ipv4_hdr);
						if (ip->next_proto_id == IPPROTO_UDP
						    && rx_mbufs[ri]->pkt_len >= l2_len + l3_len
						       + sizeof(struct rte_udp_hdr) + 8) {
							const struct rte_udp_hdr *udp =
								(const struct rte_udp_hdr *)(ip + 1);
							rte_memcpy(&tx_tsc, (const uint8_t *)(udp + 1), 8);
						} else if (ip->next_proto_id == IPPROTO_ICMP
						    && rx_mbufs[ri]->pkt_len >= l2_len + l3_len
						       + sizeof(struct rte_icmp_hdr) + 8) {
							const struct rte_icmp_hdr *icmp =
								(const struct rte_icmp_hdr *)(ip + 1);
							rte_memcpy(&tx_tsc, (const uint8_t *)(icmp + 1), 8);
						} else if (ip->next_proto_id == IPPROTO_TCP) {
							const struct rte_tcp_hdr *tcp =
								(const struct rte_tcp_hdr *)(ip + 1);
							uint16_t tcphdr_len = (tcp->data_off >> 2) & 0x3C;
							if (tcphdr_len < sizeof(struct rte_tcp_hdr)) {
								tcphdr_len = sizeof(struct rte_tcp_hdr);
							}
							if (rx_mbufs[ri]->pkt_len >= l2_len + l3_len
							    + tcphdr_len + 8) {
								rte_memcpy(&tx_tsc,
									(const uint8_t *)tcp + tcphdr_len, 8);
							}
						}
						if (tx_tsc && now > tx_tsc) {
							uint64_t delta = now - tx_tsc;
							uint64_t hz = rte_get_tsc_hz();
							uint32_t us = hz > 0
								? (uint32_t)(delta * 1000000ULL / hz)
								: (uint32_t)(delta >> 12);
							latency_hist_add(&w->sampler.lat_hist, us);
						}
					}
				}
			}
			rte_pktmbuf_free_bulk(rx_mbufs, nb_rx);
			num -= nb_rx;
		}
	}

	LOG_INFO("core %u exit normally", cv->core);
	return 0;

EXIT:
	LOG_INFO("core %u exit", cv->port);
	return 0;
}

/* L2 forwarding worker: RX -> swap MAC addresses -> TX.
 *
 * Reads from rxq, writes to txq on the same port.
 * Latency is measured as the full RTT through the DUT
 * (embedded TSC -> DUT loopback -> return).
 * Only unicast-not-to-self packets are forwarded. */
int worker_func_fwd(struct worker *w)
{
	LOG_INFO("core %u forward", w->cv.core);

	if (!w) {
		LOG_ERR("Not a worker");
		return -1;
	}

	struct bless_conf *conf = &w->conf;
	int64_t num = conf->num < 0 ? INT64_MAX : conf->num;
	atomic_int *state = conf->state;

	uint16_t batch = conf->batch;
	uint64_t batch_delay_us = conf->batch_delay_us;
	uint64_t batch_jitter_us = conf->batch_jitter_us;
	(void)batch_jitter_us;  /* used by traffic_model=0 only */
	if (batch > 2048) {
		batch = 2048;
		LOG_INFO("batch -> 2048");
	}
	if (batch > num) {
		batch = num;
	}
	struct base_core_view *cv = &w->cv;
	uint16_t port = cv->port;
	uint16_t rxq = cv->rxq;
	uint16_t txq = cv->txq;

	w->rx_mbufs = rte_malloc(NULL, batch * sizeof(struct rte_mbuf *),
			RTE_CACHE_LINE_SIZE);
	if (unlikely(!w->rx_mbufs)) {
		LOG_ERR("fwd: rx_mbufs alloc failed -- lcore %u disabled", w->cv.core);
		return -1;
	}
	struct rte_mbuf **rx_mbufs = w->rx_mbufs;

	while (1) {
		if (worker_check_state(state, cv, num, &w->sampler) == CHECK_EXIT) {
			goto EXIT;
		}
		const uint16_t nb_rx = rte_eth_rx_burst(port, rxq, rx_mbufs, batch);
		if (nb_rx) {
			/* latency histogram: extract embedded TSC from UDP packets (RTT through DUT) */
			if (conf->latency_hist_enable) {
				for (uint16_t ri = 0; ri < nb_rx; ri++) {
					const struct rte_ether_hdr *eth =
						rte_pktmbuf_mtod(rx_mbufs[ri], const struct rte_ether_hdr *);
					if (likely(eth->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4))) {
						const struct rte_ipv4_hdr *ip = (const struct rte_ipv4_hdr *)(eth + 1);
						if (ip->next_proto_id == IPPROTO_UDP
						    && rx_mbufs[ri]->pkt_len >= sizeof(struct rte_ether_hdr)
						       + sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_udp_hdr) + 8) {
							const struct rte_udp_hdr *udp =
								(const struct rte_udp_hdr *)(ip + 1);
							uint64_t tx_tsc;
							rte_memcpy(&tx_tsc, (const uint8_t *)(udp + 1), 8);
							uint64_t now = rte_rdtsc();
							uint64_t delta = now - tx_tsc;
							uint64_t hz = rte_get_tsc_hz();
							uint32_t us = hz > 0
								? (uint32_t)(delta * 1000000ULL / hz)
								: (uint32_t)(delta >> 12);
							latency_hist_add(&w->sampler.lat_hist, us);
						}
					}
				}
			}
			int n = nb_rx - 1;
			rte_prefetch0(rte_pktmbuf_mtod(rx_mbufs[0], struct rte_ether_hdr*));
			for (int i = 1, j = 0; i <= n; i++, j++) {
				rte_prefetch0(rte_pktmbuf_mtod(rx_mbufs[i], struct rte_ether_hdr*));
				swap_mac(rte_pktmbuf_mtod(rx_mbufs[j], struct rte_ether_hdr*));
			}
			swap_mac(rte_pktmbuf_mtod(rx_mbufs[n], struct rte_ether_hdr*));
			uint16_t nb_tx = rte_eth_tx_burst(port, txq, rx_mbufs, nb_rx);
			if (nb_tx) {
				uint64_t fwd_bytes = 0;
				for (uint16_t fi = 0; fi < nb_tx; fi++)
					fwd_bytes += rx_mbufs[fi]->pkt_len;
				metric_tx_account(nb_tx, fwd_bytes);
			rate_psd_account(&w->psd, nb_tx, rte_rdtsc());
			}
			if (nb_tx != nb_rx) {
				rte_pktmbuf_free_bulk(&rx_mbufs[nb_tx], nb_rx - nb_tx);
			}
			num -= nb_tx;
		} else {
			if (unlikely(batch_delay_us)) {
				rte_delay_us(random_delay_jitter(batch_delay_us, batch_jitter_us));
			}
		}
	}

	LOG_INFO("core %u exit normally", cv->core);
	return 0;

EXIT:
	LOG_INFO("core %u exit", cv->core);
	return 0;
}

/* Flow-tracking worker (placeholder).
 *
 * Design intent: RX -> classify by 5-tuple -> maintain per-flow state
 * (pps, bytes, packet inter-arrival) -> report to entropy sampler.
 * Unlike fwd (L2 swap), flow mode maintains a flow table and may
 * apply per-flow shaping, marking, or selective forwarding.
 *
 * Not yet implemented. Returns 0 immediately. */
int worker_func_flow(struct worker *w)
{
	LOG_WARN("core %u flow mode not implemented -- no-op", w->cv.core);
	return 0;
}

int (*worker_func[])(struct worker*) = {
	worker_func_none,
	worker_func_tx_only,
	worker_func_rx_only,
	worker_func_fwd,
	worker_func_flow,
	worker_func_handshake,
};


static char* BLESS_MODE_STR[] = {
	"none", "tx_only", "rx_only",
	"fwd", "flow", "handshake", "max",
};

/* Per-lcore worker thread entry point.
 *
 * Allocates worker-local state (mbuf pools, Cnode clone, cpuset),
 * waits on a pthread barrier for all workers to be ready,
 * then dispatches to the appropriate worker_func[mode]().
 *
 * Called from rte_eal_remote_launch(). */
void worker_loop(void *data)
{
	uint32_t lcore_id = rte_lcore_id();
	struct bless_conf *bconf = (struct bless_conf*)data;
	struct worker *worker = rte_malloc(NULL, sizeof(struct worker), 0);
	if (!worker) {
		LOG_ERR("[%s %d] rte_malloc(worker) -- lcore %u disabled",
			__func__, __LINE__, lcore_id);
		pthread_barrier_wait(bconf->barrier);
		while (1) {
			if (atomic_load_explicit(bconf->state, memory_order_acquire) == STATE_EXIT) {
				return;
			}
			rte_pause();
		}
	}
	// cppcheck-suppress nullPointerRedundantCheck
	memset(worker, 0, sizeof(struct worker));

	/* init this worker's entropy sampler */
	entropy_sampler_init(&worker->sampler, bconf->sample_interval,
			     worker_sampler_seed());
	entropy_samplers[lcore_id] = &worker->sampler;
	/* init this worker's rate PSD sampler */
	rate_psd_init(&worker->psd);
	rate_psd_samplers[lcore_id] = &worker->psd;
	pacing_init(&worker->pacing, bconf->base->topo.cv[lcore_id].port,
		bconf->base->topo.cv[lcore_id].txq);
	pacing_register(lcore_id, &worker->pacing);
	/* init rate limiter token buckets */
	token_bucket_init(&worker->pps_bucket,
		(uint64_t)bconf->pps_rate,
		(uint64_t)(bconf->pps_burst ? bconf->pps_burst
			   : (bconf->batch * 4)));
	token_bucket_init(&worker->bps_bucket,
		(uint64_t)bconf->bps_rate,
		(uint64_t)(bconf->bps_burst ? bconf->bps_burst : 1048576));
	/* CPS: derive from hs_rate if set, 0 = disabled */
	if (bconf->hs_rate > 0) {
		token_bucket_init(&worker->cps_bucket, (uint64_t)bconf->hs_rate,
			(uint64_t)(bconf->batch * 4));
	} else {
		memset(&worker->cps_bucket, 0, sizeof(worker->cps_bucket));
	}


	uint8_t mode = bconf->mode;

	if (BLESS_MODE_NONE == mode || mode >= BLESS_MODE_MAX) {
		LOG_WARN("Unsupported bless mode %d", mode);
		goto INIT_FAIL;
	}

	/* set this worker(lcore or pthread) name */
	snprintf(worker->name, sizeof(worker->name), "%s@%u",
			BLESS_MODE_STR[mode], lcore_id);
	int n = pthread_setname_np(pthread_self(), worker->name);
	if (n) {
		printf("pthread_setname_np(%s) %d %d %s", worker->name, n,
				errno, strerror(errno));
	}

	struct bless_conf *conf = &worker->conf;
	memcpy(conf, bconf, offsetof(struct bless_conf, dist_ratio));
	atomic_store(&effective_batch, conf->batch);
	atomic_store(&effective_traffic_model, conf->traffic_model);
	atomic_store(&effective_batch_delay_us, conf->batch_delay_us);
	atomic_store(&effective_batch_jitter_us, conf->batch_jitter_us);
	atomic_store(&effective_sample_interval, conf->sample_interval);

	conf->dist = NULL;
	if (bconf->dist) {
		conf->dist = rte_malloc(NULL, sizeof(struct distribution) +
				sizeof(uint8_t) * bconf->dist->size, 0);
		if (unlikely(!conf->dist)) {
			LOG_ERR("[%s %d] Cannot rte_malloc(distribution) -- lcore %u disabled",
				__func__, __LINE__, lcore_id);
			goto INIT_FAIL;
		}
		memcpy(conf->dist, bconf->dist, sizeof(struct distribution) +
				sizeof(uint8_t) * bconf->dist->size);
		for (int i = 0; i < bconf->dist->size; i++) {
			if (bconf->dist->data[i] != conf->dist->data[i]) {
				LOG_WARN("distribution error: i %d %u %u",
						i, bconf->dist->data[i], conf->dist->data[i]);
				goto INIT_FAIL;
			}
		}
		bless_show_dist(conf->dist);
	}

	Cnode *cnode = &worker->cnode;
	memset(cnode, 0, sizeof(struct Cnode));
	if (unlikely(config_clone_cnode(bconf->cnode, cnode) < 0)) {
		LOG_ERR("[%s %d] config_clone_cnode(%p) -- lcore %u disabled",
			__func__, __LINE__, cnode, lcore_id);
		goto INIT_FAIL;
	}
	/* propagate runtime flags not stored in YAML config */
	cnode->latency_hist_enable = bconf->latency_hist_enable;

	struct base_core_view *cv = &worker->cv;
	memcpy(cv, conf->base->topo.cv + lcore_id, sizeof(struct base_core_view));

	uint16_t port = cv->port;

	/* check if src mac address is provided */
	if (!cnode->ether.n_src) {
		rte_eth_macaddr_get(port, (struct rte_ether_addr*)cnode->ether.src);
		LOG_META_NNL("injector will use local port mac address: ");
		bless_print_mac((struct rte_ether_addr*)cnode->ether.src);
		cnode->ether.n_src = 1;
	}
	/* check if vxlan src mac address is provided */
	if (cnode->vxlan.enable && !cnode->vxlan.ether.n_src) {
		rte_eth_macaddr_get(port, (struct rte_ether_addr*)cnode->vxlan.ether.src);
		LOG_META_NNL("injector vxlan will use local port mac address: ");
		bless_print_mac((struct rte_ether_addr*)cnode->vxlan.ether.src);
		cnode->vxlan.ether.n_src = 1;
	}

	worker->conf.cnode = &worker->cnode;
#if LOG_ENABLE_DEBUG
	cnode_show(conf->cnode, 0);
	cnode_show_summary(conf->cnode);
#endif

	// Get thread CPU affinity
	cpu_set_t *cpuset = &worker->cpuset;
	CPU_ZERO(cpuset);
	int s = pthread_getaffinity_np(pthread_self(), sizeof(cpu_set_t), cpuset);
	if (s != 0) {
		perror("pthread_getaffinity_np");
		LOG_ERR("Failed to get CPU affinity for lcore %u", lcore_id);
		goto INIT_FAIL;
	}

	pthread_barrier_wait(conf->barrier);

	worker_show(worker);

	worker_func[mode](worker);
	return;

INIT_FAIL:
	pthread_barrier_wait(bconf->barrier);
	while (1) {
		if (atomic_load_explicit(bconf->state, memory_order_acquire) == STATE_EXIT) {
			return;
		}
		rte_pause();
	}
}

/* Encode a command-reply string into JSON and push to WebSocket clients.
 *
 * Called by the HTTP control handler to echo command results. */
void worker_generate_cmd_reply(const char *str)
{
	if (!str || !strlen(str)) {
		return;
	}
	char *reply = encode_cmdReply_to_json(str);
	if (!reply) {
		LOG_ERR("cmd_reply: encode_cmdReply_to_json returned NULL");
		return;
	}
	LOG_TRACE("cmdReply %s", reply);
	ws_broadcast_log(reply, strlen(reply));
	free(reply);
}

/* Encode a log message string into JSON and push to WebSocket clients.
 *
 * Called by the log system whenever a real-time log is emitted. */
void worker_generate_log(const char *str)
{
	if (!str || !strlen(str)) {
		return;
	}
	char *msg = encode_log_to_json(str);
	LOG_TRACE("msg %s", msg);
	ws_broadcast_log(msg, strlen(msg));
	free(msg);
}

/* Entropy-adaptive rate limiting */static void adapt_pps_rate(struct bless_conf *bconf, const struct stats_snapshot *s)
{
	double target = runtime_control_load_entropy_target(&bconf->runtime);
	if (target <= 0.0) {
		return;
	}

	double cur = 0.0;
	uint8_t dim = runtime_control_load_entropy_dim(&bconf->runtime);
	switch (dim) {
	case 0: cur = s->entropy_src_ip;       break;
	case 1: cur = s->entropy_dst_ip;       break;
	case 2: cur = s->entropy_src_port;     break;
	case 3: cur = s->entropy_dst_port;     break;
	case 4: cur = s->entropy_protocol;     break;
	case 5: cur = s->entropy_joint_5tuple; break;
	case 6: cur = s->entropy_tcp_flags;    break;
	case 7: cur = s->entropy_pkt_size;     break;
	case 8: cur = s->entropy_delta_tsc;    break;
	default: return;
	}

	double error = target - cur;
	double gain = runtime_control_load_entropy_adapt_gain(&bconf->runtime);
	double adjust = error * gain;
	uint32_t expected_pps =
		runtime_control_load_pps_rate(&bconf->runtime);
	int64_t new_pps = (int64_t)expected_pps + (int64_t)adjust;
	if (new_pps < 0) {
		new_pps = 0;
	}
	if (new_pps > 100000000) {
		new_pps = 100000000; /* cap at 100 Mpps */
	}
	/* A concurrent explicit WS update wins: do not retry a failed CAS. */
	runtime_control_compare_exchange_pps_rate(
		&bconf->runtime, &expected_pps, (uint32_t)new_pps);

	LOG_INFO("entropy-adapt: dim=%u cur=%.3f target=%.3f error=%.3f pps=%u",
		 runtime_control_load_entropy_dim(&bconf->runtime),
		 cur,
		 runtime_control_load_entropy_target(&bconf->runtime),
		 error,
		 runtime_control_load_pps_rate(&bconf->runtime));
}

/* Collect and encode per-port statistics into both JSON and Prometheus text.
 *
 * Aggregates RX/TX counters, rate (Mpps/Gbps), DPDK eth stats,
 * entropy metrics, latency histogram buckets, and handshake-mode
 * CPS/success-rate.  Written to the inactive stats buffer (double-buffered)
 * so the broadcast /metrics handler can read the complete snapshot.
 *
 * Called on a periodic timer by the main_loop thread. */
void worker_generate_stats(uint32_t enabled_port_mask,
			   const struct dist_ratio *dr, const Cnode *cnode,
			   struct bless_conf *bconf)
{
	int active = stats_get_active_index();
	int inactive = active ^ 1;

	struct stats_snapshot *s = stats_get(inactive);

	/* If a reader is still copying from this (inactive) buffer,
	 * skip this cycle.  Readers finish in microseconds and
	 * generation runs on a timer_period (hundreds of ms), so a
	 * single skip has no visible effect on dashboards. */
	if (!stats_snapshot_writable(inactive)) {
		return;
	}

	/* compute entropy from config before encoding */
	compute_entropy_stats(s, dr, cnode, bconf);

	/* compute flow-level entropy (handshake mode) */
	flow_entropy_compute(s);

	/* compute rate PSD (frequency-domain TX rate analysis) */
	rate_psd_compute();
	rate_psd_fill_snapshot(s);

	/* compute observe metrics: throughput rates, CPU, memory */
	compute_rate_metrics(s, enabled_port_mask);
	sample_cpu_usage(s);
	sample_memory_usage(s);
	sample_runtime_noise(s);
	pacing_fill_snapshot(s);
	worker_effective_config_snapshot(s);

	/* entropy-adaptive rate limiting */
	adapt_pps_rate(bconf, s);

	/* drain handshake stats from per-worker contexts */	{
		uint64_t hs_syn_sent = 0, hs_syn_recv = 0;
		uint64_t hs_synack_sent = 0, hs_synack_recv = 0;
		uint64_t hs_ack_sent = 0, hs_established = 0;
		uint64_t hs_rst_sent = 0, hs_rst_recv = 0;
		uint64_t hs_timed_out = 0;
		uint32_t hs_conn_current = 0, hs_conn_max = 0;
		uint64_t cps_start_tsc = 0;
		uint64_t syn_sent_at_start = 0;
		uint64_t last_syn_sent = 0;
		int has_hs = 0;

		unsigned main_lc = rte_get_main_lcore();
		for (unsigned lc = 0; lc < RTE_MAX_LCORE; lc++) {
			struct handshake_ctx *h = handshake_ctxs[lc];
			if (!h || lc == main_lc) {
				continue;
			}
			has_hs = 1;
			hs_syn_sent      += h->syn_sent;
			hs_syn_recv      += h->syn_recv;
			hs_synack_sent   += h->synack_sent;
			hs_synack_recv   += h->synack_recv;
			hs_ack_sent      += h->ack_sent;
			hs_established   += h->established;
			hs_rst_sent      += h->rst_sent;
			hs_rst_recv      += h->rst_recv;
			hs_timed_out     += h->timed_out;
			hs_conn_current  += h->count;
			if (h->conn_max > hs_conn_max) {
				hs_conn_max = h->conn_max;
			}
			cps_start_tsc     = h->cps_start_tsc;
			syn_sent_at_start = h->syn_sent_at_start;
			last_syn_sent     = h->syn_sent;
		}

		if (has_hs) {
			s->hs_syn_sent      = (double)hs_syn_sent;
			s->hs_syn_recv      = (double)hs_syn_recv;
			s->hs_synack_sent   = (double)hs_synack_sent;
			s->hs_synack_recv   = (double)hs_synack_recv;
			s->hs_ack_sent      = (double)hs_ack_sent;
			s->hs_established   = (double)hs_established;
			s->hs_rst_sent      = (double)hs_rst_sent;
			s->hs_rst_recv      = (double)hs_rst_recv;
			s->hs_timed_out     = (double)hs_timed_out;
			s->hs_conn_current  = (double)hs_conn_current;
			s->hs_conn_max      = (double)hs_conn_max;
			s->hs_success_rate  = hs_syn_sent > 0
				? (double)hs_established / (double)hs_syn_sent : 0.0;

			/* CPS: delta over the measurement window */
			uint64_t delta = last_syn_sent - syn_sent_at_start;
			double secs = cps_start_tsc > 0
				? (double)(rte_get_tsc_cycles() - cps_start_tsc)
				  / (double)rte_get_timer_hz() : 1.0;
			s->hs_cps = secs > 0.001 ? (double)delta / secs : 0.0;
		}
	}

	char tmp[128];
	sprintf(tmp, "log: %lu", rte_rdtsc());
	char *msg = encode_stats_to_json(enabled_port_mask, tmp, s);

	/* JSON — checked snprintf: its return value is the "would-be"
	 * length, not the actual bytes stored.  Treat truncation as an
	 * error: restore the previous valid snapshot so readers never
	 * see truncated (broken) JSON. */
	{
		int ret = snprintf(s->json, sizeof(s->json), "%s\n", msg);
		if (ret < 0) {
			LOG_ERR("stats JSON: snprintf returned %d", ret);
			s->json[0] = '\0';
			s->json_len = 0;
		} else if ((size_t)ret >= sizeof(s->json)) {
			/* Truncated — restore previous valid snapshot
			 * from the active buffer. */
			LOG_WARN("stats JSON truncated: needed %d bytes, "
				 "buffer %zu — keeping previous snapshot",
				 ret, sizeof(s->json));
			const struct stats_snapshot *prev =
				stats_get(active);
			s->json_len = prev->json_len;
			if (s->json_len > 0) {
				memcpy(s->json, prev->json,
				       s->json_len + 1);
			}
		} else {
			s->json_len = (size_t)ret;
		}
	}
	free(msg);

	/* Prometheus — encode_stats_to_text already bounds-checks via
	 * the APPEND macro and returns max_len on truncation. */
	s->metric_len = encode_stats_to_text(enabled_port_mask,
			s->metric, sizeof(s->metric), s);
	/* If Prometheus output was truncated, keep the previous
	 * valid metric in place (same rationale as JSON). */
	if (s->metric_len == sizeof(s->metric)) {
		const struct stats_snapshot *prev = stats_get(active);
		s->metric_len = prev->metric_len;
		if (s->metric_len > 0) {
			memcpy(s->metric, prev->metric,
			       s->metric_len + 1);
		}
	}
	s->tsc_cycles = rte_get_tsc_cycles();

	stats_set(inactive);
	/* NOTE: ws_broadcast_stats() is now called by the dedicated
	 * broadcast thread -- no need to block the main loop here. */
}

/* Dedicated control-lcore thread for periodic stats generation.
 *
 * Sleeps 100ms per iteration via clock_nanosleep (absolute time); after
 * timer_period elapses it calls worker_generate_stats() to produce a fresh
 * snapshot.  Also runs entropy-adaptive PPS rate limiting (adapt_pps_rate).
 *
 * Spun up via rte_eal_remote_launch() on the control lcore. */
void worker_main_loop(void *data)
{
	unsigned int lcore_id = rte_lcore_id();
	struct bless_conf *conf = (struct bless_conf*)data;
	struct base_core_view *cv = conf->base->topo.cv + lcore_id;
	atomic_int *state = conf->state;
	uint64_t timer_period = conf->timer_period;
	uint64_t prev_tsc = 0, diff_tsc = 0, cur_tsc = 0, timer_tsc = 0;
	uint64_t val = 0;
	struct timespec wake;

	pthread_barrier_wait(conf->barrier);

	clock_gettime(CLOCK_MONOTONIC, &wake);

	while ((val = atomic_load_explicit(state, memory_order_acquire)) != STATE_EXIT) {
		static uint64_t i = 0;
		/* Absolute 100 ms periodic sleep.  TIMER_ABSTIME anchors the wake
		 * cadence to a fixed schedule instead of drifting by the
		 * per-iteration execution time; EINTR is retried against the same
		 * absolute target. */
		wake.tv_nsec += 100000000L;
		if (wake.tv_nsec >= 1000000000L) {
			wake.tv_sec += wake.tv_nsec / 1000000000L;
			wake.tv_nsec %= 1000000000L;
		}
		while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME,
				       &wake, NULL) == EINTR)
			;
		cur_tsc = rte_rdtsc();
		diff_tsc = cur_tsc - prev_tsc;
		timer_tsc += diff_tsc;
		prev_tsc = cur_tsc;
		/* if timer has reached its timeout */
		if (unlikely(timer_tsc >= timer_period * 1)) {
			timer_tsc = 0;
			if (unlikely(val == STATE_STOPPED)) {
				if (i) {
					worker_generate_stats(conf->enabled_port_mask,
							&conf->dist_ratio, conf->cnode,
							conf);
				}
				i = 0;
			} else {
				worker_generate_stats(conf->enabled_port_mask,
						&conf->dist_ratio, conf->cnode,
						conf);
				i++;
			}
		}
	}

	/* --stats-dump: one final entropy snapshot after all workers finish.
	 * All workers are done (STATE_EXIT set by each), so ring buffers
	 * contain the complete packet set.  Two runs with the same --seed
	 * produce byte-identical JSON (modulo meta.timestamp_ns). */
	if (conf->stats_dump_path) {
		worker_generate_stats(conf->enabled_port_mask,
				      &conf->dist_ratio, conf->cnode, conf);
		const struct stats_snapshot *s = stats_snapshot_acquire();
		if (s && s->json_len) {
			FILE *f = fopen(conf->stats_dump_path, "w");
			if (f) {
				fwrite(s->json, 1, s->json_len, f);
				fclose(f);
				LOG_INFO("stats dumped to %s (%zu bytes)",
					 conf->stats_dump_path, s->json_len);
			} else {
				LOG_ERR("cannot open %s: %s",
					conf->stats_dump_path, strerror(errno));
			}
		}
		stats_snapshot_release(s);
	}

	LOG_INFO("main core %u exit", cv->core);
}

const static char *ws_json_get_string(cJSON *obj, const char *key)
{
	cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
	return cJSON_IsString(item) ? item->valuestring : NULL;
}

/* WebSocket user-data callback -- handles incoming WS messages.
 *
 * The callback receives raw WebSocket data (command, config update,
 * live control) and dispatches to the relevant handler.
 *
 * Registered as part of the civetweb WebSocket server. */
void ws_user_func(struct mg_connection *conn, void *user,
		  void *data, size_t size)
{
	if (!data || !size) {
		return;
	}

	struct ws_user_data *wsud = (struct ws_user_data*)user;
	struct base *base = (struct base*)wsud->data;
	atomic_int *state = base->g_state;
	struct config_file_map *cfm = &base->config->cfm;

	/* Use length-aware parse to avoid dependence on NUL termination */
	cJSON *root = cJSON_ParseWithLength((const char *)data, size);
	if (!root) {
		/* Return a structured error response so clients can
		 * distinguish malformed input from silent failure. */
		cJSON *err = cJSON_CreateObject();
		if (err) {
			cJSON_AddStringToObject(err, "error",
						"invalid JSON");
			cJSON_AddNumberToObject(err, "code", 400);
			char *err_str = cJSON_PrintUnformatted(err);
			if (err_str) {
				mg_websocket_write(conn,
					MG_WEBSOCKET_OPCODE_TEXT,
					err_str, strlen(err_str));
				free(err_str);
			}
			cJSON_Delete(err);
		}
		return;
	}

	LOG_TRACE("state %p %p", state, base->g_state);

	const char *cmd = ws_json_get_string(root, "cmd");
	char reply_buf[256];  /* reply buffer for "set" handler -- function scope to avoid use-after-scope */
	if (cmd) {
		LOG_TRACE("cmd = %s", cmd);
		if (strcmp(cmd, "start") == 0) {
			LOG_TRACE("Received START command");
			atomic_store(state, STATE_RUNNING);
			cmd = "started";
		} else if (strcmp(cmd, "stop") == 0) {
			LOG_TRACE("Received STOP command");
			atomic_store(state, STATE_STOPPED);
			cmd = "stopped";
		} else if (strcmp(cmd, "init") == 0) {
			LOG_TRACE("Received INIT command");
			/* Full reinit (ports, queues, mempool) is not supported --
			 * DPDK device lifecycles are fragile mid-process and
			 * cold restart is fast enough.  Use `set` for runtime
			 * parameter changes instead. */
			cmd = "reinit not supported -- restart the process";
		} else if (strcmp(cmd, "exit") == 0) {
			LOG_TRACE("Received EXIT command");
			atomic_store(state, STATE_EXIT);
			cmd = "exited";
		} else if (strcmp(cmd, "conf") == 0) {
			LOG_TRACE("Received conf command");
			cmd = (char*)cfm->addr;
			printf("name %s\n", cfm->name);
		} else if (strcmp(cmd, "set") == 0) {
			/* Runtime parameter adjustment */
			struct bless_conf *bconf = base->bconf;
			if (!bconf) {
				cmd = "error: not inited";
			} else {
			const char *key = ws_json_get_string(root, "key");
			cJSON *val = cJSON_GetObjectItemCaseSensitive(root, "value");
			if (key && val) {
				/* ── special fields: seed (global PRNG, not in bless_conf) ── */
				if (strcmp(key, "seed") == 0) {
					if (cJSON_IsNumber(val)) {
						double d = cJSON_GetNumberValue(val);
						if (isfinite(d) && d >= 0.0 &&
						    d < 0x1p64 &&
						    d == (double)((uint64_t)d)) {
							fast_rand_set_seed((uint64_t)d);
							snprintf(reply_buf, sizeof(reply_buf),
								 "set seed=%lu (effective on next start)",
								 (unsigned long)fast_rand_get_seed());
						} else {
							snprintf(reply_buf, sizeof(reply_buf),
								 "seed: invalid value");
						}
					} else {
						snprintf(reply_buf, sizeof(reply_buf),
							 "seed: value must be a number");
					}
					cmd = reply_buf;
				} else {
					/* ── descriptor-driven set ── */
					const struct runtime_field *f =
						runtime_field_lookup(key);
					if (!f) {
						snprintf(reply_buf, sizeof(reply_buf),
							 "set: unknown key '%s'", key);
						cmd = reply_buf;
					} else {
						struct field_value fv = {0};
						if (!f->validate(val, f, &fv)) {
							snprintf(reply_buf, sizeof(reply_buf),
								 "set: invalid value for '%s'", key);
							cmd = reply_buf;
						} else if (f->mutability == FIELD_STARTUP) {
							snprintf(reply_buf, sizeof(reply_buf),
								 "set: '%s' is startup-only (%s)",
								 key, f->apply_desc ? f->apply_desc : "restart required");
							cmd = reply_buf;
						} else {
							/* serialise: CivetWeb has 4 worker threads */
							runtime_control_lock(&bconf->runtime);

								/* write to bless_conf */
								switch (f->type) {
								case FIELD_U8:
									*(uint8_t *)((char *)bconf + f->offset) =
										(uint8_t)fv.u64;
									break;
								case FIELD_U16:
									*(uint16_t *)((char *)bconf + f->offset) =
										(uint16_t)fv.u64;
									break;
								case FIELD_U32:
									*(uint32_t *)((char *)bconf + f->offset) =
										(uint32_t)fv.u64;
									break;
								case FIELD_U64:
									*(uint64_t *)((char *)bconf + f->offset) =
										fv.u64;
									break;
								case FIELD_I64:
									*(int64_t *)((char *)bconf + f->offset) =
										fv.i64;
									break;
								case FIELD_DOUBLE:
									/* cppcheck-suppress invalidPointerCast */
									*(double *)((char *)bconf + f->offset) =
										fv.f64;
									break;
								}
								/* post-apply callback (e.g. propagate to workers) */
								if (f->apply) {
									f->apply(bconf);
								}

							runtime_control_unlock(&bconf->runtime);

							/* generate reply */
							switch (f->type) {
							case FIELD_U32:
							case FIELD_U16:
							case FIELD_U8:
								snprintf(reply_buf, sizeof(reply_buf),
									 "set %s=%lu", key,
									 (unsigned long)fv.u64);
								break;
							case FIELD_U64:
								snprintf(reply_buf, sizeof(reply_buf),
									 "set %s=%llu", key,
									 (unsigned long long)fv.u64);
								break;
							case FIELD_I64:
								snprintf(reply_buf, sizeof(reply_buf),
									 "set %s=%lld", key,
									 (long long)fv.i64);
								break;
							case FIELD_DOUBLE:
								snprintf(reply_buf, sizeof(reply_buf),
									 "set %s=%.3f", key, fv.f64);
								break;
							}
							cmd = reply_buf;
						}
					}
				}
			} else {
				cmd = "set: missing 'key' or 'value'";
			}
			}  // end !bconf
		} else if (strcmp(cmd, "get") == 0) {
			/* Query current runtime config */
			struct bless_conf *bconf = base->bconf;
			cJSON *reply = cJSON_CreateObject();
			if (!reply) {
				cmd = "error: OOM";
			} else if (!bconf) {
				cJSON_Delete(reply);
				cmd = "error: not inited";
			} else {
				char buf[32];
				cJSON_AddStringToObject(reply, "cmd", "config");

				/* serialise: protect plain-field reads from
				 * concurrent WS set handlers */
				runtime_control_lock(&bconf->runtime);

				/* descriptor-driven fields */
				for (unsigned i = 0; i < runtime_field_count; i++)
					runtime_field_get_json(&runtime_fields[i],
							       bconf, reply);

				runtime_control_unlock(&bconf->runtime);

				/* special: seed (global PRNG, not in bless_conf) */
				cJSON_AddNumberToObject(reply, "seed",
					(double)fast_rand_get_seed());

				snprintf(buf, sizeof(buf), "%d", atomic_load(state));
				cJSON_AddStringToObject(reply, "state", buf);
				char *json_str = cJSON_Print(reply);
				worker_generate_cmd_reply(json_str);
				free(json_str);
				cJSON_Delete(reply);
			}
			cmd = NULL;  /* already sent reply */
		} else {
			cmd = "Not supported";
		}
	} else {
		LOG_WARN("cmd missing or not a string");
		cmd = "cmd missing or not a string";
	}

	cJSON_Delete(root);
	if (cmd) {
		worker_generate_cmd_reply(cmd);
	}
}
