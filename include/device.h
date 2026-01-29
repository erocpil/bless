#ifndef __DEVICE_H__
#define __DEVICE_H__

#include <stdint.h>

enum ethdev_type {
	ETHDEV_PHYSICAL,
	ETHDEV_PCAP,
	ETHDEV_VIRTIO,
	ETHDEV_RING,
	ETHDEV_NOT_SUPPORTED,
	ETHDEV_OTHER,
};

enum ethdev_type device_get_ethdev_type(uint16_t portid);
void device_show_info(uint16_t portid);
char *device_get_string(uint16_t type);

#endif
