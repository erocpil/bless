#include "injector_plugin.h"
#include "injector_cfg.h"
#include "config.h"      /* Node, find_by_path */
#include "log.h"

/**
 * @file injector_plugin.c
 * @brief Extension registration tables and generic field parser.
 *
 * Manages two registries:
 *   - Packet-type constructors (injector_pkt_type)
 *   - Extension config parsers (injector_ext_cfg)
 *
 * New protocols register via __attribute__((constructor)) in their
 * proto_*.c file at static-init time.
 */

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <arpa/inet.h>

/* Packet-type registration tables (indexed by type_idx) */
static const struct injector_pkt_type *pkt_types[INJECTOR_MAX_TYPES];
static int32_t type_weights[INJECTOR_MAX_TYPES];  /* 0 = disabled */
static unsigned int next_ext_idx = INJECTOR_NUM_BUILTIN;

/* Config-parser registration table */
#define MAX_CFG_PARSERS 16

static const struct injector_ext_cfg *cfg_parsers[MAX_CFG_PARSERS];
static unsigned int n_cfg_parsers;

/* Packet-type API */
unsigned int injector_register_pkt_type(const struct injector_pkt_type *type)
{
    if (!type) return UINT_MAX;

    unsigned int idx = type->type_idx;

    /* Auto-assign for ext protocols */
    if (idx == INJECTOR_AUTO_IDX) {
        if (next_ext_idx >= INJECTOR_MAX_TYPES) {
            LOG_ERR("injector_register_pkt_type: too many types (max %u)", INJECTOR_MAX_TYPES);
            return UINT_MAX;
        }
        idx = next_ext_idx++;
    }

    if (idx >= INJECTOR_MAX_TYPES) {
        LOG_ERR("injector_register_pkt_type: type_idx %u out of range", idx);
        return UINT_MAX;
    }

    if (pkt_types[idx] != NULL && pkt_types[idx] != type) {
        LOG_ERR("injector_register_pkt_type: type_idx %u already registered (\"%s\")",
                idx, pkt_types[idx]->name);
        return UINT_MAX;
    }

    pkt_types[idx] = type;
    return idx;
}

void injector_set_type_weight(const char *name, int32_t weight)
{
    if (!name) return;
    for (unsigned int i = 0; i < INJECTOR_MAX_TYPES; i++) {
        if (pkt_types[i] && pkt_types[i]->name &&
            strcmp(pkt_types[i]->name, name) == 0) {
            type_weights[i] = weight;
            LOG_DEBUG("set weight %d for type \"%s\" (idx %u)", weight, name, i);
            return;
        }
    }
    LOG_WARN("injector_set_type_weight: type \"%s\" not registered", name);
}

int32_t injector_get_type_weight(const char *name)
{
    if (!name) return 0;
    for (unsigned int i = 0; i < INJECTOR_MAX_TYPES; i++) {
        if (pkt_types[i] && pkt_types[i]->name &&
            strcmp(pkt_types[i]->name, name) == 0) {
            return type_weights[i];
        }
    }
    return 0;
}

const struct injector_pkt_type *injector_find_pkt_type(const char *name)
{
    if (!name) return NULL;
    for (unsigned int i = 0; i < INJECTOR_MAX_TYPES; i++) {
        if (pkt_types[i] && pkt_types[i]->name &&
            strcmp(pkt_types[i]->name, name) == 0)
            return pkt_types[i];
    }
    return NULL;
}

void injector_foreach_pkt_type(void (*fn)(const struct injector_pkt_type *t,
                                       void *ctx), void *ctx)
{
    if (!fn) return;
    for (unsigned int i = 0; i < INJECTOR_MAX_TYPES; i++) {
        if (pkt_types[i])
            fn(pkt_types[i], ctx);
    }
}

injector_pkt_ctor_t injector_get_ctor(unsigned int type_idx)
{
    if (type_idx >= INJECTOR_MAX_TYPES)
        return NULL;
    const struct injector_pkt_type *pt = pkt_types[type_idx];
    return pt ? pt->construct : NULL;
}

const char *injector_get_type_name(unsigned int type_idx)
{
    if (type_idx >= INJECTOR_MAX_TYPES || !pkt_types[type_idx])
        return "unknown";
    return pkt_types[type_idx]->name;
}

/* Config-parser API */
void injector_register_cfg_parser(const struct injector_ext_cfg *ext)
{
    if (!ext || !ext->name || !ext->fields || n_cfg_parsers >= MAX_CFG_PARSERS)
        return;
    cfg_parsers[n_cfg_parsers++] = ext;
}

const struct injector_ext_cfg *injector_find_cfg_parser(const char *name)
{
    if (!name) return NULL;
    for (unsigned int i = 0; i < n_cfg_parsers; i++) {
        if (cfg_parsers[i]->name &&
            strcmp(cfg_parsers[i]->name, name) == 0)
            return cfg_parsers[i];
    }
    return NULL;
}

void injector_foreach_cfg_parser(void (*fn)(const struct injector_ext_cfg *ext,
                                         void *ctx), void *ctx)
{
    if (!fn) return;
    for (unsigned int i = 0; i < n_cfg_parsers; i++)
        fn(cfg_parsers[i], ctx);
}

/* Generic field parsing */
static int parse_uint16_field(const char *s, uint16_t *out)
{
    if (!s || !*s || !out) return -1;
    char *endptr;
    errno = 0;
    unsigned long val = strtoul(s, &endptr, 0);
    if (errno || *endptr || val > UINT16_MAX)
        return -1;
    *out = (uint16_t)val;
    return 0;
}

static int parse_uint32_field(const char *s, uint32_t *out)
{
    if (!s || !*s || !out) return -1;
    char *endptr;
    errno = 0;
    unsigned long long val = strtoull(s, &endptr, 0);
    if (errno || *endptr || val > UINT32_MAX)
        return -1;
    *out = (uint32_t)val;
    return 0;
}

/* Parse "base+count" port-range syntax into {port[0], *range}. */
static int parse_port_range_field(const char *s, uint16_t *port, int32_t *range)
{
    if (!s || !*s) return -1;
    char *copy = strdup(s);
    if (!copy) return -1;

    int i = 0;
    while (copy[i] && copy[i] != '+') i++;

    if (copy[i] == '+') {
        copy[i] = '\0';
        if (parse_uint16_field(copy, port) < 0 ||
            parse_uint16_field(&copy[i + 1], (uint16_t*)range) < 0) {
            free(copy);
            return -1;
        }
    } else {
        if (parse_uint16_field(copy, port) < 0) {
            free(copy);
            return -1;
        }
        *range = 0;
    }
    free(copy);
    return 0;
}

static int parse_ip_range_field(const char *s, uint32_t *ip, int64_t *range)
{
    if (!s || !*s) return -1;
    char *copy = strdup(s);
    if (!copy) return -1;

    int i = 0;
    while (copy[i] && copy[i] != '+') i++;

    if (copy[i] == '+') {
        copy[i] = '\0';
        char *endptr;
        errno = 0;
        unsigned long r = strtoul(&copy[i + 1], &endptr, 0);
        if (errno || *endptr) {
            free(copy);
            return -1;
        }
        *range = (int64_t)r;
    } else {
        *range = 0;
    }

    if (inet_pton(AF_INET, copy, ip) != 1) {
        free(copy);
        return -1;
    }
    free(copy);
    return 0;
}

int injector_parse_cfg_fields(const struct injector_cfg_field *fields,
                           void *cfg, void *yaml_node_v)
{
    if (!fields || !cfg || !yaml_node_v)
        return -1;

    Node *yaml_node = (Node *)yaml_node_v;

    for (const struct injector_cfg_field *f = fields; f->yaml_path; f++) {
        Node *val = find_by_path(yaml_node, f->yaml_path);
        if (!val)
            continue;  /* optional field -- skip */

        void *field_ptr = (char *)cfg + f->offset;

        switch (f->type) {
        case INJECTOR_CFG_TOGGLE:
            if (val->type == NODE_SCALAR && val->value) {
                int on = (strcmp(val->value, "true") == 0 ||
                          strcmp(val->value, "TRUE") == 0);
                *(uint8_t *)field_ptr = (uint8_t)on;
            }
            break;

        case INJECTOR_CFG_UINT16:
            if (val->type == NODE_SCALAR && val->value)
                parse_uint16_field(val->value, (uint16_t *)field_ptr);
            break;

        case INJECTOR_CFG_UINT32:
            if (val->type == NODE_SCALAR && val->value)
                parse_uint32_field(val->value, (uint32_t *)field_ptr);
            break;

        case INJECTOR_CFG_IPV4: {
            if (val->type != NODE_SCALAR || !val->value)
                break;
            int64_t range = 0;
            uint32_t ip;
            if (parse_ip_range_field(val->value, &ip, &range) == 0) {
                *(uint32_t *)field_ptr = ip;
                if (f->offset_count) {
                    /* Store range at offset_count, not count (range mode) */
                    *(int64_t *)((char *)cfg + f->offset_count) = range;
                }
            }
            break;
        }

        case INJECTOR_CFG_PORT_RANGE: {
            if (val->type != NODE_SCALAR || !val->value)
                break;
            int32_t range = 0;
            uint16_t port;
            if (parse_port_range_field(val->value, &port, &range) == 0) {
                *(uint16_t *)field_ptr = port;
                if (f->offset_count) {
                    *(int32_t *)((char *)cfg + f->offset_count) = range;
                }
            }
            break;
        }

        case INJECTOR_CFG_UINT16_ARRAY:
            if (val->type == NODE_SEQUENCE) {
                uint16_t n = 0;
                for (Node *child = val->child; child && n < f->limit;
                     child = child->next, n++) {
                    parse_uint16_field(child->value,
                        &((uint16_t *)field_ptr)[n]);
                }
                if (f->offset_count)
                    *(uint16_t *)((char *)cfg + f->offset_count) = n;
            } else if (val->type == NODE_SCALAR && val->value) {
                if (parse_uint16_field(val->value, (uint16_t *)field_ptr) == 0)
                    if (f->offset_count)
                        *(uint16_t *)((char *)cfg + f->offset_count) = 1;
            }
            break;

        case INJECTOR_CFG_UINT32_ARRAY:
            if (val->type == NODE_SEQUENCE) {
                uint16_t n = 0;
                for (Node *child = val->child; child && n < f->limit;
                     child = child->next, n++) {
                    parse_uint32_field(child->value,
                        &((uint32_t *)field_ptr)[n]);
                }
                if (f->offset_count)
                    *(uint16_t *)((char *)cfg + f->offset_count) = n;
            } else if (val->type == NODE_SCALAR && val->value) {
                if (parse_uint32_field(val->value, (uint32_t *)field_ptr) == 0)
                    if (f->offset_count)
                        *(uint16_t *)((char *)cfg + f->offset_count) = 1;
            }
            break;

        case INJECTOR_CFG_IPV4_ARRAY:
            if (val->type == NODE_SEQUENCE) {
                uint16_t n = 0;
                for (Node *child = val->child; child && n < f->limit;
                     child = child->next, n++) {
                    inet_pton(AF_INET, child->value,
                              &((uint32_t *)field_ptr)[n]);
                }
                if (f->offset_count)
                    *(uint16_t *)((char *)cfg + f->offset_count) = n;
            } else if (val->type == NODE_SCALAR && val->value) {
                inet_pton(AF_INET, val->value, (uint32_t *)field_ptr);
                if (f->offset_count)
                    *(uint16_t *)((char *)cfg + f->offset_count) = 1;
            }
            break;

        case INJECTOR_CFG_STRING:
            if (val->value && strlen(val->value)) {
                char *payload = strdup(val->value);
                *(char **)field_ptr = payload;
                if (f->offset_count)
                    *(uint16_t *)((char *)cfg + f->offset_count) =
                        (uint16_t)(strlen(payload) + 1);
            }
            break;
        }
    }

    return 0;
}
