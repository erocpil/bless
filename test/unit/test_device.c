#include <stdio.h>
#include <stdlib.h>

/* Pure device classification has no DPDK dependency. */
#include "device_type.h"

static int n_pass = 0, n_fail = 0;

#define TEST(name, expr) do { \
	if (expr) { \
		printf("  PASS: %s\n", name); \
		n_pass++; \
	} else { \
		printf("  FAIL: %s\n", name); \
		n_fail++; \
	} \
} while (0)

int main(void)
{
	printf("BLESS device classification unit tests\n");
	printf("=======================================\n\n");

	printf("=== driver-name classification ===\n");
	TEST("Virtio PCI driver is supported",
	     device_classify_driver_name("net_virtio") == ETHDEV_VIRTIO);
	TEST("Virtio-user vdev driver is supported",
	     device_classify_driver_name("net_virtio_user") == ETHDEV_VIRTIO);
	TEST("Virtio-like unknown driver is rejected",
	     device_classify_driver_name("net_virtio_unknown") ==
	     ETHDEV_NOT_SUPPORTED);
	TEST("NULL driver name is unavailable",
	     device_classify_driver_name(NULL) == ETHDEV_MAX);
	TEST("mlx5 PCI driver remains physical",
	     device_classify_driver_name("mlx5_pci") == ETHDEV_PHYSICAL);
	TEST("PCAP driver remains simple virtual",
	     device_classify_driver_name("net_pcap") == ETHDEV_PCAP);

	printf("=== device_is_full_featured ===\n");
	TEST("PHYSICAL is full-featured",
	     device_is_full_featured(ETHDEV_PHYSICAL) == 1);
	TEST("VIRTIO is full-featured",
	     device_is_full_featured(ETHDEV_VIRTIO) == 1);
	TEST("PCAP is NOT full-featured",
	     device_is_full_featured(ETHDEV_PCAP) == 0);
	TEST("RING is NOT full-featured",
	     device_is_full_featured(ETHDEV_RING) == 0);
	TEST("NOT_SUPPORTED is NOT full-featured",
	     device_is_full_featured(ETHDEV_NOT_SUPPORTED) == 0);
	TEST("MAX is NOT full-featured",
	     device_is_full_featured(ETHDEV_MAX) == 0);

	printf("\n=== device_is_simple_vdev ===\n");
	TEST("PCAP is simple vdev",
	     device_is_simple_vdev(ETHDEV_PCAP) == 1);
	TEST("RING is simple vdev",
	     device_is_simple_vdev(ETHDEV_RING) == 1);
	TEST("PHYSICAL is NOT simple vdev",
	     device_is_simple_vdev(ETHDEV_PHYSICAL) == 0);
	TEST("VIRTIO is NOT simple vdev",
	     device_is_simple_vdev(ETHDEV_VIRTIO) == 0);
	TEST("NOT_SUPPORTED is NOT simple vdev",
	     device_is_simple_vdev(ETHDEV_NOT_SUPPORTED) == 0);
	TEST("MAX is NOT simple vdev",
	     device_is_simple_vdev(ETHDEV_MAX) == 0);

	printf("\n=== exhaustive coverage ===\n");
	for (int t = 0; t < ETHDEV_MAX; t++) {
		int ff = device_is_full_featured((enum ethdev_type)t);
		int sv = device_is_simple_vdev((enum ethdev_type)t);
		int covered = ff || sv;

		const char *name;
		switch (t) {
		case ETHDEV_PHYSICAL:      name = "PHYSICAL"; break;
		case ETHDEV_PCAP:          name = "PCAP"; break;
		case ETHDEV_VIRTIO:        name = "VIRTIO"; break;
		case ETHDEV_RING:          name = "RING"; break;
		case ETHDEV_NOT_SUPPORTED: name = "NOT_SUPPORTED"; break;
		default:                   name = "?"; break;
		}

		if (t == ETHDEV_NOT_SUPPORTED) {
			TEST("NOT_SUPPORTED is uncovered (expected)",
			     covered == 0);
		} else {
			TEST(name, covered == 1);
		}
	}

	printf("\n=======================\n");
	printf("Results: %d passed, %d failed\n", n_pass, n_fail);

	return n_fail ? EXIT_FAILURE : EXIT_SUCCESS;
}
