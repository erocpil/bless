#include "worker.h"

void worker_loop_txonly(void *data)
{
	struct rte_mbuf **mbufs = NULL;
	unsigned lcore_id;
	uint16_t portid = 0;
	struct lcore_queue_conf *qconf;

	lcore_id = rte_lcore_id();

	char name[256] = { 0 };
	snprintf(name, sizeof(name), "worker@%u", lcore_id);
	pthread_setname_np(pthread_self(), name);

	struct bless_conf *bconf = (struct bless_conf*)data;
	qconf = bconf->qconf + lcore_id;
	printf("lid %d pid %d qid %d\n", qconf->txl_id, qconf->txp_id, qconf->txq_id);
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

	/* FIXME */
	/* only txq 0 malloc stats */
	if (!qconf->txq_id) {
		uint16_t pid = conf->qconf->txp_id;
		conf->stats[pid] = rte_malloc(NULL, sizeof(struct port_statistics) * TYPE_MAX, 0);
		if (!conf->stats[pid]) {
			rte_exit(EXIT_FAILURE, "[%s %d] Cannot rte_malloc(port_statistics)\n", __func__, __LINE__);
		}
		printf("%d %p\n", lcore_id, conf->stats[pid]);
	}
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
	{
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
	}

	sprintf(name, "%s-c%d-p%d-q%d", "tx_pkts_pool",
			qconf->txl_id, qconf->txp_id, qconf->txq_id);

	mbufs = rte_malloc(NULL, conf->batch * sizeof(struct rte_mbuf *), RTE_CACHE_LINE_SIZE);
	if (unlikely(!mbufs)) {
		rte_exit(EXIT_FAILURE, "mbufs alloc failed");
	}

	struct rte_mempool *pktmbuf_pool = bless_create_pktmbuf_pool(conf->batch << 1, name);

	if (-1 == bless_alloc_mbufs(pktmbuf_pool, mbufs, conf->batch)) {
		printf("bless_alloc_mbufs() failed\n");
	}

	printf("cpu=%d lcore=%u lid=%d pid=%d qid=%d tid=%lu\n", sched_getcpu(), rte_lcore_id(),
			qconf->txp_id, qconf->txq_id, qconf->txl_id, pthread_self());
	assert(qconf->txl_id == rte_lcore_id());

	uint64_t num = conf->num;
	uint16_t batch = bconf->batch;
	if (batch > 2048) {
		batch = 2048;
		printf("batch -> 2048\n");
	}
	if (batch > num) {
		batch = num;
	}
	uint16_t nb_tx = batch;
	printf("num %lu batch %u\n", num, batch);

	portid = qconf->txp_id;
	uint16_t qid = qconf->txq_id;

	/* check if src mac address is provided */
	if (!cnode->ether.n_src) {
		rte_eth_macaddr_get(portid, (struct rte_ether_addr*)cnode->ether.src);
		printf("injector will use local port mac address:\n");
		bless_print_mac((struct rte_ether_addr*)cnode->ether.src);
	}
	/* check if vxlan src mac address is provided */
	if (cnode->vxlan.enable && !cnode->ether.n_src) {
		rte_eth_macaddr_get(portid, (struct rte_ether_addr*)cnode->vxlan.ether.src);
		printf("injector vxlan will use local port mac address:\n");
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
			printf("Finished: lcore_id %d lid %d pid %d qid %d\n", rte_lcore_id(),
					qconf->txl_id, qconf->txp_id, qconf->txq_id);
			break;
		}

		int type = -1;

		for (int j = 0; j < nb_tx; j++) {
			uint64_t tx_bytes = 0;
			/* should this mbuf be a mutation? */
			uint64_t tsc = rte_rdtsc();
			tsc = tsc ^ (tsc >> 8);
			enum BLESS_TYPE type = dist->data[rte_rdtsc() & dist->mask];
			if (cnode->erroneous.ratio > 0 && cnode->erroneous.n_mutation &&
					(tsc & 1023) < cnode->erroneous.ratio) {
				int n = tsc % cnode->erroneous.n_mutation;
				mutation_func func = cnode->erroneous.func[n];
				int r = func((void**)&mbufs[j], 1, (void*)cnode);
				if (!r) {
					rte_exit(EXIT_FAILURE, "Cannot mutate(%d)\n", n);
				}
				tx_bytes += r;
			} else {
				enum BLESS_TYPE type = dist->data[rte_rdtsc() & dist->mask];
				int r = bless_mbufs(&mbufs[j], 1, type, (void*)cnode);
				if (!r) {
					rte_exit(EXIT_FAILURE, "Cannot bless_mbuf()\n");
				}
				tx_bytes += r;
			}
			rte_atomic64_inc(&(conf->stats[portid] + type)->tx_pkts);
			rte_atomic64_add(&(conf->stats[portid] + type)->tx_bytes, tx_bytes);
		}

		/* FIXME stats of type */
		// rte_pktmbuf_dump(stdout, mbufs[0], 1000);
		uint16_t sent = rte_eth_tx_burst(portid, qid, mbufs, nb_tx);
		if (sent) {
			num -= sent;
			// printf("lcore %u port %u sent %d remain %lu\n", lcore_id, portid, sent, size);
		}
		if (sent != nb_tx) {
			uint64_t dropped_bytes = 0;
			for (int i = sent; i < nb_tx; i++) {
				dropped_bytes += mbufs[i]->pkt_len;
			}
			// printf("lcore %u port %u unsent %d remain %lu\n", lcore_id, portid, nb_tx - sent, size);
			rte_atomic64_sub(&(conf->stats[portid] + type)->dropped_pkts, nb_tx - sent);
			rte_atomic64_add(&(conf->stats[portid] + type)->dropped_bytes, dropped_bytes);
			// (conf->stats[portid] + type)->dropped_pkts += nb_tx - sent;
			// (conf->stats[portid] + type)->dropped_bytes += dropped_bytes;
			rte_pktmbuf_free_bulk(&mbufs[sent], nb_tx - sent);
		}

		val = atomic_load_explicit(state, memory_order_acquire);
	}

EXIT:
	printf("core %d exit\n", qconf->txl_id);
}
