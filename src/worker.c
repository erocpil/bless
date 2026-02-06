#include "bless.h"
#include "cnode.h"
#include "define.h"
#include "worker.h"
#include "metric.h"
#include "log.h"
#include "server.h"
#include "log.h"

#include <stdatomic.h>
#include <stdint.h>
#include <sched.h>
#include <string.h>
#include <unistd.h>

/* DO NOT USE ANY RTE_DEFINE_PER_LCORE() !!! */

static inline void swap_mac(struct rte_ether_hdr *eth_hdr)
{
	struct rte_ether_addr addr;

	/* Swap dest and src mac addresses. */
	rte_ether_addr_copy(&eth_hdr->dst_addr, &addr);
	rte_ether_addr_copy(&eth_hdr->src_addr, &eth_hdr->dst_addr);
	rte_ether_addr_copy(&addr, &eth_hdr->src_addr);
}

static void worker_show_cpuset(cpu_set_t *cpuset)
{
	for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
		if (CPU_ISSET(cpu, cpuset)) {
			printf("%d ", cpu);
		}
	}
	printf("\n");
}

void worker_show(struct worker *w)
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

typedef enum {
	CHECK_CONTINUE = 0,
	CHECK_EXIT = 1,
} check_state_result_t;

static inline check_state_result_t worker_check_state(
		atomic_int *state, struct base_core_view *cv, int64_t num)
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


int worker_func_none(struct worker *w)
{
	LOG_WARN("core %u doing nothing", w->cv.core);

	atomic_int *state = w->conf.state;

	while (1) {
		if (worker_check_state(state, &w->cv, 1) == CHECK_EXIT) {
			goto EXIT;
		}
	}

	LOG_INFO("core %u exit normally", w->cv.core);
	return 0;

EXIT:
	LOG_INFO("core %u exit", w->cv.port);
	return 0;
}

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
	if (batch > 2048) {
		batch = 2048;
		LOG_INFO("batch -> 2048");
	}
	if (batch > num) {
		batch = num;
	}
	struct base_core_view *cv = &w->cv;
	uint16_t txq = cv->txq;
	uint16_t nb_tx = batch;


	w->mbufs = rte_malloc(NULL, batch * sizeof(struct rte_mbuf *), RTE_CACHE_LINE_SIZE);
	if (unlikely(!w->mbufs)) {
		rte_exit(EXIT_FAILURE, "mbufs alloc failed");
	}
	struct rte_mbuf **mbufs = w->mbufs;

	char *name = w->name;
	struct rte_mempool *pktmbuf_pool = bless_create_pktmbuf_pool(conf->batch << 1, name);

	if (-1 == bless_alloc_mbufs(pktmbuf_pool, mbufs, conf->batch)) {
		LOG_ERR("bless_alloc_mbufs() failed");
		rte_exit(EXIT_FAILURE, "bless_alloc_mbufs(%s)\n", name);
	}

	LOG_META("cpu %u lcore %u core %u port %u txq %u tid %lu",
			sched_getcpu(), rte_lcore_id(), cv->core,
			cv->port, cv->txq, pthread_self());
	assert(cv->core == rte_lcore_id());

	uint16_t port = cv->port;
	Cnode *cnode = &w->cnode;

	while (1) {
		if (worker_check_state(state, cv, num) == CHECK_EXIT) {
			goto EXIT;
		}

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
				int index = fast_rand_next() & conf->dist->mask;
				enum BLESS_TYPE type = conf->dist->data[index];
				int r = bless_mbufs(&mbufs[j], 1, type, (void*)cnode);
				if (!r) {
					rte_exit(EXIT_FAILURE, "Cannot bless_mbuf()\n");
				}
			}
		}

		uint16_t sent = rte_eth_tx_burst(port, txq, mbufs, nb_tx);
		if (sent) {
			num -= sent;
		}
		if (sent != nb_tx) {
			rte_pktmbuf_free_bulk(&mbufs[sent], nb_tx - sent);
		}

		if (unlikely(batch_delay_us)) {
			rte_delay_us(batch_delay_us);
		}
	}

	LOG_INFO("core %u exit normally", cv->core);
	return 0;

EXIT:
	LOG_INFO("core %u exit", cv->core);
	return 0;
}

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
		rte_exit(EXIT_FAILURE, "rx_mbufs alloc failed");
	}

	struct rte_mbuf **rx_mbufs = w->rx_mbufs;

	while (1) {
		if (worker_check_state(state, cv, num) == CHECK_EXIT) {
			goto EXIT;
		}
		const uint16_t nb_rx = rte_eth_rx_burst(port, rxq, rx_mbufs, batch);
		if (nb_rx) {
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
		rte_exit(EXIT_FAILURE, "rx_mbufs alloc failed");
	}
	struct rte_mbuf **rx_mbufs = w->rx_mbufs;

	while (1) {
		if (worker_check_state(state, cv, num) == CHECK_EXIT) {
			goto EXIT;
		}
		const uint16_t nb_rx = rte_eth_rx_burst(port, rxq, rx_mbufs, batch);
		if (nb_rx) {
			int n = nb_rx - 1;
			rte_prefetch0(rte_pktmbuf_mtod(rx_mbufs[0], struct rte_ether_hdr*));
			for (int i = 1, j = 0; i < n; i++, j++) {
				rte_prefetch0(rte_pktmbuf_mtod(rx_mbufs[i], struct rte_ether_hdr*));
				swap_mac(rte_pktmbuf_mtod(rx_mbufs[j], struct rte_ether_hdr*));
			}
			swap_mac(rte_pktmbuf_mtod(rx_mbufs[n], struct rte_ether_hdr*));
			uint16_t nb_tx = rte_eth_tx_burst(port, txq, rx_mbufs, nb_rx);
			if (nb_tx != nb_rx) {
				rte_pktmbuf_free_bulk(&rx_mbufs[nb_tx], nb_rx - nb_tx);
			}
			num -= nb_tx;
		} else {
			if (unlikely(batch_delay_us)) {
				rte_delay_us(batch_delay_us);
			}
		}
	}

	LOG_INFO("core %u exit normally", cv->core);
	return 0;

EXIT:
	LOG_INFO("core %u exit", cv->core);
	return 0;
}

int worker_func_flow(struct worker *w)
{
	LOG_INFO("core %u flow not implemented", w->cv.core);

	return 0;
}

int (*worker_func[])(struct worker*) = {
	worker_func_none,
	worker_func_tx_only,
	worker_func_rx_only,
	worker_func_fwd,
	worker_func_flow,
};


static char* BLESS_MODE_STR[] = {
	"none", "tx_only", "rx_only",
	"fwd", "flow", "max",
};

void worker_loop(void *data)
{
	uint32_t lcore_id = rte_lcore_id();

	struct worker *worker = rte_malloc(NULL, sizeof(struct worker), 0);
	if (!worker) {
		rte_exit(EXIT_FAILURE, "[%s %d] rte_malloc(worker)\n",
				__func__, __LINE__);
	}
	memset(worker, 0, sizeof(struct worker));

	struct bless_conf *bconf = (struct bless_conf*)data;
	uint8_t mode = bconf->mode;

	if (BLESS_MODE_NONE == mode || mode >= BLESS_MODE_MAX) {
		LOG_ERR("Unsupported bless mode %d", mode);
		return;
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

	conf->dist = rte_malloc(NULL, sizeof(struct distribution) +
			sizeof(uint8_t) * bconf->dist->size, 0);
	if (unlikely(!conf->dist)) {
		rte_exit(EXIT_FAILURE, "[%s %d] Cannot rte_malloc(distribution)\n",
				__func__, __LINE__);
	}
	memcpy(conf->dist, bconf->dist, sizeof(struct distribution) +
			sizeof(uint8_t) * bconf->dist->size);
	for (int i = 0; i < bconf->dist->size; i++) {
		if (bconf->dist->data[i] != conf->dist->data[i]) {
			LOG_ERR("distritution error: i %d %u %u",
					i, bconf->dist->data[i], conf->dist->data[i]);
			exit(0);
		}
	}
	struct distribution *dist = conf->dist;
	bless_show_dist(dist);

	Cnode *cnode = rte_malloc(NULL, sizeof(Cnode), 0);
	if (unlikely(!cnode)) {
		rte_exit(EXIT_FAILURE, "[%s %d] rte_malloc(Cnode)\n",
				__func__, __LINE__);
	}
	if (unlikely(config_clone_cnode(cnode, cnode) < 0)) {
		rte_exit(EXIT_FAILURE, "[%s %d] config_clone_cnode(%p)\n",
				__func__, __LINE__, cnode);
	}

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

	// cnode_show(conf->cnode, 0);
	// cnode_show_summary(conf->cnode);

	// 获取当前线程 CPU 亲和性
	cpu_set_t *cpuset = &worker->cpuset;
	CPU_ZERO(cpuset);
	int s = pthread_getaffinity_np(pthread_self(), sizeof(cpu_set_t), cpuset);
	if (s != 0) {
		perror("pthread_getaffinity_np");
		pthread_exit(NULL);
	}

	pthread_barrier_wait(conf->barrier);

	worker_show(worker);

	worker_func[mode](worker);
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
			timer_tsc = 0;
			if (unlikely(val == STATE_STOPPED)) {
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

	LOG_INFO("main core %u exit", cv->core);
}

const static char *ws_json_get_string(cJSON *obj, const char *key)
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
