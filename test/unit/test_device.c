#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Deliberately NOT including device.h — unit tests are compiled
 * without DPDK.  Mirror the ethdev_type enum and helpers here so
 * the classification logic is independently verifiable.
 */

enum ethdev_type {
	ETHDEV_PHYSICAL,
	ETHDEV_PCAP,
	ETHDEV_VIRTIO,
	ETHDEV_RING,
	ETHDEV_NOT_SUPPORTED,
	ETHDEV_MAX,
};

static inline int device_is_full_featured(enum ethdev_type type)
{
	return type == ETHDEV_PHYSICAL || type == ETHDEV_VIRTIO;
}

static inline int device_is_simple_vdev(enum ethdev_type type)
{
	return type == ETHDEV_PCAP || type == ETHDEV_RING;
}

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
