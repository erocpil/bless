#ifndef __BLESS_CNODE_RUNTIME_H__
#define __BLESS_CNODE_RUNTIME_H__

#include "cnode.h"

/* Fast-path selectors for values stored in Cnode. */
#define RANDOM_VXLAN_IP_SRC(p) \
	random_array_elem_uint32_t(p->vxlan.ether.type.ipv4.src, \
		p->vxlan.ether.type.ipv4.n_src, p->vxlan.ether.type.ipv4.src_range)
#define RANDOM_VXLAN_IP_DST(p) \
	random_array_elem_uint32_t(p->vxlan.ether.type.ipv4.dst, \
		p->vxlan.ether.type.ipv4.n_dst, p->vxlan.ether.type.ipv4.dst_range)
#define RANDOM_VXLAN_IP_VNI(p) \
	random_array_elem_uint32_t_with_peer(p->vxlan.ether.type.ipv4.src, \
		p->vxlan.ether.type.ipv4.vni, p->vxlan.ether.type.ipv4.n_src, \
		p->vxlan.ether.type.ipv4.src_range)
#define RANDOM_VXLAN_UDP_SRC(p) \
	rte_cpu_to_be_16(random_array_elem_uint16_t( \
		p->vxlan.ether.type.ipv4.udp.src, \
		p->vxlan.ether.type.ipv4.udp.n_src, \
		p->vxlan.ether.type.ipv4.udp.src_range))
#define RANDOM_VXLAN_UDP_DST(p) \
	rte_cpu_to_be_16(random_array_elem_uint16_t( \
		p->vxlan.ether.type.ipv4.udp.dst, \
		p->vxlan.ether.type.ipv4.udp.n_dst, \
		p->vxlan.ether.type.ipv4.udp.dst_range))
#define RANDOM_VXLAN6_UDP_SRC(p) \
	rte_cpu_to_be_16(random_array_elem_uint16_t( \
		p->vxlan.ether.type.ipv6.udp.src, \
		p->vxlan.ether.type.ipv6.udp.n_src, \
		p->vxlan.ether.type.ipv6.udp.src_range))
#define RANDOM_VXLAN6_UDP_DST(p) \
	rte_cpu_to_be_16(random_array_elem_uint16_t( \
		p->vxlan.ether.type.ipv6.udp.dst, \
		p->vxlan.ether.type.ipv6.udp.n_dst, \
		p->vxlan.ether.type.ipv6.udp.dst_range))

#define RANDOM_IP_SRC(p) \
	random_array_elem_uint32_t(p->ether.type.ipv4.src, \
		p->ether.type.ipv4.n_src, p->ether.type.ipv4.src_range)
#define RANDOM_IP_DST(p) \
	random_array_elem_uint32_t(p->ether.type.ipv4.dst, \
		p->ether.type.ipv4.n_dst, p->ether.type.ipv4.dst_range)
#define RANDOM_TCP_SRC(p) \
	rte_cpu_to_be_16(random_array_elem_uint16_t(p->ether.type.ipv4.tcp.src, \
		p->ether.type.ipv4.tcp.n_src, p->ether.type.ipv4.tcp.src_range))
#define RANDOM_TCP_DST(p) \
	rte_cpu_to_be_16(random_array_elem_uint16_t(p->ether.type.ipv4.tcp.dst, \
		p->ether.type.ipv4.tcp.n_dst, p->ether.type.ipv4.tcp.dst_range))
#define RANDOM_UDP_SRC(p) \
	rte_cpu_to_be_16(random_array_elem_uint16_t(p->ether.type.ipv4.udp.src, \
		p->ether.type.ipv4.udp.n_src, p->ether.type.ipv4.udp.src_range))
#define RANDOM_UDP_DST(p) \
	rte_cpu_to_be_16(random_array_elem_uint16_t(p->ether.type.ipv4.udp.dst, \
		p->ether.type.ipv4.udp.n_dst, p->ether.type.ipv4.udp.dst_range))

#define RANDOM_IPV6_TCP_SRC(p) \
	rte_cpu_to_be_16(random_array_elem_uint16_t(p->ether.type.ipv6.tcp.src, \
		p->ether.type.ipv6.tcp.n_src, p->ether.type.ipv6.tcp.src_range))
#define RANDOM_IPV6_TCP_DST(p) \
	rte_cpu_to_be_16(random_array_elem_uint16_t(p->ether.type.ipv6.tcp.dst, \
		p->ether.type.ipv6.tcp.n_dst, p->ether.type.ipv6.tcp.dst_range))
#define RANDOM_IPV6_UDP_SRC(p) \
	rte_cpu_to_be_16(random_array_elem_uint16_t(p->ether.type.ipv6.udp.src, \
		p->ether.type.ipv6.udp.n_src, p->ether.type.ipv6.udp.src_range))
#define RANDOM_IPV6_UDP_DST(p) \
	rte_cpu_to_be_16(random_array_elem_uint16_t(p->ether.type.ipv6.udp.dst, \
		p->ether.type.ipv6.udp.n_dst, p->ether.type.ipv6.udp.dst_range))

#define IMIX_SIZE(cnode) \
	((cnode)->ether.n_imix \
		? random_array_elem_uint16_t((cnode)->ether.imix, \
			(cnode)->ether.n_imix, 0) \
		: 0)
#define IMIX_PAYLOAD_LEN(cnode, l3, l4) \
	({ \
		uint16_t _total = IMIX_SIZE(cnode); \
		_total ? (_total > (l3) + (l4) ? _total - (l3) - (l4) : 0) : 0; \
	})

#endif
