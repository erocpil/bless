#include "worker.h"

static uint32_t l2fwd_enabled_port_mask = 0;
// RTE_DEFINE_PER_LCORE(volatile struct port_statistics, port_statistics);

void worker_loop_txonly(void *data)
{
	// struct rte_mbuf *pkts_burst[MAX_PKT_BURST];
	struct rte_mbuf *mbufs[1024];
	unsigned lcore_id;
	// unsigned i;
	uint16_t portid = 0;
	struct lcore_queue_conf *qconf;
	// struct rte_eth_dev_tx_buffer *buffer;

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
	if (!conf->dist) {
		rte_exit(EXIT_FAILURE, "[%s %d] Cannot rte_malloc(distribution)\n",
				__func__, __LINE__);
	}

	memcpy(conf, bconf, offsetof(struct bless_conf, dist_ratio));
	struct distribution *dist = conf->dist;
	conf->qconf = rte_malloc(NULL, sizeof(struct lcore_queue_conf), 0);
	if (!conf->qconf) {
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

	l2fwd_enabled_port_mask = conf->enabled_port_mask;
	qconf = conf->qconf; // bconf->qconf + lcore_id;
	volatile bool *force_quit = conf->force_quit;
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

	RTE_LOG(INFO, BLESS, "entering worker loop %s: pid %d tid %d self %lu lcore %d port %d txq %d\n", name,
			getpid(), rte_gettid(), (unsigned long)rte_thread_self().opaque_id,
			qconf->txl_id, qconf->txp_id, qconf->txq_id);

	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);

	// 获取当前线程 CPU 亲和性
	int s = pthread_getaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
	if (s != 0) {
		perror("pthread_getaffinity_np");
		pthread_exit(NULL);
	}

	/*
	   printf("Thread running on CPUs: ");
	   for (int i = 0; i < CPU_SETSIZE; i++) {
	   if (CPU_ISSET(i, &cpuset)) {
	   printf("%d ", i);
	   }
	   }
	   printf("\n");
	   */

	/* XXX
	   for (i = 0; i < qconf->n_rx_port; i++) {
	   portid = qconf->rx_port_list[i];
	   RTE_LOG(INFO, BLESS, " -- lcoreid=%u portid=%u\n", lcore_id, portid);
	   }
	   */

	printf(" >>> %d pthread_barrier_wait(%p);\n", lcore_id, bconf->barrier);
	pthread_barrier_wait(bconf->barrier);

	sprintf(name, "%s-c%d-p%d-q%d", "tx_pkts_pool",
			qconf->txl_id, qconf->txp_id, qconf->txq_id);
	struct rte_mempool *pktmbuf_pool = bless_create_pktmbuf_pool(16384, name);

	bless_alloc_mbufs(pktmbuf_pool, mbufs, conf->batch + 1);

	// struct rte_ring *ring = (struct rte_ring*)(pktmbuf_pool->pool_data);
	// printf("size %u mask %u capacity %u\n", ring->size, ring->mask, ring->capacity);

	uint64_t size = conf->size;
	// printf("size %lu\n", size);
	uint16_t batch = bconf->batch;
	if (batch > 1024) {
		batch = 1024;
	}
	if (batch > size) {
		batch = size;
	}
	uint16_t nb_tx = batch;
	// printf("[%s %d] lcore %d send %lu batch %u\n", __func__, __LINE__, lcore_id, size, batch);

	portid = qconf->txp_id;
	uint16_t qid = qconf->txq_id;
	printf("lcore_id %d lid %d pid %d qid %d\n", rte_lcore_id(),
			qconf->txl_id, qconf->txp_id, qconf->txq_id);
	assert(qconf->txl_id == rte_lcore_id());

	// return;

	while (!*force_quit && size > 0) {
		/*
		 * TX burst queue
		 */
		// for (i = 0; i < qconf->n_rx_port; i++) {
		// portid = l2fwd_dst_ports[qconf->rx_port_list[i]];
		// rte_eth_macaddrs_get(portid, &bep.inner->src_addr, 1);
		// TODO
		// rte_eth_macaddr_get();
		// sent = rte_eth_tx_buffer_flush(portid, 0, buffer);
		uint64_t tx_bytes = 0;
		int type = -1;

		// srandom(rte_rdtsc());
		for (int j = 0; j < nb_tx; j++) {
			/* should this mbuf be a mutation? */
			uint64_t tsc = rte_rdtsc();
			tsc = tsc ^ (tsc >> 8);
			enum BLESS_TYPE type = dist->data[rte_rdtsc() & dist->mask];
			// if (1) {
			if ((tsc & 1023) < cnode->erroneous.ratio) {
				int n = tsc % cnode->erroneous.n_mutation;
				// n = 24;
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
			// (conf->stats[portid] + type)->tx_pkts++;
			// (conf->stats[portid] + type)->tx_bytes += tx_bytes;
		}

		/* FIXME stats of type */
		// rte_pktmbuf_dump(stdout, mbufs[0], 1000);
		uint16_t sent = rte_eth_tx_burst(portid, qid, mbufs, nb_tx);
		if (sent) {
			size -= sent;
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
		// rte_delay_ms(1);
		// }
	}
}
