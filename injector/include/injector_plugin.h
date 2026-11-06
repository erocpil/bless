#ifndef __INJECTOR_PLUGIN_H__
#define __INJECTOR_PLUGIN_H__

#include <stdint.h>
#include <rte_mbuf.h>

/*
 * Injector Plugin Interface
 *
 * Extend Injector with new protocol types and mutation functions.
 * Register at compile time via __attribute__((constructor)):
 *
 *   static void __attribute__((constructor)) register_my_proto(void) {
 *       static const struct injector_pkt_type t = {
 *           .name       = "gre",
 *           .ether_type = RTE_ETHER_TYPE_IPV4,
 *           .ip_proto   = 47,
 *           .type_idx   = INJECTOR_AUTO_IDX,  // auto-assign for ext protocols
 *           .construct  = my_gre_construct,
 *       };
 *       injector_register_pkt_type(&t);
 *       injector_set_type_weight("gre", 30);  // optional default weight
 *   }
 */

/* Maximum built-in types (ARP/ICMP/TCP/UDP) -- ext types start at this base */
#define INJECTOR_NUM_BUILTIN  4   /* arp, icmp, tcp, udp */
/* Maximum total types (built-in + ext) */
#define INJECTOR_MAX_TYPES    32

/** Default type_idx for auto-assignment (extension protocols should use this). */
#define INJECTOR_AUTO_IDX  255

/* Packet Type Constructor */
/** Construct n packets of this protocol in pre-allocated mbufs.
 *  @param mbufs  Array of n pre-allocated mbufs (must be non-NULL)
 *  @param n      Number of mbufs to fill
 *  @param cfg    Cnode* configuration (type-cast from void*)
 *  @return       Total bytes written across all n mbufs, or 0 on error
 */
typedef uint64_t (*injector_pkt_ctor_t)(struct rte_mbuf **mbufs,
                                     unsigned int n, void *cfg);

/** A registered packet type descriptor. */
struct injector_pkt_type {
    const char *name;              /* protocol name, e.g. "tcp", "gre" */
    uint16_t    ether_type;        /* Ethernet type (0x0800=IPv4, etc.) */
    uint8_t     ip_proto;          /* IP protocol number (6=TCP, 47=GRE) */
    uint8_t     type_idx;          /* 0 = auto-assign, 1-255 = explicit.
                                    * Built-in types MUST use explicit indices
                                    * matching the enum in injector.h:
                                    *   0=arp, 1=icmp, 2=tcp, 3=udp.
                                    * Extension types use 0 (auto-assigned). */
    injector_pkt_ctor_t construct;    /* packet constructor */
};

/* Registration API (call from __attribute__((constructor))) */
/** Register a packet type. Returns the assigned type_idx. */
unsigned int injector_register_pkt_type(const struct injector_pkt_type *type);

/** Set the weight for a protocol type by name.
 *  Weight determines the proportion of packets for this type in distribution.
 *  Weight 0 = excluded from distribution. */
void injector_set_type_weight(const char *name, int32_t weight);

/** Get weight for a protocol type by name. Returns 0 if not set / unknown. */
int32_t injector_get_type_weight(const char *name);

/* Lookup API (used by injector.c dispatch) */
/** Find a registered packet type by name. Returns NULL if not found. */
const struct injector_pkt_type *injector_find_pkt_type(const char *name);

/** Iterate all registered packet types. Call fn(type, ctx) for each. */
void injector_foreach_pkt_type(void (*fn)(const struct injector_pkt_type *t,
                                       void *ctx), void *ctx);

/** Look up constructor by type index (for injector_mbufs dispatch). */
injector_pkt_ctor_t injector_get_ctor(unsigned int type_idx);

/** Get type name by type index. Returns "unknown" if not found. */
const char *injector_get_type_name(unsigned int type_idx);

#endif /* __INJECTOR_PLUGIN_H__ */
