#define _GNU_SOURCE

#include <errno.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_errno.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>

#include "device_type.h"

#define TEST_VDEV_NAME "virtio_user_bless_test"
#define TEST_IFACE_NAME "bless-vu0"
#define TEST_QUEUE_SIZE 256
#define TEST_MBUF_COUNT 1024

static int test_failed(const char *step, int ret)
{
	fprintf(stderr, "FAIL: %s: ret=%d (%s)\n", step, ret,
		ret < 0 ? rte_strerror(-ret) : "unexpected result");
	return EXIT_FAILURE;
}

int main(void)
{
	if (access("/dev/vhost-net", R_OK | W_OK) != 0) {
		printf("SKIP: /dev/vhost-net is not available or not accessible\n");
		return EXIT_SUCCESS;
	}

	int cpu = sched_getcpu();
	if (cpu < 0) {
		perror("FAIL: sched_getcpu");
		return EXIT_FAILURE;
	}

	char lcore_arg[32];
	char prefix_arg[64];
	snprintf(lcore_arg, sizeof(lcore_arg), "%d", cpu);
	snprintf(prefix_arg, sizeof(prefix_arg), "--file-prefix=bless-vu-%ld",
		(long)getpid());

	char *eal_argv[] = {
		"test_virtio_user",
		"-l", lcore_arg,
		"--main-lcore", lcore_arg,
		"--no-pci",
		"--no-shconf",
		prefix_arg,
	};
	int eal_argc = (int)(sizeof(eal_argv) / sizeof(eal_argv[0]));
	int ret = rte_eal_init(eal_argc, eal_argv);
	if (ret < 0) {
		fprintf(stderr,
			"SKIP: DPDK EAL could not initialize; check hugepages and permissions\n");
		return EXIT_SUCCESS;
	}

	const char *vdev_args =
		"path=/dev/vhost-net,iface=" TEST_IFACE_NAME
		",queues=1,queue_size=256";
	ret = rte_eal_hotplug_add("vdev", TEST_VDEV_NAME, vdev_args);
	if (ret < 0) {
		rte_eal_cleanup();
		return test_failed("rte_eal_hotplug_add", ret);
	}

	uint16_t port_id;
	ret = rte_eth_dev_get_port_by_name(TEST_VDEV_NAME, &port_id);
	if (ret < 0) {
		goto remove_vdev;
	}

	struct rte_eth_dev_info info;
	memset(&info, 0, sizeof(info));
	ret = rte_eth_dev_info_get(port_id, &info);
	if (ret < 0) {
		goto remove_vdev;
	}
	if (device_classify_driver_name(info.driver_name) != ETHDEV_VIRTIO ||
	    !info.driver_name || strcmp(info.driver_name, "net_virtio_user") != 0) {
		fprintf(stderr, "FAIL: unexpected driver name: %s\n",
			info.driver_name ? info.driver_name : "(null)");
		ret = -ENOTSUP;
		goto remove_vdev;
	}
	if (info.max_rx_queues < 1 || info.max_tx_queues < 1) {
		fprintf(stderr, "FAIL: virtio-user exposes no usable queue pair\n");
		ret = -ENOTSUP;
		goto remove_vdev;
	}

	struct rte_eth_conf port_conf;
	memset(&port_conf, 0, sizeof(port_conf));
	ret = rte_eth_dev_configure(port_id, 1, 1, &port_conf);
	if (ret < 0) {
		goto remove_vdev;
	}

	uint16_t nb_rxd = TEST_QUEUE_SIZE;
	uint16_t nb_txd = TEST_QUEUE_SIZE;
	ret = rte_eth_dev_adjust_nb_rx_tx_desc(port_id, &nb_rxd, &nb_txd);
	if (ret < 0) {
		goto close_port;
	}

	struct rte_mempool *pool = rte_pktmbuf_pool_create(
		"bless_virtio_user_test_pool", TEST_MBUF_COUNT, 0, 0,
		RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
	if (!pool) {
		ret = -rte_errno;
		goto close_port;
	}

	ret = rte_eth_rx_queue_setup(port_id, 0, nb_rxd, rte_socket_id(),
		&info.default_rxconf, pool);
	if (ret < 0) {
		goto free_pool;
	}
	ret = rte_eth_tx_queue_setup(port_id, 0, nb_txd, rte_socket_id(),
		&info.default_txconf);
	if (ret < 0) {
		goto free_pool;
	}
	ret = rte_eth_dev_start(port_id);
	if (ret < 0) {
		goto free_pool;
	}

	printf("PASS: hotplugged and started %s with driver %s, RXD=%u TXD=%u\n",
		TEST_VDEV_NAME, info.driver_name, nb_rxd, nb_txd);
	rte_eth_dev_stop(port_id);
	rte_eth_dev_close(port_id);
	rte_mempool_free(pool);
	rte_eal_hotplug_remove("vdev", TEST_VDEV_NAME);
	rte_eal_cleanup();
	return EXIT_SUCCESS;

free_pool:
	rte_eth_dev_close(port_id);
	rte_mempool_free(pool);
	goto remove_vdev;
close_port:
	rte_eth_dev_close(port_id);
remove_vdev:
	(void)rte_eal_hotplug_remove("vdev", TEST_VDEV_NAME);
	(void)rte_eal_cleanup();
	return test_failed("virtio-user integration", ret);
}
