#include "injector.h"
#include "cnode.h"
#include "define.h"
#include "worker.h"
#include "log.h"
#include "injector_plugin.h"
#include "bless_dpdk_runtime.h"
#include <stdatomic.h>
#include <stdint.h>
#include <sched.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <stdlib.h>  /* free */

static atomic_uint active_workers;
static atomic_int worker_failed;
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
	LOG_SHOW("  numa         %u", w->cv.numa);
	LOG_SHOW("  core         %u", w->cv.core);
	LOG_SHOW("  port         %u", w->cv.port);
	LOG_SHOW("  type         %u", w->cv.type);
	LOG_SHOW("  txq          %u", w->cv.txq);
	LOG_META_NNL("  cpuset       ");
	worker_show_cpuset(&w->cpuset);
	LOG_SHOW("  name         %s", w->name);
	LOG_SHOW("  mbufs        %p", w->mbufs);
	LOG_SHOW("  conf         %p", &w->conf);
	LOG_SHOW("  core view    %p", &w->cv);
	LOG_SHOW("  cnode        %p", &w->cnode);
}

int worker_check_state(atomic_int *state, struct base_core_view *cv, int64_t num)
{
    if (atomic_load_explicit(state, memory_order_acquire) == STATE_EXIT)
        return CHECK_EXIT;
    if (num <= 0) {
        LOG_INFO("Finished: core %u port %u", cv->core, cv->port);
        return CHECK_EXIT;
    }
    return CHECK_CONTINUE;
}

/* Construct and send packets with optional PPS/BPS limits. */
static int worker_transmit(struct worker *w)
{
	LOG_INFO("core %u tx only", w->cv.core);

	if (!w) {
		LOG_ERR("Not a worker");
		return -1;
	}

	struct injector_conf *conf = &w->conf;
	int result = 0;
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
	if (num == 0) return 0;
	if (batch == 0) return -1;
	if (batch > num) {
		batch = num;
	}
	struct base_core_view *cv = &w->cv;
	uint16_t txq = cv->txq;
	uint16_t packet_lengths[2048];


	w->mbufs = rte_malloc(NULL, batch * sizeof(struct rte_mbuf *), RTE_CACHE_LINE_SIZE);
	if (unlikely(!w->mbufs)) {
		LOG_ERR("tx_only: mbufs alloc failed -- lcore %u disabled", w->cv.core);
		return -1;
	}
	struct rte_mbuf **mbufs = w->mbufs;

	char *name = w->name;
	struct rte_mempool *pktmbuf_pool = br_runtime_tx_pool_create(name,
			batch * 2 + 4096, rte_socket_id());

	if (!pktmbuf_pool) {
		rte_free(w->mbufs);
		w->mbufs = NULL;
		return -1;
	}

	LOG_META("cpu %u lcore %u core %u port %u txq %u tid %lu",
			sched_getcpu(), rte_lcore_id(), cv->core,
			cv->port, cv->txq, pthread_self());
	assert(cv->core == rte_lcore_id());

	uint16_t port = cv->port;
	Cnode *cnode = &w->cnode;

	/* bench template: pre-build one fixed packet, reuse via memcpy */
	struct rte_mbuf *template_mbuf = NULL;
	uint16_t template_pkt_len = 0;
	if (conf->bench_mode == 1) {
		template_mbuf = rte_pktmbuf_alloc(pktmbuf_pool);
		if (!template_mbuf) {
			LOG_ERR("bench: template alloc failed -- lcore %u", cv->core);
			result = -1;
			goto EXIT;
		}
		if (!injector_mbufs_udp(&template_mbuf, 1, cnode) ||
		    template_mbuf->pkt_len == 0) {
			LOG_ERR("bench: template construction failed -- lcore %u", cv->core);
			result = -1;
			goto EXIT;
		}
		template_pkt_len = template_mbuf->pkt_len;
		LOG_INFO("bench template: pkt_len=%u", template_pkt_len);
	}

	/* Pareto ON-OFF state (traffic_model == 2) */
	uint64_t on_off_rem = 0;     /* remaining bursts in current state */
	int       on_off_is_on = 1;  /* 1=ON (sending), 0=OFF (silent) */

	while (1) {
		if (worker_check_state(state, cv, num) == CHECK_EXIT) {
			goto EXIT;
		}

		/* PPS / BPS token bucket rate limiters */
		uint64_t pps_avail = token_bucket_available(&w->pps_bucket);
		uint64_t bps_avail = token_bucket_available(&w->bps_bucket);
		uint16_t effective = num < batch ? (uint16_t)num : batch;
		if (pps_avail < (uint64_t)effective)
			effective = (uint16_t)pps_avail;
		if (effective > 0 && bps_avail < (uint64_t)effective * 64)
			effective = (uint16_t)(bps_avail / 64);

		if (effective == 0) {
			/* When pps_rate is active, cap delay so the rate limiter
			 * actually controls throughput instead of batch_delay_us. */
			uint64_t _rl_delay = w->pps_bucket.cir > 0
				? (uint64_t)batch * 1000000ULL / w->pps_bucket.cir : 0;
			uint64_t _eff_delay = batch_delay_us ? batch_delay_us : 1000;
			if (_rl_delay > 0 && _rl_delay < _eff_delay)
				_eff_delay = _rl_delay > 0 ? _rl_delay : 1;
			if (conf->traffic_model == 1)
				rte_delay_us(exp_random(_eff_delay));
			else
				rte_delay_us(_eff_delay);
			continue;
		}

        if (injector_alloc_mbufs(pktmbuf_pool, mbufs, effective) < 0) {
            rte_delay_us(100);
            continue;
        }
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
			enum INJECTOR_TYPE type = conf->dist->data[index];
			int r = injector_mbufs(&mbufs[j], 1, type, (void*)cnode);
			if (!r) {
				LOG_ERR("Cannot injector_mbuf() -- freeing mbuf");
				rte_pktmbuf_free(mbufs[j]);
				/* swap with last to keep burst contiguous */
				mbufs[j] = mbufs[effective - 1];
				effective--;
				j--;
				continue;
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
			if (limit >= effective) limit = effective;
			if (limit < 2) limit = 2;
			for (int si = limit - 1; si > 0; si--) {
				int sj = fast_rand_next() % (si + 1);
				struct rte_mbuf *tmp = mbufs[si];
				mbufs[si] = mbufs[sj];
				mbufs[sj] = tmp;
			}
		}
		} /* end bench else */
		for (uint16_t i = 0; i < effective; i++)
			packet_lengths[i] = mbufs[i]->pkt_len;
		uint16_t sent = br_runtime_tx_burst(port, txq, mbufs, effective);
		if (sent) {
			token_bucket_consume(&w->pps_bucket, sent);
			uint64_t txb = 0;
			for (int i = 0; i < sent; i++)
				txb += packet_lengths[i];
			token_bucket_consume(&w->bps_bucket, txb);
			num -= sent;
		}
		if (sent != effective) {
			rte_pktmbuf_free_bulk(&mbufs[sent], effective - sent);
		}

		if (unlikely(batch_delay_us)) {
			/* When pps_rate is active, cap delay to match target rate
			 * so batch_delay_us doesn't become the bottleneck. */
			uint64_t _rl_delay = w->pps_bucket.cir > 0
				? (uint64_t)batch * 1000000ULL / w->pps_bucket.cir : 0;
			uint64_t _eff_delay = batch_delay_us ? batch_delay_us : 1000;
			if (_rl_delay > 0 && _rl_delay < _eff_delay)
				_eff_delay = _rl_delay > 0 ? _rl_delay : 1;
			if (conf->traffic_model == 1)
				rte_delay_us(exp_random(_eff_delay));
			else if (conf->traffic_model == 2) {
				/* Pareto ON-OFF: idle delay = a few batch delays */
				rte_delay_us(pareto_random(_eff_delay, conf->pareto_alpha));
			} else
				rte_delay_us(_eff_delay);
			continue;
		}

		/* Pareto ON-OFF state machine (traffic_model == 2) */
		if (conf->traffic_model == 2) {
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
	if (template_mbuf)
		rte_pktmbuf_free(template_mbuf);
	rte_free(w->mbufs);
	w->mbufs = NULL;
	LOG_INFO("core %u exit", cv->core);
	return result;
}

/* Per-lcore worker thread entry point.
 *
 * Allocates worker-local state (mbuf pools, Cnode clone, cpuset),
 * waits on a pthread barrier for all workers to be ready,
 * then starts packet transmission.
 *
 * Called from rte_eal_remote_launch(). */
void worker_loop(void *data)
{
	uint32_t lcore_id = rte_lcore_id();
	struct injector_conf *bconf = (struct injector_conf*)data;
	struct worker *worker = rte_malloc(NULL, sizeof(struct worker), 0);
	if (!worker) {
		atomic_store(&worker_failed, 1);
		atomic_store(bconf->state, STATE_EXIT);
		LOG_ERR("[%s %d] rte_malloc(worker) -- lcore %u disabled",
			__func__, __LINE__, lcore_id);
		pthread_barrier_wait(bconf->barrier);
		while (1) {
			if (atomic_load_explicit(bconf->state, memory_order_acquire) == STATE_EXIT)
				return;
			rte_pause();
		}
	}
	// cppcheck-suppress nullPointerRedundantCheck
	memset(worker, 0, sizeof(struct worker));

	/* init rate limiter token buckets */
	token_bucket_init(&worker->pps_bucket,
		(uint64_t)bconf->pps_rate,
		(uint64_t)(bconf->pps_burst ? bconf->pps_burst
			   : (bconf->batch * 4)));
	token_bucket_init(&worker->bps_bucket,
		(uint64_t)bconf->bps_rate,
		(uint64_t)(bconf->bps_burst ? bconf->bps_burst : 1048576));


	/* set this worker(lcore or pthread) name */
	snprintf(worker->name, sizeof(worker->name), "%s@%u",
			"tx_only", lcore_id);
	int n = pthread_setname_np(pthread_self(), worker->name);
	if (n) {
		printf("pthread_setname_np(%s) %d %d %s", worker->name, n,
				errno, strerror(errno));
	}

	struct injector_conf *conf = &worker->conf;
	memcpy(conf, bconf, offsetof(struct injector_conf, dist_ratio));

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
		injector_show_dist(conf->dist);
	}

	Cnode *cnode = &worker->cnode;
	memset(cnode, 0, sizeof(struct Cnode));
	if (unlikely(config_clone_cnode(bconf->cnode, cnode) < 0)) {
		LOG_ERR("[%s %d] config_clone_cnode(%p) -- lcore %u disabled",
			__func__, __LINE__, cnode, lcore_id);
		goto INIT_FAIL;
	}
	/* propagate runtime flags not stored in YAML config */

	struct base_core_view *cv = &worker->cv;
	memcpy(cv, conf->base->topo.cv + lcore_id, sizeof(struct base_core_view));

	uint16_t port = cv->port;

	/* check if src mac address is provided */
	if (!cnode->ether.n_src) {
		rte_eth_macaddr_get(port, (struct rte_ether_addr*)cnode->ether.src);
		LOG_META_NNL("injector will use local port mac address: ");
		injector_print_mac((struct rte_ether_addr*)cnode->ether.src);
		cnode->ether.n_src = 1;
	}
	/* check if vxlan src mac address is provided */
	if (cnode->vxlan.enable && !cnode->vxlan.ether.n_src) {
		rte_eth_macaddr_get(port, (struct rte_ether_addr*)cnode->vxlan.ether.src);
		LOG_META_NNL("injector vxlan will use local port mac address: ");
		injector_print_mac((struct rte_ether_addr*)cnode->vxlan.ether.src);
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

	atomic_fetch_add(&active_workers, 1);
	pthread_barrier_wait(conf->barrier);

	worker_show(worker);

	if (worker_transmit(worker) < 0) {
		atomic_store(&worker_failed, 1);
		atomic_store(bconf->state, STATE_EXIT);
	}
	atomic_fetch_sub(&active_workers, 1);
	return;

INIT_FAIL:
	atomic_store(&worker_failed, 1);
	atomic_store(bconf->state, STATE_EXIT);
	pthread_barrier_wait(bconf->barrier);
	while (1) {
		if (atomic_load_explicit(bconf->state, memory_order_acquire) == STATE_EXIT)
			return;
		rte_pause();
	}
}

/* Wait for finite transmission completion or a termination signal. */
void worker_main_loop(void *data)
{
    struct injector_conf *conf = data;
    pthread_barrier_wait(conf->barrier);
    while (atomic_load_explicit(conf->state, memory_order_acquire) != STATE_EXIT) {
        if (atomic_load(&active_workers) == 0) {
            atomic_store(conf->state, STATE_EXIT);
            break;
        }
        rte_delay_ms(100);
    }
    if (atomic_load(&worker_failed)) conf->base->return_value = -1;
}
