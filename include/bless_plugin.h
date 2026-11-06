#ifndef __BLESS_PLUGIN_H__
#define __BLESS_PLUGIN_H__

#include <stdint.h>
#include <rte_mbuf.h>

/*
 * Bless Plugin Interface
 *
 * Extend Bless with new protocol types and mutation functions.
 * Register at compile time via __attribute__((constructor)):
 *
 *   static void __attribute__((constructor)) register_my_proto(void) {
 *       static const struct bless_pkt_type t = {
 *           .name       = "gre",
 *           .ether_type = RTE_ETHER_TYPE_IPV4,
 *           .ip_proto   = 47,
 *           .type_idx   = BLESS_AUTO_IDX,  // auto-assign for ext protocols
 *           .construct  = my_gre_construct,
 *       };
 *       bless_register_pkt_type(&t);
 *       bless_set_type_weight("gre", 30);  // optional default weight
 *   }
 */

/* Maximum built-in types (ARP/ICMP/TCP/UDP) -- ext types start at this base */
#define BLESS_NUM_BUILTIN  5   /* arp, icmp, tcp, udp, sctp */
/* Maximum total types (built-in + ext) */
#define BLESS_MAX_TYPES    32

/** Default type_idx for auto-assignment (extension protocols should use this). */
#define BLESS_AUTO_IDX  255

/* Packet Type Constructor */
/** Construct n packets of this protocol in pre-allocated mbufs.
 *  @param mbufs  Array of n pre-allocated mbufs (must be non-NULL)
 *  @param n      Number of mbufs to fill
 *  @param cfg    Cnode* configuration (type-cast from void*)
 *  @return       Total bytes written across all n mbufs, or 0 on error
 */
typedef uint64_t (*bless_pkt_ctor_t)(struct rte_mbuf **mbufs,
                                     unsigned int n, void *cfg);

/** A registered packet type descriptor. */
struct bless_pkt_type {
    const char *name;              /* protocol name, e.g. "tcp", "gre" */
    uint16_t    ether_type;        /* Ethernet type (0x0800=IPv4, etc.) */
    uint8_t     ip_proto;          /* IP protocol number (6=TCP, 47=GRE) */
    uint8_t     type_idx;          /* 0 = auto-assign, 1-255 = explicit.
                                    * Built-in types MUST use explicit indices
                                    * matching the enum in bless.h:
                                    *   0=arp, 1=icmp, 2=tcp, 3=udp, 4=sctp.
                                    * Extension types use 0 (auto-assigned). */
    bless_pkt_ctor_t construct;    /* packet constructor */
};

/* Registration API (call from __attribute__((constructor))) */
/** Register a packet type. Returns the assigned type_idx. */
unsigned int bless_register_pkt_type(const struct bless_pkt_type *type);

/** Set the weight for a protocol type by name.
 *  Weight determines the proportion of packets for this type in distribution.
 *  Weight 0 = excluded from distribution. */
void bless_set_type_weight(const char *name, int32_t weight);

/** Get weight for a protocol type by name. Returns 0 if not set / unknown. */
int32_t bless_get_type_weight(const char *name);

/* Lookup API (used by bless.c dispatch) */
/** Find a registered packet type by name. Returns NULL if not found. */
const struct bless_pkt_type *bless_find_pkt_type(const char *name);

/** Iterate all registered packet types. Call fn(type, ctx) for each. */
void bless_foreach_pkt_type(void (*fn)(const struct bless_pkt_type *t,
                                       void *ctx), void *ctx);

/** Look up constructor by type index (for bless_mbufs dispatch). */
bless_pkt_ctor_t bless_get_ctor(unsigned int type_idx);

/** Get type name by type index. Returns "unknown" if not found. */
const char *bless_get_type_name(unsigned int type_idx);

/** Get IP protocol number by type index. Returns 0 if not found.
 *  Used by entropy sampler to tag extension protocol samples. */
uint8_t bless_pkt_ip_proto(unsigned int type_idx);

/* Iterate Cnode::ext[] and aggregate port ranges from all registered ext configs.
 * Updates *out_src / *out_dst to the max across all ext configs found. */
struct Cnode;
void bless_ext_aggregate_port_ranges(const struct Cnode *cnode,
                                     uint32_t *out_src, uint32_t *out_dst);

#endif /* __BLESS_PLUGIN_H__ */
