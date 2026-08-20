#include "bless.h"
#include "bless_cfg.h"
#include "bless_plugin.h"
#include "quic.h"
#include <string.h>

/**
 * @file proto_quic.c
 * @brief QUIC v1 Initial packet constructor (RFC 9000).
 *
 * Produces valid QUIC Initial packets with a minimal TLS ClientHello
 * carried in a CRYPTO frame.  Runs over UDP (ether_type=IPv4, ip_proto=17).
 *
 * Registered as a bless extension plugin -- its YAML config fields are
 * parsed by the generic bless_cfg framework.
 */

/* QUIC wire constants */
#define QUIC_VERSION_V1      0x00000001
#define QUIC_LONG_HEADER     0xC0
#define QUIC_INITIAL_PKT     0x00
#define QUIC_CRYPTO_FRAME    0x06
#define QUIC_DEFAULT_DCID_LEN 8
#define QUIC_DEFAULT_SCID_LEN 8

/* Minimal TLS ClientHello for a valid Initial packet (Wireshark-parsable).
 * All length fields are pre-computed -- no runtime fixup needed. */
static const uint8_t tls_client_hello[] = {
    0x16, 0x03, 0x01,             /* TLS record: handshake, version 1.0 */
    0x00, 0x78,                   /* record length = 120 */
    0x01,                         /* msg_type = ClientHello */
    0x00, 0x00, 0x74,             /* CH length = 116 */
    0x03, 0x03,                   /* legacy_version = TLS 1.2 */
    /* 32 bytes random */
    0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
    0x00,                         /* session_id length = 0 */
    0x00, 0x02, 0x13, 0x01,       /* cipher suites: TLS_AES_128_GCM_SHA256 */
    0x01, 0x00,                   /* compression: null */
    0x00, 0x49,                   /* extensions length = 73 */
    /* server_name ext  (20 bytes + 2-byte type = 22) */
    0x00, 0x00, 0x00, 0x14,       /* type=SNI, ext_len=20 */
    0x00, 0x12,                   /* list_len=18 */
    0x00, 0x00, 0x0B,             /* name_type=host, name_len=11 */
    'b','l','e','s','s','.','l','o','c','a','l',
    /* supported_versions ext  (7 bytes + 2-byte type = 9) */
    0x00, 0x2B, 0x00, 0x03,       /* type, ext_len=3 */
    0x02, 0x03, 0x04,             /* vers_len=2, TLS 1.3 */
    /* key_share ext (x25519)  (42 bytes + 2-byte type = 44) */
    0x00, 0x33, 0x00, 0x26,       /* type, ext_len=38 */
    0x00, 0x24,                   /* shares_len=36 */
    0x00, 0x1D, 0x00, 0x20,       /* group=x25519, key_len=32 */
    0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,  /* 32 zero key */
    0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
    /* quic_transport_parameters ext  (4 bytes + 2-byte type = 6) */
    0x00, 0x39, 0x00, 0x00,       /* type, ext_len=0 */
};

#define TLS_CH_SIZE sizeof(tls_client_hello)

/* QUIC variable-length integer encoding.  Returns byte count (1/2/4/8). */
static int quic_encode_varint(uint8_t *out, uint64_t val)
{
    if (val <= 63)            { out[0] = (uint8_t)val; return 1; }
    if (val <= 16383)         { out[0] = (uint8_t)(0x40|(val>>8));
                                out[1] = (uint8_t)val; return 2; }
    if (val <= 1073741823)    { out[0] = (uint8_t)(0x80|(val>>24));
                                out[1] = (uint8_t)(val>>16);
                                out[2] = (uint8_t)(val>>8);
                                out[3] = (uint8_t)val; return 4; }
    out[0] = (uint8_t)(0xC0|(val>>56)); out[1] = (uint8_t)(val>>48);
    out[2] = (uint8_t)(val>>40); out[3] = (uint8_t)(val>>32);
    out[4] = (uint8_t)(val>>24); out[5] = (uint8_t)(val>>16);
    out[6] = (uint8_t)(val>>8);  out[7] = (uint8_t)val;
    return 8;
}

/* Build QUIC Initial payload.  Returns bytes written. */
static uint16_t quic_build_initial(uint8_t *quic, uint16_t max_len,
                                    const struct quic_ext_cfg *cfg)
{
    uint8_t *p = quic;
    uint8_t dcid_len = cfg ? cfg->dcid_len : QUIC_DEFAULT_DCID_LEN;
    uint8_t scid_len = cfg ? cfg->scid_len : QUIC_DEFAULT_SCID_LEN;
    uint32_t version = (cfg && cfg->version) ? cfg->version : QUIC_VERSION_V1;

    if (!dcid_len) {
        dcid_len = QUIC_DEFAULT_DCID_LEN;
    }
    if (!scid_len) {
        scid_len = QUIC_DEFAULT_SCID_LEN;
    }
    if (dcid_len > 20) {
        dcid_len = 20;
    }
    if (scid_len > 20) {
        scid_len = 20;
    }

    /* Long header */
    *p++ = QUIC_LONG_HEADER | QUIC_INITIAL_PKT;
    *p++ = (uint8_t)(version >> 24);
    *p++ = (uint8_t)(version >> 16);
    *p++ = (uint8_t)(version >> 8);
    *p++ = (uint8_t)version;

    /* DCID */
    *p++ = dcid_len;
    for (int i = 0; i < dcid_len; i++)
        *p++ = (uint8_t)((i * 0x2A + 0x83) & 0xFF);

    /* SCID */
    *p++ = scid_len;
    for (int i = 0; i < scid_len; i++)
        *p++ = (uint8_t)((i * 0x5F + 0x11) & 0xFF);

    /* Token: absent */
    *p++ = 0;

    /* Length (varint, filled later) */
    uint8_t *len_pos = p;
    p += 2;

    /* CRYPTO frame */
    *p++ = QUIC_CRYPTO_FRAME;      /* frame type */
    *p++ = 0;                      /* offset = 0 */
    uint8_t *crypto_len_pos = p;
    p += 2;                        /* length varint placeholder */

    /* TLS ClientHello */
    uint8_t tls[TLS_CH_SIZE];
    memcpy(tls, tls_client_hello, TLS_CH_SIZE);

    uint16_t tls_bytes = TLS_CH_SIZE;
    if ((uint16_t)(p - quic) + tls_bytes > max_len) {
        tls_bytes = max_len - (uint16_t)(p - quic);
    }
    memcpy(p, tls, tls_bytes);
    p += tls_bytes;

    /* Fixup CRYPTO length */
    uint16_t crypto_sz = (uint16_t)(p - (crypto_len_pos + 2));
    quic_encode_varint(crypto_len_pos, crypto_sz);

    /* Fixup QUIC payload length */
    uint64_t quic_payload = p - (len_pos + 2);
    quic_encode_varint(len_pos, quic_payload);

    return (uint16_t)(p - quic);
}

/* Extension config (fields parsed from YAML) */
static const struct bless_cfg_field quic_cfg_fields[] = {
    { "dcid-len", BLESS_CFG_UINT16, offsetof(struct quic_ext_cfg, dcid_len),
      0, 20 },
    { "scid-len", BLESS_CFG_UINT16, offsetof(struct quic_ext_cfg, scid_len),
      0, 20 },
    { "version",  BLESS_CFG_UINT32, offsetof(struct quic_ext_cfg, version),
      0, 0 },
    { NULL },
};

static void quic_free_cfg(void *cfg_v) { (void)cfg_v; }

static int quic_clone_cfg(const void *src_v, void *dst_v)
{
    memcpy(dst_v, src_v, sizeof(struct quic_ext_cfg));
    return 0;
}

static void __attribute__((constructor)) register_quic_cfg(void)
{
    static const struct bless_ext_cfg ext = {
        .name      = "quic",
        .cfg_size  = sizeof(struct quic_ext_cfg),
        .fields    = quic_cfg_fields,
        .yaml_path = "quic",
        .free_cfg  = quic_free_cfg,
        .clone_cfg = quic_clone_cfg,
        .port_range = NULL,
    };
    bless_register_cfg_parser(&ext);
}

/* Helper: find QUIC config from Cnode */
struct quic_ext_cfg *quic_ext_find(Cnode *cnode)
{
    if (!cnode) {
        return NULL;
    }
    for (int i = 0; i < (int)cnode->n_ext; i++) {
        if (cnode->ext[i].desc &&
            !strcmp(cnode->ext[i].desc->name, "quic")) {
            return (struct quic_ext_cfg *)cnode->ext[i].cfg;
        }
    }
    return NULL;
}

/* Public constructor */
uint64_t bless_mbufs_quic(struct rte_mbuf **mbufs, unsigned int n, void *data)
{
    Cnode *cnode = (Cnode *)data;
    struct quic_ext_cfg *qc = quic_ext_find(cnode);

    const uint16_t l2 = sizeof(struct rte_ether_hdr);
    const uint16_t l3 = sizeof(struct rte_ipv4_hdr);
    const uint16_t l4 = sizeof(struct rte_udp_hdr);

    uint16_t max_quic = 1200;
    {
        uint16_t mtu = cnode->ether.mtu;
        if (cnode->vxlan.enable && cnode->vxlan.wire_mtu) {
            uint16_t vm = cnode->vxlan.wire_mtu;
            if (vm > BLESS_SIZEOF_VXLAN + l2) {
                vm -= BLESS_SIZEOF_VXLAN + l2;
            } else {
                vm = 0;
            }
            if (vm && (!mtu || vm < mtu)) {
                mtu = vm;
            }
        }
        if (mtu) {
            uint16_t a = mtu - l3 - l4;
            if (a < max_quic) {
                max_quic = a;
            }
        }
    }

    uint64_t tx_bytes = 0;

    for (unsigned int i = 0; i < n; i++) {
        struct rte_mbuf *m = mbufs[i];
        rte_pktmbuf_reset(m);

        struct rte_ether_hdr *eth = (void *)rte_pktmbuf_append(m, l2);
        struct rte_ipv4_hdr  *ip  = (void *)rte_pktmbuf_append(m, l3);
        struct rte_udp_hdr   *udp = (void *)rte_pktmbuf_append(m, l4);
        if (!eth || !ip || !udp) {
            goto drop;
        }

        /* Ethernet */
        memcpy(&eth->dst_addr, cnode->ether.dst, RTE_ETHER_ADDR_LEN);
        memcpy(&eth->src_addr, cnode->ether.src, RTE_ETHER_ADDR_LEN);
        eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

        /* IPv4 */
        memset(ip, 0, l3);
        ip->version_ihl  = 0x45;
        ip->time_to_live = 64;
        ip->next_proto_id = IPPROTO_UDP;
        ip->src_addr = RANDOM_IP_SRC(cnode);
        ip->dst_addr = RANDOM_IP_DST(cnode);

        /* UDP header */
        udp->src_port = RANDOM_UDP_SRC(cnode);
        udp->dst_port = RANDOM_UDP_DST(cnode);

        /* QUIC payload */
        uint8_t *quic = (uint8_t *)rte_pktmbuf_append(m, max_quic);
        if (!quic) {
            goto drop;
        }

        uint16_t qlen = quic_build_initial(quic, max_quic, qc);
        if (!qlen) {
            goto drop;
        }

        if (qlen < max_quic) {
            rte_pktmbuf_trim(m, max_quic - qlen);
        }

        uint16_t total = l3 + l4 + qlen;
        ip->total_length = rte_cpu_to_be_16(total);
        udp->dgram_len   = rte_cpu_to_be_16(l4 + qlen);

        m->ol_flags |= RTE_MBUF_F_TX_IPV4
                    |  RTE_MBUF_F_TX_IP_CKSUM
                    |  RTE_MBUF_F_TX_UDP_CKSUM;
        m->l2_len = l2; m->l3_len = l3; m->l4_len = l4;
        tx_bytes += total;
        continue;

drop:
        rte_pktmbuf_free(m);
    }

    return tx_bytes;
}

/* Plugin registration */
static void __attribute__((constructor)) register_quic_pkt(void)
{
    static const struct bless_pkt_type t = {
        .name       = "quic",
        .ether_type = RTE_ETHER_TYPE_IPV4,
        .ip_proto   = IPPROTO_UDP,
        .type_idx   = BLESS_AUTO_IDX,
        .construct  = bless_mbufs_quic,
    };
    bless_register_pkt_type(&t);
}

/* Erroneous mutations */
/**
 * Offsets into the QUIC payload (past Eth + IPv4 + UDP = 42 bytes).
 * M = l2_len + l3_len + l4_len  (typically 14 + 20 + 8 = 42).
 *
 * Layout from M:
 *   [0]    flags (0xC0 | type)
 *   [1:4]  version (4 B, big-endian)
 *   [5]    DCID len
 *   [6]    DCID
 *   [6+dcid_len]       SCID len
 *   [7+dcid_len]       SCID
 *   [7+dcid_len+scid_len]  Token len
 */

/** Replace the QUIC version field with a non-v1 value to trigger
 *  version-negotiation paths on the gateway. */
uint64_t mutation_quic_version(void **mbufs, unsigned int n, void *data)
{
    (void)data;
    uint64_t ok = 0;

    for (unsigned int i = 0; i < n; i++) {
        struct rte_mbuf *m = mbufs[i];
        uint16_t off = m->l2_len + m->l3_len + m->l4_len;
        if (off + 5 > m->pkt_len) {
            continue;
        }

        uint8_t *quic = rte_pktmbuf_mtod(m, uint8_t *) + off;

        /* Verify long-header bit */
        if ((quic[0] & 0x80) == 0) {
            continue;
        }

        /* Write a non-v1 version (0xDEADBEEF or v2-draft 0x709A50C4) */
        uint32_t bad = 0xDEADBEEF;
        quic[1] = (uint8_t)(bad >> 24);
        quic[2] = (uint8_t)(bad >> 16);
        quic[3] = (uint8_t)(bad >> 8);
        quic[4] = (uint8_t)bad;
        ok++;
    }
    return ok;
}

/** Set DCID or SCID length to an invalid value (> 20, RFC 9000 cap). */
uint64_t mutation_quic_cid_len(void **mbufs, unsigned int n, void *data)
{
    (void)data;
    uint64_t ok = 0;
    uint64_t rnd = fast_rand_next();

    for (unsigned int i = 0; i < n; i++) {
        struct rte_mbuf *m = mbufs[i];
        uint16_t off = m->l2_len + m->l3_len + m->l4_len;
        if (off + 7 > m->pkt_len) {
            continue; /* need at least flags + version + dcid_len + scid_len */
        }

        uint8_t *quic = rte_pktmbuf_mtod(m, uint8_t *) + off;
        if ((quic[0] & 0x80) == 0) {
            continue;
        }

        /* Corrupt either DCID or SCID length (alternating) */
        if ((rnd ^ (rnd >> 4)) & 1) {
            quic[5] = 0xFF;  /* DCID len = 255 (> 20) */
        } else {
            uint8_t dcid_len = quic[5];
            if (dcid_len > 20) {
                dcid_len = 20;
            }
            uint8_t *scid_len = &quic[6 + dcid_len];
            *scid_len = 0xFF;  /* SCID len = 255 */
        }
        rnd = fast_rand_next();
        ok++;
    }
    return ok;
}

/** Inject a non-empty Token field (normally 0-length in Initial packets).
 *  Gateways must either drop or handle token validation. */
uint64_t mutation_quic_token_len(void **mbufs, unsigned int n, void *data)
{
    (void)data;
    uint64_t ok = 0;

    for (unsigned int i = 0; i < n; i++) {
        struct rte_mbuf *m = mbufs[i];
        uint16_t off = m->l2_len + m->l3_len + m->l4_len;
        if (off + 8 > m->pkt_len) {
            continue;
        }

        uint8_t *quic = rte_pktmbuf_mtod(m, uint8_t *) + off;
        if ((quic[0] & 0x80) == 0) {
            continue;
        }

        uint8_t dcid_len = quic[5];
        if (dcid_len > 20) {
            continue; /* already corrupted */
        }
        uint8_t scid_len = quic[6 + dcid_len];
        if (scid_len > 20) {
            continue;
        }

        /* Token length is at offset 7 + dcid_len + scid_len */
        uint16_t token_off = 7 + dcid_len + scid_len;
        if (token_off >= m->pkt_len - off) {
            continue;
        }

        /* Set a non-zero token length (32 bytes) -- the token data
         * that follows doesn't exist in this packet, so the length
         * field alone will cause parse errors downstream. */
        quic[token_off] = 32;
        ok++;
    }
    return ok;
}

/* struct Mutator is defined in mutation.h -- redeclare here to avoid
 * including that header (which contains non-static function definitions). */
struct Mutator {
    char name[32];
    mutation_func func;
};

struct Mutator quic_mutators[] = {
    { "version",   mutation_quic_version },
    { "cid_len",   mutation_quic_cid_len },
    { "token_len", mutation_quic_token_len },
};
