#ifndef __METRICS_H__
#define __METRICS_H__

#define _GNU_SOURCE
#include <pthread.h>
#include <unistd.h>

#include <rte_lcore.h>
#include <rte_telemetry.h>
#include <rte_debug.h>

#include "bless.h"
#include "define.h"

extern struct bless_conf *bconf;

static int bless_handle_injector(const char *cmd __rte_unused, const char *params,
		struct rte_tel_data *d, enum BLESS_TYPE type)
{
}

static int bless_handle_metrics(const char *cmd __rte_unused, const char *params,
		struct rte_tel_data *d, enum BLESS_TYPE type)
{
	// unsigned int lcore_id = rte_lcore_id();
	// unsigned int main_id = rte_get_main_lcore();
	uint32_t bless_enabled_port_mask = bconf->enabled_port_mask;
	struct port_statistics **port_statistics = bconf->stats;

	uint64_t total_packets_tx_pkts = 0;
	uint64_t total_packets_tx_bytes = 0;
	// uint64_t total_packets_rx = 0;
	uint64_t total_packets_dropped_pkts = 0;
	uint64_t total_packets_dropped_bytes = 0;

	RTE_SET_USED(cmd);
	RTE_SET_USED(params);
	if (type < 0 || type >= TYPE_MAX) {
		return -1;
	}

	rte_tel_data_start_dict(d);

	for (unsigned int portid = 0; portid < RTE_MAX_ETHPORTS; portid++) {
		/* skip disabled ports */
		if ((bless_enabled_port_mask & (1 << portid)) == 0) {
			// printf("\nskipped %d\n", portid);
			continue;
		}
		if (!port_statistics[portid]) {
			continue;
		}

		/* FIXME atomic
		total_packets_tx_pkts += (port_statistics[portid] + type)->tx_pkts;
		total_packets_tx_bytes += (port_statistics[portid] + type)->tx_bytes;
		// total_packets_rx += (port_statistics[portid] + type)->rx;
		total_packets_dropped_pkts += (port_statistics[portid] + type)->dropped_pkts;
		total_packets_dropped_bytes += (port_statistics[portid] + type)->dropped_bytes;
		*/
	}
	rte_tel_data_add_dict_int(d, "tx_pkts", total_packets_tx_pkts);
	rte_tel_data_add_dict_int(d, "tx_bytes", total_packets_tx_bytes);
	rte_tel_data_add_dict_int(d, "dropped_pkts", total_packets_dropped_pkts);
	rte_tel_data_add_dict_int(d, "dropped_bytes", total_packets_dropped_bytes);

	return 0;
}

/* FIXME */
static int bless_handle_all(const char *cmd __rte_unused, const char *params, struct rte_tel_data *d)
{
	for (int i = 0; i < TYPE_MAX; i++) {
		bless_handle_metrics(cmd, params, d, i);
	}
	return 0;
}

static int bless_handle_arp(const char *cmd __rte_unused, const char *params, struct rte_tel_data *d)
{
	return bless_handle_metrics(cmd, params, d, TYPE_ARP);
}

static int bless_handle_udp(const char *cmd __rte_unused, const char *params, struct rte_tel_data *d)
{
	return bless_handle_metrics(cmd, params, d, TYPE_UDP);
}

static int bless_handle_tcp(const char *cmd __rte_unused, const char *params, struct rte_tel_data *d)
{
	return bless_handle_metrics(cmd, params, d, TYPE_UDP);
	return 0;
}

static int bless_handle_icmp(const char *cmd __rte_unused, const char *params, struct rte_tel_data *d)
{
	return bless_handle_metrics(cmd, params, d, TYPE_UDP);
	return 0;
}

static int bless_handle_vxlan(const char *cmd __rte_unused, const char *params, struct rte_tel_data *d)
{
	return bless_handle_metrics(cmd, params, d, TYPE_UDP);
	return 0;
}

static int bless_handle_erroneous(const char *cmd __rte_unused, const char *params, struct rte_tel_data *d)
{
	return bless_handle_metrics(cmd, params, d, TYPE_UDP);
	return 0;
}

static int my_custom_metrics_cb(const char *cmd, const char *params, struct rte_tel_data *d)
{
	unsigned int lcore_id = rte_lcore_id();
	unsigned int main_id = rte_get_main_lcore();
	if (lcore_id == main_id) {
		RTE_LOG(INFO, BLESS, "%s @ main lcore %u\n", __func__, lcore_id);
	}

	RTE_LOG(INFO, BLESS, "pid: %d, tid:%d, self: %lu\n",
			getpid(), rte_gettid(), (unsigned long)rte_thread_self().opaque_id);

	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);

	// 获取当前线程 CPU 亲和性
	int s = pthread_getaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
	if (s != 0) {
		perror("pthread_getaffinity_np");
		pthread_exit(NULL);
	}

	printf("Thread running on CPUs: ");
	for (int i = 0; i < CPU_SETSIZE; i++) {
		if (CPU_ISSET(i, &cpuset)) {
			printf("%d ", i);
		}
	}
	printf("\n");

	RTE_SET_USED(cmd);
	RTE_SET_USED(params);

	rte_tel_data_start_dict(d);
	rte_tel_data_add_dict_int(d, "my_metric1", 123);
	rte_tel_data_add_dict_int(d, "my_metric2", 456);

	return 0;
}

#endif
