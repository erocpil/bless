#include "config_bless_internal.h"

#include "bless_parse.h"
#include "bless_plugin.h"
#include "config_array.h"
#include "config_node.h"
#include "config_value.h"
#include "log.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <rte_ether.h>

static int
parse_payload(Node *node, char **payload, uint16_t *payload_len)
{
	if (!node || !node->value || !*node->value) {
		return 0;
	}
	size_t len = strlen(node->value) + 1;
	if (len > UINT16_MAX) {
		return -1;
	}
	char *copy = malloc(len);
	if (!copy) {
		return -1;
	}
	memcpy(copy, node->value, len);
	*payload = copy;
	*payload_len = (uint16_t)len;
	return 0;
}

static int config_parse_bless_ether(Node *bless_node, Cnode *cnode)
{
	Node *ether_node = find_by_path(bless_node, "ether");
	if (!ether_node) {
		return 0;
	}

	int64_t mtu = 0;
	Node *node = find_by_path(ether_node, "mtu");
	if (node) {
		char *endptr;
		errno = 0;
		mtu = (int64_t)strtoll(node->value, &endptr, 0);
		if (errno || endptr == node->value || *endptr != '\0') {
			LOG_WARN("invalid mtu value \"%s\", disabling", node->value);
			mtu = 0;
		} else if (mtu < 46) {
			LOG_INFO("set invalid mtu value %ld to 0(disabled)", mtu);
			mtu = 0;
		} else if (mtu > 1500) {
			LOG_INFO("set invalid mtu value %ld to 0(disabled)", mtu);
			mtu = 1500;
		}
	}
	cnode->ether.mtu = (uint16_t)mtu;

	/* IMIX -- random packet sizes */
	node = find_by_path(ether_node, "imix");
	if (node && node->type == NODE_SEQUENCE) {
		uint16_t n = 0;
		for (Node *child = node->child; child && n < BLESS_CONFIG_MAX;
				child = child->next, n++) {
			if (config_value_uint16(child->value, &cnode->ether.imix[n]) < 0) {
				LOG_WARN("invalid IMIX size \"%s\", using 0", child->value);
				cnode->ether.imix[n] = 0;
			}
		}
		cnode->ether.n_imix = n;
		LOG_INFO("IMIX configured with %u sizes", n);
	}

	cnode->ether.copy_payload = 0;
	node = find_by_path(ether_node, "copy-payload");
	if (node && node->value && strlen(node->value) &&
	    (!strcmp("true", node->value) ||
	     !strcmp("TRUE", node->value))) {
		cnode->ether.copy_payload = 1;
	}

	/* distribution weights: bless.ether.dist */	node = find_by_path(ether_node, "dist");
	if (node) {
		if (node->type == NODE_MAPPING) {
			/* dist: { tcp: 50, udp: 30 } */
			for (Node *child = node->child; child; child = child->next) {
				if (child->value && child->key) {
					char *end = NULL;
					long v = strtol(child->value, &end, 10);
					if (!*end && v > 0 && v < 100000) {
						bless_set_type_weight(child->key, (int32_t)v);
					}
				}
			}
		} else if (node->type == NODE_SEQUENCE) {
			/* dist: [ tcp: 50, udp: 30 ]  -- inline mapping entries */
			for (Node *child = node->child; child; child = child->next) {
				if (child->value && child->key) {
					char *end = NULL;
					long v = strtol(child->value, &end, 10);
					if (!*end && v > 0 && v < 100000) {
						bless_set_type_weight(child->key, (int32_t)v);
					}
				}
			}
		}
	}

	node = find_by_path(ether_node, "dst");
	if (!node) {
		goto ERROR;
	}

	/* take only 1 mac address */
	if (node->value && strlen(node->value) && NODE_SCALAR == node->type &&
			(rte_ether_unformat_addr(node->value,
									 (struct rte_ether_addr*)&(cnode->ether.dst))
			 == 0)) {
		// bless_print_mac((struct rte_ether_addr*)cnode->ether.dst);
	} else {
		LOG_WARN("Invalid dst mac address");
		goto ERROR;
	}
	cnode->ether.n_dst = 1;

	node = find_by_path(ether_node, "src");
	if (!node) {
		LOG_INFO("no src mac address node provided");
		cnode->ether.n_src = 0;
	} else {
		if (node->value && strlen(node->value) && NODE_SCALAR == node->type &&
				(rte_ether_unformat_addr(node->value, (struct rte_ether_addr*)
										 &(cnode->ether.src)) == 0)) {
			// bless_print_mac((struct rte_ether_addr*)cnode->ether.src);
			cnode->ether.n_src = 1;
		} else {
			LOG_INFO("Invalid src mac address, injector will try local port");
			cnode->ether.n_src = 0;
		}
	}

	return 1;

ERROR:
	LOG_ERR("yaml config parse error: ether.dst");
	return -1;
}

/* Parse the ``bless.vxlan.ether`` subsection (VXLAN outer MAC). */
/* Parse the ``bless.ether.type.arp`` section (ARP request/response config). */
static int config_parse_bless_ether_type_arp(Node *ether_node, Cnode *cnode)
{
	int n = 0;
	Node *node = find_by_path(ether_node, "type.arp.src");
	if (!node) {
		return 0;  /* ARP section not configured -- skip */
	}

	n = config_parse_sequence_ipv4_to_array(node, cnode->ether.type.arp.src,
			sizeof(uint32_t), IP_ADDR_MAX);
	if (n > 0) {
		cnode->ether.type.arp.n_src = n;
	} else {
		goto ERROR;
	}
	node = find_by_path(ether_node, "type.arp.dst");
	if (!node) {
		LOG_WARN("path type.arp.dst not found");
		return -1;
	}
	n = config_parse_sequence_ipv4_to_array(node, cnode->ether.type.arp.dst,
			sizeof(uint32_t), IP_ADDR_MAX);
	if (n > 0) {
		cnode->ether.type.arp.n_dst = n;
	} else {
		goto ERROR;
	}

	return n;

ERROR:
	LOG_ERR("yaml config parse error: type.arp");
	return -1;
}

/* Parse the ``bless.ether.type.ipv4.icmp`` section (ICMP Echo identifiers). */
static int config_parse_bless_ether_type_ipv4_icmp(Node *ether_node, Cnode *cnode)
{
	int n = 0;

	Node *node = find_by_path(ether_node, "type.ipv4.icmp.ident");
	if (!node) {
		return 0;  /* ICMP section not configured -- skip */
	}

	n = config_parse_sequence_to_array(node,
			cnode->ether.type.ipv4.icmp.ident, sizeof(uint16_t), PORT_MAX);
	if (n > 0) {
		cnode->ether.type.ipv4.icmp.n_ident = n;
	} else {
		goto ERROR;
	}

	node = find_by_path(ether_node, "type.ipv4.icmp.payload");
	if (parse_payload(node, &cnode->ether.type.ipv4.icmp.payload,
			  &cnode->ether.type.ipv4.icmp.payload_len) < 0) {
		goto ERROR;
	}

	return n;

ERROR:
	LOG_ERR("yaml config parse error: type.ipv4.icmp.ident");
	return -1;
}

/* Parse ``bless.ether.type.ipv4.tcp`` section (TCP src/dst ports, flags, payload). */
static int config_parse_bless_ether_type_ip_tcp(Node *ether_node, Cnode *cnode)
{
	int n = 0;

	Node *node = find_by_path(ether_node, "type.ipv4.tcp.src");
	if (!node) {
		return 0;  /* TCP section not configured -- skip */
	}

	int32_t range = 0;
	n = config_parse_port_maybe_range_to_array(node,
			cnode->ether.type.ipv4.tcp.src, &range, PORT_MAX);
	if (n > 0) {
		if (range) {
			cnode->ether.type.ipv4.tcp.src_range = range;
			cnode->ether.type.ipv4.tcp.n_src = 0;
		} else {
			cnode->ether.type.ipv4.tcp.src_range = 0;
			cnode->ether.type.ipv4.tcp.n_src = n;
		}
	} else {
		goto ERROR;
	}
	/*
	   printf("  src: ");
	   if (range) {
	   printf("%u + %u", cnode->ether.type.ipv4.tcp.src[0],
	   cnode->ether.type.ipv4.tcp.src_range);
	   } else {
	   for (int i = 0; i < n; i++) {
	   printf("%u ", cnode->ether.type.ipv4.tcp.src[i]);
	   }
	   }
	   printf("\n");
	   */

	node = find_by_path(ether_node, "type.ipv4.tcp.dst");
	if (!node) {
		goto ERROR;
	}
	range = 0;
	n = config_parse_port_maybe_range_to_array(node,
			cnode->ether.type.ipv4.tcp.dst, &range, PORT_MAX);
	if (n > 0) {
		if (range) {
			cnode->ether.type.ipv4.tcp.dst_range = range;
			cnode->ether.type.ipv4.tcp.n_dst = 0;
		} else {
			cnode->ether.type.ipv4.tcp.dst_range = 0;
			cnode->ether.type.ipv4.tcp.n_dst = n;
		}
	} else {
		goto ERROR;
	}
	/*
	   printf("  dst: ");
	   if (range) {
	   printf("%u + %u", cnode->ether.type.ipv4.tcp.dst[0],
	   cnode->ether.type.ipv4.tcp.dst_range);
	   } else {
	   for (int i = 0; i < n; i++) {
	   printf("%u ", cnode->ether.type.ipv4.tcp.dst[i]);
	   }
	   }
	   printf("\n");
	   */

	node = find_by_path(ether_node, "type.ipv4.tcp.payload");
	if (parse_payload(node, &cnode->ether.type.ipv4.tcp.payload,
			  &cnode->ether.type.ipv4.tcp.payload_len) < 0) {
		goto ERROR;
	}

	return n;

ERROR:
	LOG_ERR("yaml config parse error: type.ipv4.tcp");
	return -1;
}

/* Parse ``bless.ether.type.ipv4.udp`` section (UDP src/dst ports, payload). */
static int config_parse_bless_ether_type_ip_udp(Node *ether_node, Cnode *cnode)
{
	int n = 0;

	Node *node = find_by_path(ether_node, "type.ipv4.udp.src");
	if (!node) {
		return 0;  /* UDP section not configured -- skip */
	}

	int32_t range = 0;
	n = config_parse_port_maybe_range_to_array(node,
			cnode->ether.type.ipv4.udp.src, &range, PORT_MAX);
	if (n > 0) {
		if (range) {
			cnode->ether.type.ipv4.udp.src_range = range;
			cnode->ether.type.ipv4.udp.n_src = 0;
		} else {
			cnode->ether.type.ipv4.udp.src_range = 0;
			cnode->ether.type.ipv4.udp.n_src = n;
		}
	} else {
		goto ERROR;
	}
	/*
	   printf("  src:\n");
	   for (int i = 0; i < n; i++) {
	   printf("%u ", cnode->ether.type.ipv4.udp.src[i]);
	   }
	   printf("\n");
	   */

	node = find_by_path(ether_node, "type.ipv4.udp.dst");
	if (!node) {
		goto ERROR;
	}
	range = 0;
	n = config_parse_port_maybe_range_to_array(node,
			cnode->ether.type.ipv4.udp.dst, &range, PORT_MAX);
	if (n > 0) {
		if (range) {
			cnode->ether.type.ipv4.udp.dst_range = range;
			cnode->ether.type.ipv4.udp.n_dst = 0;
		} else {
			cnode->ether.type.ipv4.udp.dst_range = 0;
			cnode->ether.type.ipv4.udp.n_dst = n;
		}
	} else {
		goto ERROR;
	}

	node = find_by_path(ether_node, "type.ipv4.udp.payload");
	if (parse_payload(node, &cnode->ether.type.ipv4.udp.payload,
			  &cnode->ether.type.ipv4.udp.payload_len) < 0) {
		goto ERROR;
	}

	return n;

ERROR:
	LOG_ERR("yaml config parse error: type.ipv4.udp");
	return -1;
}

/* Parse the ``bless.ether.type.ipv4`` section (IP src/dst, proto, TOS, TTL). */
static int config_parse_bless_ether_type_ipv4(Node *ether_node, Cnode *cnode)
{
	int n = 0;

	Node *node = find_by_path(ether_node, "type.ipv4.src");
	if (!node) {
		goto ERROR;
	}

	int64_t range = 0;
	n = config_parse_ipv4_maybe_range_to_array(node,
			cnode->ether.type.ipv4.src, &range, IP_ADDR_MAX);
	if (n > 0) {
		if (range) {
			cnode->ether.type.ipv4.src_range = range;
			cnode->ether.type.ipv4.n_src = 0;
		} else {
			cnode->ether.type.ipv4.src_range = 0;
			cnode->ether.type.ipv4.n_src = n;
		}
	} else {
		goto ERROR;
	}

	node = find_by_path(ether_node, "type.ipv4.dst");
	if (!node) {
		goto ERROR;
	}
	n = config_parse_ipv4_maybe_range_to_array(node,
			cnode->ether.type.ipv4.dst, &range, IP_ADDR_MAX);
	if (n > 0) {
		if (range) {
			cnode->ether.type.ipv4.dst_range = range;
			cnode->ether.type.ipv4.n_dst = 0;
		} else {
			cnode->ether.type.ipv4.dst_range = 0;
			cnode->ether.type.ipv4.n_dst = n;
		}
	} else {
		goto ERROR;
	}

	return n;

ERROR:
	LOG_ERR("yaml config parse error: type.ipv4");
	return -1;
}

/* Parse the ``bless.ether.type.ipv6`` section.
 *
 * Handles src/dst addresses in range ("2001:db8::1+100") or sequence
 * (["2001:db8::1", "2001:db8::2"]) format, plus TCP/UDP port configs. */
static int config_parse_bless_ether_type_ipv6(Node *ether_node, Cnode *cnode)
{
	Node *node = find_by_path(ether_node, "type.ipv6");
	if (!node) {
		return 0; /* IPv6 section not present -- OK */
	}

	/* src addresses */	Node *src_node = find_by_path(ether_node, "type.ipv6.src");
	if (src_node) {
		if (src_node->type == NODE_SCALAR) {
			int64_t range = 0;
			if (bless_parse_ipv6_range(src_node->value,
					cnode->ether.type.ipv6.src[0], &range) == 0) {
				cnode->ether.type.ipv6.src_range = range;
				cnode->ether.type.ipv6.n_src = range ? 0 : 1;
			}
		} else if (src_node->type == NODE_SEQUENCE) {
			int n = 0;
			for (Node *i = src_node->child;
			     i && n < IP_ADDR_MAX; i = i->next, n++) {
				inet_pton(AF_INET6, i->value,
					  cnode->ether.type.ipv6.src[n]);
			}
			cnode->ether.type.ipv6.n_src = n;
			cnode->ether.type.ipv6.src_range = 0;
		}
	}
	if (!cnode->ether.type.ipv6.n_src &&
			!cnode->ether.type.ipv6.src_range) {
		goto ERROR;
	}

	/* dst addresses */	Node *dst_node = find_by_path(ether_node, "type.ipv6.dst");
	if (dst_node) {
		if (dst_node->type == NODE_SCALAR) {
			int64_t range = 0;
			if (bless_parse_ipv6_range(dst_node->value,
					cnode->ether.type.ipv6.dst[0], &range) == 0) {
				cnode->ether.type.ipv6.dst_range = range;
				cnode->ether.type.ipv6.n_dst = range ? 0 : 1;
			}
		} else if (dst_node->type == NODE_SEQUENCE) {
			int n = 0;
			for (Node *i = dst_node->child;
			     i && n < IP_ADDR_MAX; i = i->next, n++) {
				inet_pton(AF_INET6, i->value,
					  cnode->ether.type.ipv6.dst[n]);
			}
			cnode->ether.type.ipv6.n_dst = n;
			cnode->ether.type.ipv6.dst_range = 0;
		}
	}
	if (!cnode->ether.type.ipv6.n_dst &&
			!cnode->ether.type.ipv6.dst_range) {
		goto ERROR;
	}

	/* TCP ports */	{
		Node *tnode = find_by_path(ether_node, "type.ipv6.tcp.src");
		if (tnode) {
			int n = config_parse_port_maybe_range_to_array(tnode,
					cnode->ether.type.ipv6.tcp.src,
					&cnode->ether.type.ipv6.tcp.src_range,
					PORT_MAX);
			if (n > 0) {
				cnode->ether.type.ipv6.tcp.n_src = n;
			}
		}
		tnode = find_by_path(ether_node, "type.ipv6.tcp.dst");
		if (tnode) {
			int n = config_parse_port_maybe_range_to_array(tnode,
					cnode->ether.type.ipv6.tcp.dst,
					&cnode->ether.type.ipv6.tcp.dst_range,
					PORT_MAX);
			if (n > 0) {
				cnode->ether.type.ipv6.tcp.n_dst = n;
			}
		}
	}

	/* UDP ports */	{
		Node *unode = find_by_path(ether_node, "type.ipv6.udp.src");
		if (unode) {
			int n = config_parse_port_maybe_range_to_array(unode,
					cnode->ether.type.ipv6.udp.src,
					&cnode->ether.type.ipv6.udp.src_range,
					PORT_MAX);
			if (n > 0) {
				cnode->ether.type.ipv6.udp.n_src = n;
			}
		}
		unode = find_by_path(ether_node, "type.ipv6.udp.dst");
		if (unode) {
			int n = config_parse_port_maybe_range_to_array(unode,
					cnode->ether.type.ipv6.udp.dst,
					&cnode->ether.type.ipv6.udp.dst_range,
					PORT_MAX);
			if (n > 0) {
				cnode->ether.type.ipv6.udp.n_dst = n;
			}
		}
	}

	return 0;

ERROR:
	LOG_ERR("yaml config parse error: type.ipv6");
	return -1;
}

/* Parse the ``bless.vxlan`` section (enable, ratio, VNI ranges, wire MTU). */
int
config_parse_bless_ether_all(Node *bless_node, Node *ether_node, Cnode *cnode)
{
	if (config_parse_bless_ether(bless_node, cnode) < 0 ||
	    config_parse_bless_ether_type_arp(ether_node, cnode) < 0 ||
	    config_parse_bless_ether_type_ipv4(ether_node, cnode) < 0 ||
	    config_parse_bless_ether_type_ipv6(ether_node, cnode) < 0 ||
	    config_parse_bless_ether_type_ipv4_icmp(ether_node, cnode) < 0 ||
	    config_parse_bless_ether_type_ip_tcp(ether_node, cnode) < 0 ||
	    config_parse_bless_ether_type_ip_udp(ether_node, cnode) < 0) {
		return -1;
	}
	return 0;
}
