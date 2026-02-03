#include "base.h"
#include "bless.h"
#include "cnode.h"
#include "define.h"
#include "worker.h"
#include "system.h"
#include "metric.h"
// #include "device.h"
#include "log.h"
#include "server.h"
#include "log.h"

#include <stdatomic.h>
#include <stdint.h>

static inline void swap_mac(struct rte_ether_hdr *eth_hdr)
{
	struct rte_ether_addr addr;

	/* Swap dest and src mac addresses. */
	rte_ether_addr_copy(&eth_hdr->dst_addr, &addr);
	rte_ether_addr_copy(&eth_hdr->src_addr, &eth_hdr->dst_addr);
	rte_ether_addr_copy(&addr, &eth_hdr->src_addr);
}

void worker_loop(void *data)
{
	struct rte_mbuf **mbufs = NULL;
	struct rte_mbuf **rx_mbufs = NULL;
	unsigned lcore_id;
	uint16_t portid = 0;

	lcore_id = rte_lcore_id();

	char name[256] = { 0 };
	snprintf(name, sizeof(name), "worker@%u", lcore_id);
	pthread_setname_np(pthread_self(), name);

	struct bless_conf *bconf = (struct bless_conf*)data;

	// LOG_INFO("wait barrier");
	// pthread_barrier_wait(bconf->barrier);

	/*
	qconf = bconf->qconf + lcore_id;
	LOG_DEBUG("lid %d pid %d qid %d\n", qconf->txl_id, qconf->txp_id, qconf->txq_id);
	*/
	// assert(1 == qconf->enabled);

	struct bless_conf *conf = rte_malloc(NULL, sizeof(struct bless_conf), 0);
	if (!conf) {
		rte_exit(EXIT_FAILURE, "[%s %d] Cannot rte_malloc(bless_conf)\n",
				__func__, __LINE__);
	}
	// DISTRIBUTION_DUMP(bconf->dist);

	uint16_t dsize = bconf->dist->size;
	dsize = bconf->dist->capacity;

	conf->dist = rte_malloc(NULL, sizeof(struct distribution) + sizeof(uint8_t) * dsize, 0);
	if (unlikely(!conf->dist)) {
		rte_exit(EXIT_FAILURE, "[%s %d] Cannot rte_malloc(distribution)\n",
				__func__, __LINE__);
	}

	memcpy(conf, bconf, offsetof(struct bless_conf, dist_ratio));
	struct distribution *dist = conf->dist;

	// XXX
#if 0
	conf->qconf = rte_malloc(NULL, sizeof(struct lcore_queue_conf), 0);
	if (unlikely(!conf->qconf)) {
		rte_exit(EXIT_FAILURE, "[%s %d] Cannot rte_malloc(qconf)\n",
				__func__, __LINE__);
	}
	memcpy(conf->qconf, &bconf->qconf[lcore_id], sizeof(struct lcore_queue_conf));
#endif

	struct base_core_view *cv = rte_malloc(NULL, sizeof(struct base_core_view), 0);
	if (unlikely(!cv)) {
		rte_exit(EXIT_FAILURE, "[%s %d] rte_malloc(base core view)\n",
				__func__, __LINE__);
	}
	memcpy(cv, conf->base->topo.cv + lcore_id, sizeof(struct base_core_view));

	Cnode *cnode = rte_malloc(NULL, sizeof(Cnode), 0);
	if (unlikely(!cnode)) {
		rte_exit(EXIT_FAILURE, "[%s %d] rte_malloc(Cnode)\n",
				__func__, __LINE__);
	}
	if (unlikely(config_clone_cnode(conf->cnode, cnode) < 0)) {
		rte_exit(EXIT_FAILURE, "[%s %d] config_clone_cnode(%p)\n",
				__func__, __LINE__, conf->cnode);
	}

	/* check if src mac address is provided */
	if (!cnode->ether.n_src) {
		rte_eth_macaddr_get(portid, (struct rte_ether_addr*)cnode->ether.src);
		LOG_META_NNL("injector will use local port mac address: ");
		bless_print_mac((struct rte_ether_addr*)cnode->ether.src);
		cnode->ether.n_src = 1;
	}
	/* check if vxlan src mac address is provided */
	if (cnode->vxlan.enable && !cnode->vxlan.ether.n_src) {
		rte_eth_macaddr_get(portid, (struct rte_ether_addr*)cnode->vxlan.ether.src);
		LOG_META_NNL("injector vxlan will use local port mac address: ");
		bless_print_mac((struct rte_ether_addr*)cnode->vxlan.ether.src);
		cnode->vxlan.ether.n_src = 1;
	}

	conf->cnode = cnode;
	// cnode_show(conf->cnode, 0);
	cnode_show_summary(conf->cnode);

#if 0
	/* FIXME */
	/* only txq 0 malloc stats */
	if (!qconf->txq_id) {
		uint16_t pid = conf->qconf->txp_id;
		conf->stats[pid] = rte_malloc(NULL, sizeof(struct port_statistics) * TYPE_MAX, 0);
		if (!conf->stats[pid]) {
			rte_exit(EXIT_FAILURE,
					"[%s %d] Cannot rte_malloc(port_statistics)\n",
					__func__, __LINE__);
		}
		printf("%d %p\n", lcore_id, conf->stats[pid]);
	}
#endif
	/*
	   for (int i = 0; i < qconf->n_rx_port; i++) {
	   portid = l2fwd_dst_ports[qconf->rx_port_list[i]];
	   conf->stats[portid] = rte_malloc(NULL, sizeof(struct port_statistics) * TYPE_MAX, 0);
	   if (!conf->stats[portid]) {
	   rte_exit(EXIT_FAILURE, "[%s %d] Cannot rte_malloc(port_statistics)\n", __func__, __LINE__);
	   }
	   printf("%d %p\n", lcore_id, conf->stats[portid]);
	   }
	   */

	/*
	   if (qconf->n_rx_port == 0) {
	   RTE_LOG(INFO, BLESS, "lcore %u has nothing to do\n", lcore_id);
	   return;
	   }
	   */

	// XXX
#if 0
	RTE_LOG(INFO, BLESS, "entering worker loop %s: lid %d pid %d tid %d self %lu lcore %d port %d txq %d\n",
			name, lcore_id, getpid(), rte_gettid(), (unsigned long)rte_thread_self().opaque_id,
			qconf->txl_id, qconf->txp_id, qconf->txq_id);
#endif

	// 获取当前线程 CPU 亲和性
	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	int s = pthread_getaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
	if (s != 0) {
		perror("pthread_getaffinity_np");
		pthread_exit(NULL);
	}

	/*
	printf("cpu set: ");
	for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
		if (CPU_ISSET(cpu, &cpuset)) {
			printf("%d ", cpu);
		}
	}
	printf("\n");
	*/

	uint8_t mode = conf->mode;
	int64_t num = conf->num < 0 ? INT64_MAX : conf->num;
	atomic_int *state = conf->state;

	uint16_t batch = conf->batch;
	uint64_t batch_delay_us = bconf->batch_delay_us;
	if (batch > 2048) {
		batch = 2048;
		LOG_INFO("batch -> 2048");
	}
	if (batch > num) {
		batch = num;
	}
	portid = cv->port;
	uint16_t qid = cv->rxq;
	uint16_t nb_tx = batch;

	/* XXX */
	if (mode == BLESS_MODE_RX_ONLY || mode == BLESS_MODE_FWD) {
		rx_mbufs = rte_malloc(NULL, batch * sizeof(struct rte_mbuf *), RTE_CACHE_LINE_SIZE);
		if (unlikely(!rx_mbufs)) {
			rte_exit(EXIT_FAILURE, "rx_mbufs alloc failed");
		}
	}

#if 0
	/* rx-only */
	if (BLESS_MODE_RX_ONLY == mode) {
		pthread_barrier_wait(bconf->barrier);
		printf("rx only\n");
		uint64_t i = 0;
		uint64_t val = atomic_load_explicit(state, memory_order_acquire);
		while (val != STATE_EXIT) {
			if (unlikely(val != STATE_RUNNING)) {
				i = 0;
				while (unlikely(val == STATE_STOPPED)) {
					if (!i++) {
						printf("Detect STOPPED %d\n", qconf->txl_id);
					}
					rte_delay_ms(1000);
					val = atomic_load_explicit(state, memory_order_acquire);
				}
				while (unlikely(val == STATE_INIT)) {
					if (!i++) {
						printf("Detect INIT %d\n", qconf->txl_id);
					}
					rte_delay_ms(1000);
					val = atomic_load_explicit(state, memory_order_acquire);
				}
				if (val == STATE_EXIT) {
					printf("Detect EXIT %d\n", qconf->txl_id);
					goto EXIT;
				}
				if (val == STATE_RUNNING) {
					printf("Detect START %d %d %d\n", qconf->txl_id, qconf->txp_id, qconf->txq_id);
				}
			}
			while (val != STATE_EXIT) {
				const uint16_t nb_rx = rte_eth_rx_burst(portid, qid, rx_mbufs, batch);
				if (nb_rx) {
					rte_pktmbuf_free_bulk(rx_mbufs, nb_rx);
				}
				val = atomic_load_explicit(state, memory_order_acquire);
			}
		}
		printf("worker exit\n");
		return;
	}
#endif

	if (BLESS_MODE_TX_ONLY == mode) {
		mbufs = rte_malloc(NULL, batch * sizeof(struct rte_mbuf *), RTE_CACHE_LINE_SIZE);
		if (unlikely(!mbufs)) {
			rte_exit(EXIT_FAILURE, "mbufs alloc failed");
		}

		sprintf(name, "%s-c%d-p%d-q%d", "tx_pkts_pool", cv->core, cv->port, cv->rxq);
		struct rte_mempool *pktmbuf_pool = bless_create_pktmbuf_pool(conf->batch << 1, name);

		if (-1 == bless_alloc_mbufs(pktmbuf_pool, mbufs, conf->batch)) {
			LOG_ERR("bless_alloc_mbufs() failed");
			rte_exit(EXIT_FAILURE, "bless_alloc_mbufs(%s)\n", name);
		}

		LOG_META("cpu %u lcore %u core %u port %u txq %u tid %lu",
				sched_getcpu(), rte_lcore_id(), cv->core,
				cv->port, cv->txq, pthread_self());
		assert(cv->core == rte_lcore_id());
	}

	LOG_INFO("core %d pthread_barrier_wait(%p);", lcore_id, bconf->barrier);
	pthread_barrier_wait(bconf->barrier);

	uint64_t val = atomic_load_explicit(state, memory_order_acquire);
	uint64_t i = 0;
	while (val != STATE_EXIT) {
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
				goto EXIT;
			}
			if (val == STATE_RUNNING) {
				LOG_INFO("Detect START @ core %u %u %u", cv->core, cv->port, cv->rxq);
			}
		}

		if (unlikely(num <= 0)) {
			sleep(1);
			atomic_store(state, STATE_EXIT);
			LOG_INFO("Finished: lcore_id %u @ core %u port %u rxq %u", rte_lcore_id(),
					cv->core, cv->port, cv->rxq);
			goto EXIT;
		}

#if 1
		/* fwd */
		if (mode == BLESS_MODE_FWD) {
			const uint16_t nb_rx = rte_eth_rx_burst(portid, qid, rx_mbufs, batch);
			if (nb_rx) {
				int n = nb_rx - 1;
				rte_prefetch0(rte_pktmbuf_mtod(rx_mbufs[0], struct rte_ether_hdr*));
				for (int i = 1, j = 0; i < n; i++, j++) {
					rte_prefetch0(rte_pktmbuf_mtod(rx_mbufs[i], struct rte_ether_hdr*));
					swap_mac(rte_pktmbuf_mtod(rx_mbufs[j], struct rte_ether_hdr*));
				}
				swap_mac(rte_pktmbuf_mtod(rx_mbufs[n], struct rte_ether_hdr*));
				uint16_t nb_tx = rte_eth_tx_burst(portid, qid, rx_mbufs, nb_rx);
				if (nb_tx != nb_rx) {
					rte_pktmbuf_free_bulk(&mbufs[nb_tx], nb_rx - nb_tx);
				}
			} else {
				if (unlikely(batch_delay_us)) {
					rte_delay_us(batch_delay_us);
				}
			}
			val = atomic_load_explicit(state, memory_order_acquire);
			continue;
		}
#endif

		/* tx-only */
		for (int j = 0; j < nb_tx; j++) {
			if (cnode->erroneous.ratio > 0 && cnode->erroneous.n_mutation) {
				/* should this mbuf be a mutation? */
				uint64_t tsc = fast_rand_next();
				tsc = tsc ^ (tsc >> 8);
				if ((tsc & 1023) < cnode->erroneous.ratio) {
					int n = tsc % cnode->erroneous.n_mutation;
					mutation_func func = cnode->erroneous.func[n];
					int r = func((void**)&mbufs[j], 1, (void*)cnode);
					if (!r) {
						rte_exit(EXIT_FAILURE, "Cannot mutate(%d)\n", n);
					}
					// tx_bytes += r;
				}
			} else {
				enum BLESS_TYPE type = dist->data[fast_rand_next() & dist->mask];
				int r = bless_mbufs(&mbufs[j], 1, type, (void*)cnode);
				if (!r) {
					rte_exit(EXIT_FAILURE, "Cannot bless_mbuf()\n");
				}
				// tx_bytes += r;
			}
			// rte_atomic64_inc(&(conf->stats[portid] + type)->tx_pkts);
			// rte_atomic64_add(&(conf->stats[portid] + type)->tx_bytes, tx_bytes);
		}

		/* FIXME stats of type */
		// rte_pktmbuf_dump(stdout, mbufs[0], 2000);
		uint16_t sent = rte_eth_tx_burst(portid, qid, mbufs, nb_tx);
		if (sent) {
			num -= sent;
			// printf("lcore %u port %u sent %d remain %ld\n", lcore_id, portid, sent, num);
		}
		if (sent != nb_tx) {
			// printf("lcore %u port %u unsent %d remain %lu\n", lcore_id, portid, nb_tx - sent, num);
			/*
			   uint64_t dropped_bytes = 0;
			   for (int i = sent; i < nb_tx; i++) {
			   dropped_bytes += mbufs[i]->pkt_len;
			   }
			   rte_atomic64_sub(&(conf->stats[portid] + type)->dropped_pkts, nb_tx - sent);
			   rte_atomic64_add(&(conf->stats[portid] + type)->dropped_bytes, dropped_bytes);
			// (conf->stats[portid] + type)->dropped_pkts += nb_tx - sent;
			// (conf->stats[portid] + type)->dropped_bytes += dropped_bytes;
			*/
			rte_pktmbuf_free_bulk(&mbufs[sent], nb_tx - sent);
		}

		if (unlikely(batch_delay_us)) {
			rte_delay_us(batch_delay_us);
		}

		val = atomic_load_explicit(state, memory_order_acquire);
	}

EXIT:
	LOG_INFO("core %u %u exit", lcore_id, cv->core);
}

void dpdk_generate_cmdReply(const char *str)
{
	if (!str || !strlen(str)) {
		return;
	}
	char *reply = encode_cmdReply_to_json(str);
	LOG_TRACE("cmdReply %s", reply);
	ws_broadcast_log(reply, strlen(reply));
	free(reply);
}

void dpdk_generate_log(const char *str)
{
	if (!str || !strlen(str)) {
		return;
	}
	char *msg = encode_log_to_json(str);
	LOG_TRACE("msg %s", msg);
	ws_broadcast_log(msg, strlen(msg));
	free(msg);
}

void dpdk_generate_stats(uint32_t enabled_port_mask)
{
	int active = stats_get_active_index();
	int inactive = active ^ 1;

	struct stats_snapshot *s = stats_get(inactive);

	char tmp[128];
	sprintf(tmp, "log: %lu", rte_rdtsc());
	char *msg = encode_stats_to_json(enabled_port_mask, tmp);
	/* JSON */
	s->json_len = snprintf(s->json, STATS_JSON_MAX, "%s\n", msg);
	free(msg);

	/* Prometheus */
	s->metric_len = encode_stats_to_text(enabled_port_mask, s->metric, STATS_METRIC_MAX);
	s->ts_ns = rte_get_tsc_cycles();

	stats_set(inactive);
	ws_broadcast_stats();
}

void worker_main_loop(void *data)
{
	unsigned int lcore_id = rte_lcore_id();
	struct bless_conf *conf = (struct bless_conf*)data;
	struct base_core_view *cv = conf->base->topo.cv + lcore_id;
	atomic_int *state = conf->state;
	uint64_t timer_period = conf->timer_period;
	uint64_t prev_tsc = 0, diff_tsc = 0, cur_tsc = 0, timer_tsc = 0;
	uint64_t val = 0;

	pthread_barrier_wait(conf->barrier);

	while ((val = atomic_load_explicit(state, memory_order_acquire)) != STATE_EXIT) {
		static uint64_t i = 0;
		rte_delay_ms(100);
		cur_tsc = rte_rdtsc();
		diff_tsc = cur_tsc - prev_tsc;
		timer_tsc += diff_tsc;
		prev_tsc = cur_tsc;
		/* if timer has reached its timeout */
		if (unlikely(timer_tsc >= timer_period * 1)) {
			/* do this only on main core */
			if (lcore_id == rte_get_main_lcore()) {
				timer_tsc = 0;
				if (val == STATE_STOPPED) {
					if (i) {
						dpdk_generate_stats(conf->enabled_port_mask);
					}
					i = 0;
				} else {
					dpdk_generate_stats(conf->enabled_port_mask);
					i++;
				}
			}
		}
	}

	LOG_INFO("main core %u %u exit", lcore_id, cv->core);
}

const char *ws_json_get_string(cJSON *obj, const char *key)
{
	cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
	return cJSON_IsString(item) ? item->valuestring : NULL;
}

void ws_user_func(void *user, void *data, size_t size)
{
	if (!data || !size) {
		return;
	}

	struct ws_user_data *wsud = (struct ws_user_data*)user;
	struct base *base = (struct base*)wsud->data;
	atomic_int *state = base->g_state;
	struct config_file_map *cfm = &base->config->cfm;
	cJSON *root = cJSON_Parse(data);
	if (!root) {
		LOG_WARN("[%s %d] JSON parse error\n", __func__, __LINE__);
		return;
	}

	LOG_TRACE("state %p %p", state, base->g_state);

	const char *cmd = ws_json_get_string(root, "cmd");
	if (cmd) {
		LOG_TRACE("cmd = %s\n", cmd);
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
			// TODO reinit
			// atomic_store(&g_state, STATE_INIT);
			cmd = "already inited";
		} else if (strcmp(cmd, "exit") == 0) {
			LOG_TRACE("Received EXIT command");
			atomic_store(state, STATE_EXIT);
			cmd = "exited";
		} else if (strcmp(cmd, "conf") == 0) {
			LOG_TRACE("Received conf command");
			cmd = (char*)cfm->addr;
			printf("name %s\n", cfm->name);
		} else {
			cmd = "Not supported";
		}
	} else {
		LOG_WARN("cmd missing or not a string");
		cmd = "cmd missing or not a string";
	}

	cJSON_Delete(root);
	dpdk_generate_cmdReply(cmd);
}
