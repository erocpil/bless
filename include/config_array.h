#ifndef __BLESS_CONFIG_ARRAY_H__
#define __BLESS_CONFIG_ARRAY_H__

#include <stdint.h>

#include "config_yaml.h"

int config_parse_sequence_to_array(Node *node, void *array, int size,
	uint16_t limit);
int config_parse_port_maybe_range_to_array(Node *node, uint16_t *ports,
	int32_t *range, uint16_t limit);
int config_parse_sequence_ipv4_vni_to_array(Node *node, uint32_t *addresses,
	uint32_t *vnis, int size, uint16_t limit);
int config_parse_ipv4_maybe_range_to_array(Node *node, uint32_t *addresses,
	int64_t *range, uint32_t limit);
int config_parse_sequence_ipv4_to_array(Node *node, void *array, int size,
	uint16_t limit);

#endif
