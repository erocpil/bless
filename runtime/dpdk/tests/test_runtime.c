/* SPDX-License-Identifier: BSD-3-Clause */
#include "bless_dpdk_runtime.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <rte_eal.h>
#include <rte_lcore.h>

#define CHECK(cond, message) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL: %s\n", message); \
		goto fail; \
	} \
} while (0)

static int worker_pool_create(void *unused)
{
	(void)unused;
	return br_runtime_pktmbuf_pool_create("runtime_worker_pool", 512, 0, 0,
			RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id()) ? 0 : -1;
}

int main(void)
{
	char *eal_argv[] = {
		"test-runtime", "--no-pci", "--no-huge", "--no-shconf",
		"--no-telemetry", "--vdev=net_ring0", "--main-lcore=0",
		"-l", "0-1",
	};
	struct rte_eth_conf eth_conf = { 0 };
	struct br_runtime_port_config config = {
		.rx_queues = 1,
		.tx_queues = 1,
		.rx_descriptors = 64,
		.tx_descriptors = 64,
		.port_conf = &eth_conf,
	};
	struct rte_mempool *rx_pool = NULL;
	struct rte_mempool *main_tx_pool = NULL;
	uint16_t port_id = UINT16_MAX;
	uint32_t eal_argc = sizeof(eal_argv) / sizeof(eal_argv[0]);
	int ret = br_runtime_eal_init((int)eal_argc, eal_argv);
	if (ret < 0) {
		fprintf(stderr, "FAIL: DPDK EAL init returned %d\n", ret);
		return 1;
	}

	CHECK(br_runtime_port_configure(0, NULL, NULL, NULL) == -EINVAL,
		"NULL port config should be rejected");
	struct br_runtime_port_config no_queues = { .port_conf = &eth_conf };
	CHECK(br_runtime_port_configure(0, &no_queues, NULL, NULL) == -EINVAL,
		"zero RX/TX queues should be rejected");
	struct br_runtime_port_config rx_without_pool = config;
	CHECK(br_runtime_port_configure(0, &rx_without_pool, NULL, NULL) == -EINVAL,
		"RX queues without an RX pool should be rejected");
	struct br_runtime_port_config too_many_tx = {
		.tx_queues = UINT16_MAX,
		.port_conf = &eth_conf,
	};
	CHECK(br_runtime_port_configure(0, &too_many_tx, NULL, NULL) == -ENOSPC,
		"TX queues above device capability should be rejected");

	RTE_ETH_FOREACH_DEV(port_id)
		break;
	CHECK(port_id != UINT16_MAX && port_id < 32,
		"net_ring vdev should expose a port representable by the test mask");

	rx_pool = br_runtime_pktmbuf_pool_create("runtime_rx_pool", 1024, 0, 0,
			RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
	CHECK(rx_pool != NULL, "RX pool creation should succeed");
	config.rx_pool = rx_pool;
	uint16_t actual_rx_desc = 0, actual_tx_desc = 0;
	ret = br_runtime_port_configure(port_id, &config,
			&actual_rx_desc, &actual_tx_desc);
	CHECK(ret == 0, "RX/TX vdev port configuration should succeed");
	CHECK(actual_rx_desc == config.rx_descriptors &&
	      actual_tx_desc == config.tx_descriptors,
		"unadjusted descriptor counts should be returned unchanged");
	CHECK(br_runtime_port_start(port_id) == 0,
		"configured vdev port should start");

	main_tx_pool = br_runtime_tx_pool_create("runtime_main_tx_pool", 512,
			rte_socket_id());
	CHECK(main_tx_pool != NULL,
		"main lcore should create a TX pool alongside the RX pool");
	ret = rte_eal_remote_launch(worker_pool_create, NULL, 1);
	CHECK(ret == 0, "worker lcore should launch");
	ret = rte_eal_wait_lcore(1);
	CHECK(ret == 0,
		"a second lcore should register another pool without conflict");

	struct rte_mbuf *packet = rte_pktmbuf_alloc(main_tx_pool);
	CHECK(packet != NULL, "TX mbuf allocation should succeed");
	void *data = rte_pktmbuf_append(packet, 64);
	CHECK(data != NULL, "test packet should fit in the mbuf");
	memset(data, 0, 64);
	struct rte_mbuf *packets[] = { packet };
	uint16_t sent = br_runtime_tx_burst(port_id, 0, packets, 1);
	CHECK(sent <= 1, "TX wrapper must return no more than submitted packets");
	if (sent == 0)
		rte_pktmbuf_free(packet);

	CHECK(br_runtime_ports_stop(1u << port_id) == 0,
		"configured vdev port should stop and close");
	br_runtime_mempools_release();
	CHECK(br_runtime_eal_cleanup() == 0, "EAL cleanup should succeed");
	puts("DPDK runtime vdev tests: PASS");
	return 0;

fail:
	if (port_id != UINT16_MAX && port_id < 32)
		(void)br_runtime_ports_stop(1u << port_id);
	br_runtime_mempools_release();
	(void)br_runtime_eal_cleanup();
	return 1;
}
