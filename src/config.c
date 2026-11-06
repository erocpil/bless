#include "bless.h"
#include "server.h"
#include "config.h"
#include "config_bless_internal.h"
#include "define.h"
#include "log.h"

#include <string.h>

/* HW offload types configurable via YAML ``hw-offload:`` list.
 * Not exhaustive -- covers the most commonly-needed per-packet offload toggles. */
static struct offload_table_item offload_table[] = {
	{ "ipv4", OF_IPV4 },
	{ "ipv6", OF_IPV6 },
	{ "tcp", OF_TCP },
	{ "udp", OF_UDP },
	{ "outer-ipv4", OF_OUTER_IPV4 },
	{ "outer-udp", OF_OUTER_UDP },
	{ "sctp", OF_SCTP },
};

/* Parse the ``dpdk`` YAML section into DPDK EAL argv array.
 *
 * Handles -l/--lcores/-c, --socket-mem, --huge-dir, PCI whitelist
 * and other DPDK-specific EAL parameters. */
static int
config_parse_dpdk_internal(Node *node, int *targc, char ***targv, int i,
		int capacity)
{
	char **argv = *targv;

	for (Node *n = node->child; n != NULL; n = n->next) {
		if (i >= capacity) {
			LOG_ERR("too many DPDK/injector arguments (maximum %d)", capacity);
			return -1;
		}
		if (n->type == NODE_SCALAR) {
			if (!n->value || !strcmp(n->value, "null") ||
					!strcmp(n->value, "NULL")) {
				continue;
			}

			size_t len;
			if (strlen(n->key) == 1) {
				// "-k v"
				len = 1 + 1 + strlen(n->key) + 1 + strlen(n->value) + 1;
			} else {
				// "--key=value"
				len = 2 + strlen(n->key) + 1 + strlen(n->value) + 1;
			}

			argv[i] = malloc(len);
			if (!argv[i]) {
				LOG_ERR("cannot allocate DPDK argument");
				return -1;
			}

			if (strlen(n->key) == 1) {
				if (strlen(n->value)) {
					snprintf(argv[i], len, "-%s %s", n->key, n->value);
				} else {
					snprintf(argv[i], len, "-%s", n->key);
				}
			} else {
				if (strlen(n->value)) {
					snprintf(argv[i], len, "--%s=%s", n->key, n->value);
				} else {
					snprintf(argv[i], len, "--%s", n->key);
				}
			}

		} else if (n->type == NODE_SEQUENCE) {
			size_t len;
			if (strlen(n->key) == 1) {
				// "-k "
				len = 1 + 1 + strlen(n->key) + 1;
			} else {
				// "--key="
				len = 2 + strlen(n->key) + 1;
			}

			Node *t = n->child;
			while (t) {
				// ',' or ' '
				len += strlen(t->value) + 1;
				t = t->next;
			}
			len += 1; // '\0'

			t = n->child;
			while (t) {
				if (i >= capacity) {
					LOG_ERR("too many DPDK/injector arguments (maximum %d)",
						capacity);
					return -1;
				}
				argv[i] = malloc(len);
				if (!argv[i]) {
					LOG_ERR("malloc(%s)", t->value);
					return -1;
				}

				char *p = argv[i];
				if (strlen(n->key) == 1) {
					p += sprintf(p, "-%s ", n->key);
				} else {
					p += sprintf(p, "--%s=", n->key);
				}

				p += sprintf(p, "%s", t->value);
				if (t->next) {
					// *p++ = ',';
				}
				t = t->next;
				*p = '\0';
				i += !!t;
			}
		} else {
			// NODE_MAPPING
			LOG_WARN("Omitted %s", n->value);
		}
		i++;
	}

	*targc = i;
	*targv = argv;

	return i;
}

/* Parse the ``system`` YAML section into system configuration defaults. */
int config_parse_system(Node *root, struct system_cfg *cfg)
{
	system_set_defaults(cfg);

	if (!cfg) {
		return -1;
	}

	char *path = "system";
	Node *system_node = find_by_path(root, path);
	if (!system_node) {
		printf("No system found\n");
		return -1;
	}

	path = "daemonize";
	Node *node = find_by_path(system_node, path);
	if (node) {
		if (NODE_SCALAR == node->type && node->value && 0 ==
				strcmp(node->value, "true")) {
			cfg->daemonize = 1;
		}
	}
	LOG_INFO("system daemonize: %s", cfg->daemonize ? "yes" : "no");

	path = "theme";
	node = find_by_path(system_node, path);
	if (node) {
		if (NODE_SCALAR == node->type && node->value &&
				strlen(node->value) < SYSTEM_THEME_LEN_MAX) {
			strncpy(cfg->theme, node->value, SYSTEM_THEME_LEN_MAX);
		} else {
			LOG_ERR("invalid theme name: %s",
				node->value ? node->value : "(null)");
			return -1;
		}
	} else {
		strncpy(cfg->theme, "default", strlen("default") + 1);
	}

	if (config_parse_server(system_node, &cfg->server) < 0) {
		LOG_ERR("invalid server arguments");
		return -1;
	}

	return 0;
}

/* Parse the ``server`` YAML section into a server options struct.
 * Handles port, SSL cert path, threads, document root, CORS, etc. */
int config_parse_server(Node *root, struct server *srv)
{
	if (!srv) {
		LOG_WARN("NULL server");
		return -1;
	}

	/* 1. defaults */
	server_options_set_defaults(&srv->cfg);
	srv->enable               = 1;  /* server starts by default */
	srv->control_enable       = 0;  /* fail closed: WS commands opt-in */
	srv->remote_control_enable = 0;  /* fail closed: remote requires opt-in */

	struct server_options_cfg *cfg = &srv->cfg;

	char *path = "server";
	Node *node = find_by_path(root, path);
	if (!node) {
		LOG_WARN("No server found");
		return -1;
	}
	root = node;

	/* 1b. server-level booleans (fail-closed defaults) */
	path = "enable";
	node = find_by_path(root, path);
	if (node && NODE_SCALAR == node->type) {
		SERVER_PARSE_BOOL(srv->enable, node->value);
	}

	path = "control_enable";
	node = find_by_path(root, path);
	if (node && NODE_SCALAR == node->type) {
		SERVER_PARSE_BOOL(srv->control_enable, node->value);
	}

	path = "remote_control_enable";
	node = find_by_path(root, path);
	if (node && NODE_SCALAR == node->type) {
		SERVER_PARSE_BOOL(srv->remote_control_enable, node->value);
	}

	/* 2. YAML override */
	path = "options";
	node = find_by_path(root, path);
	if (node) {
		if (NODE_MAPPING == node->type) {
			for (Node *n = node->child; n; n = n->next) {
				const char *k = n->key, *v = n->value;
				if (strlen(n->key) > SERVER_KV_MAX ||
						strlen(n->value) > SERVER_KV_MAX) {
					printf("String too long %s => %s\n, omit",
							n->key, n->value);
					continue;
				}
#define X(name, type, civet_key, def)        \
				if (strcmp(k, civet_key) == 0) { \
					SERVER_PARSE_##type(cfg->name, v); \
					continue; \
				}

#include "server_options.def"
#undef X

			}
		} else {
			cfg->civet_opts[0] = NULL;
			LOG_WARN("No valid server options found, use default");
		}
	} else {
		LOG_WARN("No server options found, use default");
	}

	/* 3. cfg -> kv[] */
	/* One entry per option */
	int n = build_civet_options(cfg, cfg->kv, 16);
	/* 4. kv[] -> civet const char * */
	int i = 0;
	for (i = 0; i < n; i++) {
		cfg->civet_opts[i * 2]     = cfg->kv[i].key;
		cfg->civet_opts[i * 2 + 1] = cfg->kv[i].val;
	}
	cfg->civet_opts[i * 2] = NULL;

	struct server_service *svc = &srv->svc;
	path = "service";
	node = find_by_path(root, path);
	if (node) {
		path = "service.websocket";
		node = find_by_path(root, path);
		if (node && NODE_SCALAR == node->type) {
			int len = strlen(node->value);
			if (len > 0) {
				svc->websocket_uri = (char*)malloc(len + 1);
				strncpy(svc->websocket_uri, node->value, len + 1);
			} else {
				svc->websocket_uri = NULL;
			}
		}
		path = "service.http";
		node = find_by_path(root, path);
		if (node && NODE_SEQUENCE == node->type) {
			int i = 0;
			for (Node *n = node->child; n && i < SERVER_SERVICE_HTTP_MAX;
					n = n->next, i++) {
				int len = strlen(n->value);
				if (len > 0 && len < SERVER_SERVICE_HTTP_LEN_MAX) {
					strncpy(svc->http[i], n->value,
							SERVER_SERVICE_HTTP_LEN_MAX);
					svc->n_http++;
				} else {
					LOG_ERR("Server service length error");
					return -1;
				}
			}
		}
	} else {
		LOG_WARN("No server service found, use default");
	}

	return 0;
}

/* Top-level entry: parse the DPDK config section and return EAL argc/argv.
 * Returns the number of arguments parsed. */
int config_parse_dpdk(Node *root, int *targc, char ***targv)
{
#define MAX_EAL_PARAMS 128
	if (!root || !targc || !targv || !*targv || !(*targv)[0]) {
		return -1;
	}

	char *path = "dpdk";
	Node *node = find_by_path(root, path);
	if (!node) {
		return -1;
	}

	int argc = 1;
	char **argv = calloc(MAX_EAL_PARAMS + 1, sizeof(*argv));
	if (!argv) {
		LOG_ERR("cannot allocate DPDK argument vector");
		return -1;
	}

	int i = 0;
	size_t len = strlen((*targv)[i]) + 1;
	argv[i] = malloc(len);
	if (!argv[i]) {
		LOG_ERR("cannot allocate DPDK argv[0]");
		goto error;
	}
	strcpy(argv[i], (*targv)[i]);

	i = config_parse_dpdk_internal(node, &argc, &argv, ++i,
		MAX_EAL_PARAMS);
	if (i < 0) {
		goto error;
	}

	// Add separator --
	if (i >= MAX_EAL_PARAMS) {
		LOG_ERR("too many DPDK arguments (maximum %d)", MAX_EAL_PARAMS);
		goto error;
	}
	argv[i] = strdup("--");
	if (!argv[i]) {
		LOG_ERR("cannot allocate DPDK/injector separator");
		goto error;
	}

	path = "injector";
	node = find_by_path(root, path);
	if (!node) {
		LOG_ERR("no injector config found");
		goto error;
	}

	i = config_parse_dpdk_internal(node, &argc, &argv, ++i,
		MAX_EAL_PARAMS);
	if (i < 0) {
		goto error;
	}

	path = "bless";
	node = find_by_path(root, path);
	if (!node) {
		LOG_ERR("no bless config found");
		goto error;
	}

	*targc = argc;
	*targv = argv;
	(*targv)[*targc] = NULL;

	return 0;

error:
	for (int j = 0; j <= MAX_EAL_PARAMS; j++)
		free(argv[j]);
	free(argv);
	return -1;
}

/* Generic key-value config parser for simple scalar arrays.
 * Used for offload flags and similar flat config sections. */
int config_parse_generic(Node *node, int *targc, char ***targv, int i,
		const char *prefix)
{
	char **argv = *targv;

	for (Node *n = node->child; n != NULL; n = n->next) {
		char fullkey[256];
		if (prefix && strlen(prefix)) {
			snprintf(fullkey, sizeof(fullkey), "%s.%s", prefix, n->key);
		} else {
			snprintf(fullkey, sizeof(fullkey), "%s", n->key);
		}

		if (n->type == NODE_SCALAR) {
			if (!n->value || !strcmp(n->value, "null") ||
					!strcmp(n->value, "NULL")) {
				continue;
			}

			// --key=value
			size_t len = 2 + strlen(fullkey) + 1 + strlen(n->value) + 1;
			argv[i] = malloc(len);
			if (!argv[i]) {
				LOG_ERR("cannot allocate injector argument");
				return -1;
			}
			if (strlen(n->value)) {
				snprintf(argv[i], len, "--%s=%s", fullkey, n->value);
			} else {
				snprintf(argv[i], len, "--%s", fullkey);
			}
			i++;

		} else if (n->type == NODE_SEQUENCE) {
			size_t len = 2 + strlen(fullkey) + 1;
			Node *t = n->child;
			while (t) {
				len += strlen(t->value) + 1;
				t = t->next;
			}
			len += 1;

			argv[i] = malloc(len);
			if (!argv[i]) {
				LOG_ERR("cannot allocate injector sequence argument");
				return -1;
			}

			char *p = argv[i];
			p += sprintf(p, "--%s=", fullkey);
			t = n->child;
			while (t) {
				p += sprintf(p, "%s", t->value);
				if (t->next) {
					*p++ = ',';
				}
				t = t->next;
			}
			*p = '\0';
			i++;

		} else if (n->type == NODE_MAPPING) {
			// recursively
			i = config_parse_generic(n, &i, &argv, i, fullkey);
			if (i < 0) {
				return -1;
			}
		}
	}

	*targc = i;
	*targv = argv;

	return i;
}

/* Parse the ``bless.ether`` section (MAC addresses, MTU, offload). */
static void
config_free_partial_cnode(Cnode *cnode)
{
	if (!cnode) {
		return;
	}
	free(cnode->ether.type.ipv4.icmp.payload);
	free(cnode->ether.type.ipv4.tcp.payload);
	free(cnode->ether.type.ipv4.udp.payload);
	for (int i = 0; i < cnode->erroneous.n_clas; i++) {
		struct ec_clas *clas = &cnode->erroneous.clas[i];
		free(clas->name);
		for (int j = 0; j < clas->n_type; j++)
			free(clas->type[j]);
	}
	free(cnode->erroneous.func);
	for (uint8_t i = 0; i < cnode->n_ext; i++) {
		const struct bless_ext_cfg *desc = cnode->ext[i].desc;
		void *cfg = cnode->ext[i].cfg;
		if (desc && cfg && desc->free_cfg) {
			desc->free_cfg(cfg);
		}
		free(cfg);
	}
	free(cnode);
}

Cnode *config_parse_bless(Node *root)
{
	struct Cnode *cnode = calloc(1, sizeof(struct Cnode));
	if (!cnode) {
		LOG_ERR("cannot allocate bless configuration");
		return NULL;
	}

	Node *bless_node = find_by_path(root, "bless");
	if (!bless_node) {
		goto ERROR;
	}

	Node *ether_node = find_by_path(bless_node, "ether");
	if (!ether_node) {
		goto ERROR;
	}

	/* hw offload */
	Node *node = find_by_path(bless_node, "hw-offload");
	if (node) {
		int n_item = sizeof(offload_table) / sizeof(offload_table[0]);
		for (Node *t = node->child; t; t = t->next) {
			for (int i = 0; i < n_item; i++) {
				if (strcmp(t->value, offload_table[i].name)) {
					continue;
				}
				cnode->offload |= 1 << offload_table[i].type;
				LOG_HINT("matched offload type %s ", t->value);
			}
		}
	}

	/* ether */
	if (config_parse_bless_ether_all(bless_node, ether_node, cnode) < 0 ||
	    config_parse_bless_vxlan(bless_node, cnode) < 0 ||
	    config_parse_bless_erroneous(bless_node, cnode) < 0) {
		goto ERROR;
	}

	/* Parse extension plugin configs (registered via bless_register_cfg_parser) */
	{
		Node *ipv4_node = find_by_path(ether_node, "type.ipv4");
		if (config_parse_bless_plugins(
			ipv4_node ? ipv4_node : ether_node, cnode) < 0) {
			goto ERROR;
		}
	}

	return cnode;

ERROR:
	LOG_ERR("yaml config parse error");
	config_free_partial_cnode(cnode);
	return NULL;
}

/* Deep-copy a Cnode: allocates fresh arrays for all variable-length fields
 * (MAC addresses, IP addresses, port ranges, VNI list, etc.).
 * Returns 0 on success, -1 on allocation failure. */
static void
config_free_clone_allocations(Cnode *cnode)
{
	free(cnode->ether.type.ipv4.icmp.payload);
	free(cnode->ether.type.ipv4.tcp.payload);
	free(cnode->ether.type.ipv4.udp.payload);
	free(cnode->erroneous.func);
	for (uint8_t i = 0; i < cnode->n_ext; i++) {
		const struct bless_ext_cfg *desc = cnode->ext[i].desc;
		void *cfg = cnode->ext[i].cfg;
		if (desc && cfg && desc->free_cfg) {
			desc->free_cfg(cfg);
		}
		free(cfg);
	}
}

int
config_clone_cnode(Cnode *src, Cnode *dst)
{
	if (!src || !dst) {
		return -1;
	}

	Cnode tmp = *src;
	tmp.ether.type.ipv4.icmp.payload = NULL;
	tmp.ether.type.ipv4.tcp.payload = NULL;
	tmp.ether.type.ipv4.udp.payload = NULL;
	tmp.erroneous.func = NULL;
	tmp.n_ext = 0;
	for (uint8_t i = 0; i < BLESS_PLUGIN_MAX; i++)
		tmp.ext[i].cfg = NULL;

	/* icmp payload */
	if (src->ether.type.ipv4.icmp.payload &&
			src->ether.type.ipv4.icmp.payload_len) {
		char *payload = malloc(src->ether.type.ipv4.icmp.payload_len);
		if (!payload) {
			goto error;
		}
		memcpy(payload, src->ether.type.ipv4.icmp.payload,
				src->ether.type.ipv4.icmp.payload_len);
		tmp.ether.type.ipv4.icmp.payload = payload;
		tmp.ether.type.ipv4.icmp.payload_len =
			src->ether.type.ipv4.icmp.payload_len;
	}

	/* tcp payload */
	if (src->ether.type.ipv4.tcp.payload &&
			src->ether.type.ipv4.tcp.payload_len) {
		char *payload = malloc(src->ether.type.ipv4.tcp.payload_len);
		if (!payload) {
			goto error;
		}
		memcpy(payload, src->ether.type.ipv4.tcp.payload,
				src->ether.type.ipv4.tcp.payload_len);
		tmp.ether.type.ipv4.tcp.payload = payload;
		tmp.ether.type.ipv4.tcp.payload_len =
			src->ether.type.ipv4.tcp.payload_len;
	}

	/* udp payload */
	if (src->ether.type.ipv4.udp.payload &&
			src->ether.type.ipv4.udp.payload_len) {
		char *payload = malloc(src->ether.type.ipv4.udp.payload_len);
		if (!payload) {
			goto error;
		}
		memcpy(payload, src->ether.type.ipv4.udp.payload,
				src->ether.type.ipv4.udp.payload_len);
		tmp.ether.type.ipv4.udp.payload = payload;
		tmp.ether.type.ipv4.udp.payload_len =
			src->ether.type.ipv4.udp.payload_len;
	}

	/* ERROREOUS mutation func pointer */	if (src->erroneous.n_mutation) {
		/* erroneous mutation */
		mutation_func *func =
			malloc(sizeof(mutation_func) * src->erroneous.n_mutation);
		if (!func) {
			goto error;
		}
		for (int i = 0; i < src->erroneous.n_mutation; i++) {
			func[i] = src->erroneous.func[i];
		}
		tmp.erroneous.func = func;
		/* from config.yaml */
	}

	/* Extension slots (plugin-registered configs) */	/* memcpy above duplicated the ext[] pointers -- deep-copy each slot */
	for (uint8_t i = 0; i < src->n_ext; i++) {
		const struct bless_ext_cfg *desc = src->ext[i].desc;
		if (!desc || !src->ext[i].cfg) {
			continue;
		}

		void *cfg = malloc(desc->cfg_size);
		if (!cfg) {
			goto error;
		}
		memcpy(cfg, src->ext[i].cfg, desc->cfg_size);

		if (desc->clone_cfg && desc->clone_cfg(src->ext[i].cfg, cfg) < 0) {
			tmp.ext[i].cfg = cfg;
			tmp.ext[i].desc = desc;
			tmp.n_ext = (uint8_t)(i + 1);
			goto error;
		}

		tmp.ext[i].cfg = cfg;
		tmp.ext[i].desc = desc;
		tmp.n_ext = (uint8_t)(i + 1);
	}
	tmp.n_ext = src->n_ext;

	*dst = tmp;
	return 0;

error:
	config_free_clone_allocations(&tmp);
	memset(dst, 0, sizeof(*dst));
	return -1;
}
