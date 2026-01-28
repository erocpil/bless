#include "base.h"
#include "bless.h"
#include "define.h"
#include "worker.h"
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
	struct lcore_queue_conf *qconf;

	lcore_id = rte_lcore_id();

	char name[256] = { 0 };
	snprintf(name, sizeof(name), "worker@%u", lcore_id);
	pthread_setname_np(pthread_self(), name);

	struct bless_conf *bconf = (struct bless_conf*)data;
	qconf = bconf->qconf + lcore_id;
	LOG_DEBUG("lid %d pid %d qid %d\n", qconf->txl_id, qconf->txp_id, qconf->txq_id);
	getchar();
	assert(1 == qconf->enabled);

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
	conf->qconf = rte_malloc(NULL, sizeof(struct lcore_queue_conf), 0);
	if (unlikely(!conf->qconf)) {
		rte_exit(EXIT_FAILURE, "[%s %d] Cannot rte_malloc(qconf)\n",
				__func__, __LINE__);
	}
	memcpy(conf->qconf, &bconf->qconf[lcore_id], sizeof(struct lcore_queue_conf));

	Cnode *cnode = conf->cnode;
	if (cnode) {
		if (cnode->erroneous.n_mutation) {
			/* erroneous mutation */
			mutation_func *func = rte_malloc(NULL, sizeof(mutation_func) * cnode->erroneous.n_mutation, 0);
			for (int i = 0; i < cnode->erroneous.n_mutation; i++) {
				func[i] = bconf->cnode->erroneous.func[i];
			}
			cnode->erroneous.func = func;
			/* from config.yaml */
		}
	} else {
		/* TODO from command line params */
		rte_exit(EXIT_FAILURE, "[%s %d] no config.yaml\n", __func__, __LINE__);
	}

	qconf = conf->qconf; // bconf->qconf + lcore_id;
	atomic_int *state = conf->state;
	// uint32_t *l2fwd_dst_ports = conf->dst_ports;

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

	RTE_LOG(INFO, BLESS, "entering worker loop %s: lid %d pid %d tid %d self %lu lcore %d port %d txq %d\n",
			name, lcore_id, getpid(), rte_gettid(), (unsigned long)rte_thread_self().opaque_id,
			qconf->txl_id, qconf->txp_id, qconf->txq_id);

	// 获取当前线程 CPU 亲和性
	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	int s = pthread_getaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
	if (s != 0) {
		perror("pthread_getaffinity_np");
		pthread_exit(NULL);
	}
	printf("cpu set: ");
	for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
		if (CPU_ISSET(cpu, &cpuset)) {
			printf("%d ", cpu);
		}
	}
	printf("\n");

	uint8_t mode = conf->mode;
	int64_t num = conf->num < 0 ? INT64_MAX : conf->num;
	printf("conf.num %ld num %ld\n", conf->num, num);

	uint16_t batch = conf->batch;
	uint64_t batch_delay_us = bconf->batch_delay_us;
	if (batch > 2048) {
		batch = 2048;
		printf("batch -> 2048\n");
	}
	if (batch > num) {
		batch = num;
	}
	portid = qconf->txp_id;
	uint16_t qid = qconf->txq_id;
	uint16_t nb_tx = batch;
	printf("num %ld batch %u nb_tx %u\n", num, batch, nb_tx);

	/* XXX */
	if (mode == BLESS_MODE_RX_ONLY || mode == BLESS_MODE_FWD) {
		rx_mbufs = rte_malloc(NULL, batch * sizeof(struct rte_mbuf *), RTE_CACHE_LINE_SIZE);
		if (unlikely(!rx_mbufs)) {
			rte_exit(EXIT_FAILURE, "rx_mbufs alloc failed");
		}
	}

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

	mbufs = rte_malloc(NULL, batch * sizeof(struct rte_mbuf *), RTE_CACHE_LINE_SIZE);
	if (unlikely(!mbufs)) {
		rte_exit(EXIT_FAILURE, "mbufs alloc failed");
	}

	sprintf(name, "%s-c%d-p%d-q%d", "tx_pkts_pool", qconf->txl_id, qconf->txp_id, qconf->txq_id);
	struct rte_mempool *pktmbuf_pool = bless_create_pktmbuf_pool(conf->batch << 1, name);

	if (-1 == bless_alloc_mbufs(pktmbuf_pool, mbufs, conf->batch)) {
		printf("bless_alloc_mbufs() failed\n");
	}

	printf("cpu=%d lcore=%u lid=%d pid=%d qid=%d tid=%lu\n", sched_getcpu(), rte_lcore_id(),
			qconf->txp_id, qconf->txq_id, qconf->txl_id, pthread_self());
	assert(qconf->txl_id == rte_lcore_id());

	/* check if src mac address is provided */
	if (!cnode->ether.n_src) {
		/* FIXME race condition */
		rte_eth_macaddr_get(portid, (struct rte_ether_addr*)cnode->ether.src);
		printf("injector will use local port mac address:\n  ");
		bless_print_mac((struct rte_ether_addr*)cnode->ether.src);
		cnode->ether.n_src = 1;
	}
	/* check if vxlan src mac address is provided */
	if (cnode->vxlan.enable && !cnode->ether.n_src) {
		rte_eth_macaddr_get(portid, (struct rte_ether_addr*)cnode->vxlan.ether.src);
		printf("injector vxlan will use local port mac address:\n  ");
		bless_print_mac((struct rte_ether_addr*)cnode->vxlan.ether.src);
	}

	printf("[%s %d] >>> %d pthread_barrier_wait(%p);\n",
			__func__, __LINE__, lcore_id, bconf->barrier);
	pthread_barrier_wait(bconf->barrier);

	uint64_t val = atomic_load_explicit(state, memory_order_acquire);
	uint64_t i = 0;
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

		if (unlikely(num <= 0)) {
			sleep(1);
			atomic_store(state, STATE_EXIT);
			printf("Finished: lcore_id %d lid %d pid %d qid %d\n", rte_lcore_id(),
					qconf->txl_id, qconf->txp_id, qconf->txq_id);
			goto EXIT;
		}

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

		/* tx-only */
		for (int j = 0; j < nb_tx; j++) {
			// uint64_t tx_bytes = 0;
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
	printf("core %d exit\n", qconf->txl_id);
}

void dpdk_generate_cmdReply(const char *str)
{
	if (!str || !strlen(str)) {
		return;
	}
	char *reply = encode_cmdReply_to_json(str);
	printf("cmdReply %s\n", reply);
	ws_broadcast_log(reply, strlen(reply));
	free(reply);
}

void dpdk_generate_log(const char *str)
{
	if (!str || !strlen(str)) {
		return;
	}
	char *msg = encode_log_to_json(str);
	printf("msg %s\n", msg);
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
	uint64_t prev_tsc = 0, diff_tsc = 0, cur_tsc = 0, timer_tsc = 0;
	unsigned int lcore_id = rte_lcore_id();
	struct bless_conf *conf = (struct bless_conf*)data;
	atomic_int *state = conf->state;
	LOG_TRACE("state %p %p", state, conf->state);

	uint64_t timer_period = conf->timer_period;

	printf("lcore=%u cpu=%d\n", rte_lcore_id(), sched_getcpu());

	printf("[%s %d] pthread_barrier_wait(&conf->barrier);\n", __func__, __LINE__);
	pthread_barrier_wait(conf->barrier);
	printf("Bless pid %d is running ...\n", getpid());

	static uint64_t i = 0;
	uint64_t val = 0;
	while ((val = atomic_load_explicit(state, memory_order_acquire)) != STATE_EXIT) {
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
	struct config_file_map *cfm = base->cfm;
	cJSON *root = cJSON_Parse(data);
	if (!root) {
		printf("[%s %d] JSON parse error\n", __func__, __LINE__);
		return;
	}

	LOG_TRACE("state %p %p", state, base->g_state);

	const char *cmd = ws_json_get_string(root, "cmd");
	if (cmd) {
		printf("cmd = %s\n", cmd);
		if (strcmp(cmd, "start") == 0) {
			printf("Received START command\n");
			atomic_store(state, STATE_RUNNING);
			cmd = "started";
		} else if (strcmp(cmd, "stop") == 0) {
			printf("Received STOP command\n");
			atomic_store(state, STATE_STOPPED);
			cmd = "stopped";
		} else if (strcmp(cmd, "init") == 0) {
			printf("Received INIT command\n");
			// TODO reinit
			// atomic_store(&g_state, STATE_INIT);
			cmd = "already inited";
		} else if (strcmp(cmd, "exit") == 0) {
			printf("Received EXIT command\n");
			atomic_store(state, STATE_EXIT);
			cmd = "exited";
		} else if (strcmp(cmd, "conf") == 0) {
			printf("Received conf command\n");
			cmd = (char*)cfm->addr;
			printf("name %s\n", cfm->name);
		} else {
			cmd = "Not supported";
		}
	} else {
		printf("cmd missing or not a string\n");
		cmd = "cmd missing or not a string";
	}
	cJSON_Delete(root);
	dpdk_generate_cmdReply(cmd);
}

void worker_dump_lcore_queue_conf_single(struct lcore_queue_conf* lqc)
{
	LOG_TRACE("  lqc %p", lqc);
	LOG_HINT("    enabled: %d", lqc->enabled);
	LOG_HINT("    numa: %d", lqc->numa);
	LOG_HINT("    core: %d", lqc->txl_id);
	LOG_HINT("    port: %d", lqc->txp_id);
	LOG_HINT("    type: %d", lqc->type);
	LOG_HINT("    queue: %d", lqc->txq_id);
	LOG_HINT("    n_rx_port: %d", lqc->n_rx_port);
}

void worker_dump_lcore_queue_conf(struct lcore_queue_conf* lqc, int n)
{
	if (n <= 0) {
		return;
	}

	LOG_HINT("worker lqc %p %d", lqc, n);
	for (int i = 0; i < RTE_MAX_LCORE; i++) {
		struct lcore_queue_conf *q = lqc + i;
		if (!q->enabled) {
			continue;
		}
		worker_dump_lcore_queue_conf_single(q);
	}
}
