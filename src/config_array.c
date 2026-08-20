#include "config_array.h"

#include "bless_parse.h"
#include "config_value.h"

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static int
valid_array_args(Node *node, const void *array, int size, uint32_t limit)
{
	return node && array && (size == (int)sizeof(uint16_t) ||
		size == (int)sizeof(uint32_t)) && limit > 0;
}

int
config_parse_sequence_to_array(Node *node, void *array, int size,
		uint16_t limit)
{
	if (!valid_array_args(node, array, size, limit)) {
		return -1;
	}
	int count = 0;
	if (node->type == NODE_SCALAR) {
		int rc = size == (int)sizeof(uint16_t)
			? config_value_uint16(node->value, array)
			: config_value_uint32(node->value, array);
		return rc < 0 ? -1 : 1;
	}
	if (node->type != NODE_SEQUENCE) {
		return 0;
	}
	for (Node *item = node->child; item && count < limit;
			item = item->next, count++) {
		void *slot = (char *)array + (size_t)size * (size_t)count;
		int rc = size == (int)sizeof(uint16_t)
			? config_value_uint16(item->value, slot)
			: config_value_uint32(item->value, slot);
		if (rc < 0) {
			return -1;
		}
	}
	return count;
}

int
config_parse_port_maybe_range_to_array(Node *node, uint16_t *ports,
		int32_t *range, uint16_t limit)
{
	if (!node || !ports || !range || !limit) {
		return -1;
	}
	if (node->type == NODE_SCALAR) {
		uint16_t port;
		int32_t parsed_range;
		if (bless_parse_port_range(node->value, &port, &parsed_range) < 0) {
			return -1;
		}
		ports[0] = port;
		*range = parsed_range;
		return 1;
	}
	if (node->type != NODE_SEQUENCE) {
		return 0;
	}
	int count = config_parse_sequence_to_array(node, ports,
		sizeof(*ports), limit);
	if (count >= 0) {
		*range = 0;
	}
	return count;
}

static int
parse_vni_range(const char *text, uint32_t *base)
{
	if (!text || !*text || !base) {
		return -1;
	}
	char *end;
	errno = 0;
	unsigned long value = strtoul(text, &end, 0);
	if (errno || end == text || value > UINT32_MAX) {
		return -1;
	}
	*base = (uint32_t)value;
	if (!*end) {
		return 1;
	}
	if (*end++ != '+' || !*end) {
		return -1;
	}
	errno = 0;
	unsigned long count = strtoul(end, &end, 0);
	if (errno || *end || !count || count > INT_MAX ||
	    value + count - 1 > UINT32_MAX) {
		return -1;
	}
	return (int)count;
}

static int
parse_vtep_vni(const char *text, uint32_t *address, uint32_t *base_vni,
		int *range_count)
{
	if (!text || !address || !base_vni || !range_count) {
		return -1;
	}
	char *copy = strdup(text);
	if (!copy) {
		return -1;
	}
	char *separator = strchr(copy, ':');
	if (!separator || !separator[1]) {
		free(copy);
		return -1;
	}
	*separator++ = '\0';
	int rc = inet_pton(AF_INET, copy, address);
	if (rc == 1) {
		*range_count = parse_vni_range(separator, base_vni);
	}
	free(copy);
	return rc == 1 && *range_count > 0 ? 0 : -1;
}

int
config_parse_sequence_ipv4_vni_to_array(Node *node, uint32_t *addresses,
		uint32_t *vnis, int size, uint16_t limit)
{
	if (!valid_array_args(node, addresses, size, limit) || !vnis ||
	    size != (int)sizeof(*addresses)) {
		return -1;
	}
	if (node->type != NODE_SCALAR && node->type != NODE_SEQUENCE) {
		return 0;
	}

	int count = 0;
	Node scalar = { 0 };
	Node *item;
	if (node->type == NODE_SCALAR) {
		scalar.value = node->value;
		item = &scalar;
	} else {
		item = node->child;
	}
	for (; item && count < limit; item = item->next) {
		uint32_t address;
		uint32_t base_vni;
		int range_count;
		if (parse_vtep_vni(item->value, &address, &base_vni,
				   &range_count) < 0) {
			return -1;
		}
		for (int offset = 0; offset < range_count && count < limit;
				offset++, count++) {
			addresses[count] = address;
			vnis[count] = base_vni + (uint32_t)offset;
		}
	}
	return count;
}

int
config_parse_ipv4_maybe_range_to_array(Node *node, uint32_t *addresses,
		int64_t *range, uint32_t limit)
{
	if (!node || !addresses || !range || !limit) {
		return -1;
	}
	if (node->type == NODE_SCALAR) {
		uint32_t address;
		int64_t parsed_range;
		if (bless_parse_ip_range(node->value, &address, &parsed_range) < 0) {
			return -1;
		}
		addresses[0] = address;
		*range = parsed_range;
		return 1;
	}
	if (node->type != NODE_SEQUENCE) {
		return 0;
	}
	uint32_t count = 0;
	for (Node *item = node->child; item && count < limit;
			item = item->next, count++) {
		if (inet_pton(AF_INET, item->value, &addresses[count]) != 1) {
			return -1;
		}
	}
	*range = 0;
	return (int)count;
}

int
config_parse_sequence_ipv4_to_array(Node *node, void *array, int size,
		uint16_t limit)
{
	if (!node || !array || size <= 0 || !limit) {
		return -1;
	}
	if (node->type == NODE_SCALAR) {
		return inet_pton(AF_INET, node->value, array) == 1 ? 1 : -1;
	}
	if (node->type != NODE_SEQUENCE) {
		return 0;
	}
	int count = 0;
	for (Node *item = node->child; item && count < limit;
			item = item->next, count++) {
		void *slot = (char *)array + (size_t)size * (size_t)count;
		if (inet_pton(AF_INET, item->value, slot) != 1) {
			return -1;
		}
	}
	return count;
}
