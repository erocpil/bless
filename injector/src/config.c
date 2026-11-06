#include "injector.h"
#include "erroneous.h"
#include "config.h"
#include "define.h"
#include "log.h"

#include <ctype.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

/* HW offload types configurable via YAML ``hw-offload:`` list.
 * Not exhaustive -- covers the most commonly-needed per-packet offload toggles. */
static struct offload_table_item offload_table[] = {
	{ "ipv4", OF_IPV4 },
	{ "tcp", OF_TCP },
	{ "udp", OF_UDP },
	{ "outer-ipv4", OF_OUTER_IPV4 },
	{ "outer-udp", OF_OUTER_UDP },
};

/* forward declaration */
static void free_tree(Node *node);

Node *create_node(const char *key, const char *value, NodeType type)
{
	Node *node = (Node *)calloc(1, sizeof(Node));

	if (key) {
		node->key = strdup(key);
	}
	if (value) {
		node->value = strdup(value);
	}
	node->type = type;

	return node;
}

void add_child(Node *parent, Node *child)
{
	if (!parent->child) {
		parent->child = child;
	} else {
		Node *cur = parent->child;
		while (cur->next) {
			cur = cur->next;
		}
		cur->next = child;
	}
}

Node *parse_mapping(yaml_parser_t *parser)
{
	Node *mapping = create_node(NULL, NULL, NODE_MAPPING);
	yaml_event_t event;

	while (1) {
		if (!yaml_parser_parse(parser, &event)) {
			free_tree(mapping);
			return NULL;
		}
		if (event.type == YAML_MAPPING_END_EVENT) {
			yaml_event_delete(&event);
			break;
		}
		if (event.type == YAML_SCALAR_EVENT) {
			Node *val = parse_node(parser);
			if (val) {
				val->key = strdup((char *)event.data.scalar.value);
				add_child(mapping, val);
			} else {
				LOG_WARN("Null value detected");
			}
			yaml_event_delete(&event);
		} else {
			yaml_event_delete(&event);
		}
	}

	return mapping;
}

Node *parse_sequence(yaml_parser_t *parser)
{
	Node *sequence = create_node(NULL, NULL, NODE_SEQUENCE);
	yaml_event_t event;

	while (1) {
		if (!yaml_parser_parse(parser, &event)) {
			free_tree(sequence);
			return NULL;
		}
		if (event.type == YAML_SEQUENCE_END_EVENT) {
			yaml_event_delete(&event);
			break;
		}
		if (event.type == YAML_SCALAR_EVENT) {
			Node *child = create_node(NULL, (char *)event.data.scalar.value,
					NODE_SCALAR);
			add_child(sequence, child);
			yaml_event_delete(&event);
		} else if (event.type == YAML_MAPPING_START_EVENT) {
			yaml_event_delete(&event);
			Node *child = parse_mapping(parser);
			add_child(sequence, child);
		} else if (event.type == YAML_SEQUENCE_START_EVENT) {
			yaml_event_delete(&event);
			Node *child = parse_sequence(parser);
			add_child(sequence, child);
		} else {
			yaml_event_delete(&event);
		}
	}

	return sequence;
}

Node *parse_node(yaml_parser_t *parser)
{
	Node *node = NULL;
	yaml_event_t event;

	if (!yaml_parser_parse(parser, &event)) {
		return NULL;
	}

	switch (event.type) {
		case YAML_SCALAR_EVENT:
			node = create_node(NULL, (char *)event.data.scalar.value,
					NODE_SCALAR);
			break;
		case YAML_MAPPING_START_EVENT:
			yaml_event_delete(&event);
			return parse_mapping(parser);
		case YAML_SEQUENCE_START_EVENT:
			yaml_event_delete(&event);
			return parse_sequence(parser);
		default:
			break;
	}
	yaml_event_delete(&event);

	return node;
}

/* Parse a YAML file from disk into a tree of Node structs.
 * Returns the root Node or NULL on parse failure. */
Node *parse_yaml(const char *filename)
{
	FILE *fh = fopen(filename, "rb");
	if (!fh) {
		LOG_ERR("fopen(%s) %s", filename, strerror(errno));
		return NULL;
	}

	yaml_parser_t parser;
	yaml_event_t event;

	if (!yaml_parser_initialize(&parser)) {
		fclose(fh);
		return NULL;
	}

	yaml_parser_set_input_file(&parser, fh);

	if (!yaml_parser_parse(&parser, &event)) {
		yaml_parser_delete(&parser);
		fclose(fh);
		return NULL;
	}
	yaml_event_delete(&event);

	Node *root = NULL;

	while (1) {
		if (!yaml_parser_parse(&parser, &event)) break;
		if (event.type == YAML_STREAM_END_EVENT) {
			yaml_event_delete(&event);
			break;
		}
		if (event.type == YAML_DOCUMENT_START_EVENT) {
			yaml_event_delete(&event);
			root = parse_node(&parser);
		} else {
			yaml_event_delete(&event);
		}
	}

	yaml_parser_delete(&parser);
	fclose(fh);

	return root;
}

/* Recursively free a parsed YAML node tree (keys, values, children). */
void free_tree(Node *node)
{
	if (!node) {
		return;
	}
	free(node->key);
	free(node->value);
	free_tree(node->child);
	free_tree(node->next);
	free(node);
}

typedef void (*NodeVisitor)(Node *node, int depth, void *userdata);

void traverse(Node *node, int depth, NodeVisitor pre, NodeVisitor post,
		void *userdata)
{
	for (; node; node = node->next) {
		if (pre) {
			pre(node, depth, userdata);
		}
		if (node->child) {
			traverse(node->child, depth + 1, pre, post, userdata);
		}
		if (post) {
			post(node, depth, userdata);
		}
	}
}

void traverse_node(Node *node, int depth, NodeVisitor pre, NodeVisitor post,
		void *userdata)
{
	if (node->key) {
		printf("======\n%s: ", node->key);
		if (node->value) {
			printf("%s", node->value);
		}
		printf("\n");
	}
	node = node->child;
	depth++;
	for (; node; node = node->next) {
		if (pre) {
			pre(node, depth, userdata);
		}
		if (node->child) {
			traverse(node->child, depth + 1, pre, post, userdata);
		}
		if (post) {
			post(node, depth, userdata);
		}
	}
	printf("======\n");
}

void print_pre(Node *node, int depth, void *userdata)
{
	(void)userdata;
	for (int i = 0; i < depth; i++) {
		printf("  ");
	}
	if (node->key) printf("%s: ", node->key);

	if (node->type == NODE_SCALAR) {
		printf("%s\n", node->value ? node->value : "null");
	} else if (node->type == NODE_MAPPING) {
		printf("{\n");
	} else if (node->type == NODE_SEQUENCE) {
		printf("[\n");
	}
}

void print_post(Node *node, int depth, void *userdata)
{
	(void)userdata;
	if (node->type == NODE_MAPPING) {
		for (int i = 0; i < depth; i++) printf("  ");
		printf("}\n");
	} else if (node->type == NODE_SEQUENCE) {
		for (int i = 0; i < depth; i++) printf("  ");
		printf("]\n");
	}
}

/* Locate a YAML node by dot-delimited path (e.g. ``injector.ether.dst``).
 * Supports array index notation: ``ports[0]``.
 * Returns the matching Node or NULL. */
Node *find_by_path(Node *root, const char *path)
{
	char *copy = strdup(path);
	char *token = strtok(copy, ".");
	Node *cur = root;

	while (token && cur) {
		// Check for [index] suffix
		char key[128];
		int idx = -1;
		if (sscanf(token, "%127[^[][%d]", key, &idx) >= 1) {
			// Find key
			Node *child = cur->child;
			while (child && (!child->key || strcmp(child->key, key) != 0)) {
				child = child->next;
			}

			if (!child) {
				LOG_TRACE("no child found from key %s", key);
				free(copy);
				return NULL;
			}
			cur = child;

			// get index for array
			if (idx >= 0 && cur->type == NODE_SEQUENCE) {
				Node *elem = cur->child;
				for (int i = 0; elem && i < idx; i++) {
					elem = elem->next;
				}
				cur = elem;
			}
		} else {
			LOG_ERR("sscanf(%s) in path \"%s\"", token, path);
			return NULL;
		}
		token = strtok(NULL, ".");
	}
	free(copy);

	return cur;
}

/* Unmap and close a config file mapping previously opened
 * by config_file_map_open(). */
void config_file_unmap_close(struct config_file_map *fm)
{
	if (!fm) {
		return;
	}

	if (fm->addr && fm->addr != MAP_FAILED) {
		munmap(fm->addr, fm->len);
	}

	if (fm->fd >= 0) {
		close(fm->fd);
	}

	fm->addr = NULL;
	fm->len = 0;
	fm->fd = -1;
}

/* Memory-map a config file into the config_file_map struct.
 * Returns 0 on success, -1 on error (logs errno). */
int config_file_map_open(struct config_file_map *cfm)
{
	struct stat st;

	if (!cfm || !cfm->name) {
		return -1;
	}

	cfm->fd = -1;

	cfm->fd = open(cfm->name, O_RDONLY);
	if (cfm->fd < 0) {
		return -1;
	}

	if (fstat(cfm->fd, &st) < 0) {
		goto err;
	}

	if (!S_ISREG(st.st_mode)) {
		goto err;
	}

	if (st.st_size == 0) {
		cfm->addr = NULL;
		cfm->len = 0;
		return 0;
	}

	cfm->len = st.st_size;
	cfm->addr = mmap(NULL, cfm->len + 1, PROT_READ | PROT_WRITE,
			MAP_PRIVATE, cfm->fd, 0);
	if (cfm->addr == MAP_FAILED) {
		goto err;
	}
	((char*)cfm->addr)[cfm->len] = '\0';

	return 0;

err:
	config_file_unmap_close(cfm);

	return -1;
}

/* Validate that a config_file_map describes a readable, non-empty file.
 * Returns 0 on success, -1 if the file is missing or empty. */
int config_check_file_map(struct config_file_map *cfm)
{
	LOG_HINT("checking config file \"%s\"", cfm->name);
	if (access(cfm->name, F_OK) != 0) {
		LOG_ERR("config file \"%s\" not found", cfm->name);
		return -1;
	}
	LOG_INFO("config file \"%s\" exists", cfm->name);

	LOG_HINT("mmap config file \"%s\"", cfm->name);
	if (config_file_map_open(cfm)) {
		LOG_ERR("config file \"%s\" open/mmap failed", cfm->name);
		return -1;
	}

	LOG_HINT("reading config file \"%s\"", cfm->name);
	if (!cfm->len) {
		LOG_ERR("config file \"%s\" is empty", cfm->name);
		goto err;
	}
	LOG_INFO("config file \"%s\" non-empty", cfm->name);

	size_t binary_count = 0;
	for (size_t i = 0; i < cfm->len; i++) {
		unsigned char c = ((char*)cfm->addr)[i];
		// Allow common control chars: LF(10), CR(13), TAB(9)
		if (!(isprint(c) || c == '\n' || c == '\r' || c == '	')) {
			binary_count++;
		}
	}

	LOG_HINT("checking control character ratio in \"%s\"", cfm->name);
	// Binary file if binary char ratio > 10%
	if ((double)binary_count / cfm->len > 0.1) {
		LOG_WARN("control chars > 10%% in \"%s\"", cfm->name);
		LOG_ERR("config file \"%s\" is not a text file", cfm->name);
		goto err;
	}
	LOG_HINT("config file \"%s\" is text", cfm->name);
	LOG_HINT("closing config file \"%s\"", cfm->name);
	close(cfm->fd);

	return 0;

err:
	config_file_unmap_close(cfm);
	free(cfm);

	return -1;
}

/**
 * parse_yaml_from_mem - parse YAML from an mmap'd memory buffer.
 * Uses yaml_parser_set_input_string() to avoid re-opening the file.
 */
static Node *parse_yaml_from_mem(const char *buf, size_t len)
{
	yaml_parser_t parser;
	yaml_event_t event;

	if (!yaml_parser_initialize(&parser)) {
		return NULL;
	}

	yaml_parser_set_input_string(&parser, (const unsigned char *)buf, len);

	if (!yaml_parser_parse(&parser, &event)) {
		yaml_parser_delete(&parser);
		return NULL;
	}
	yaml_event_delete(&event);

	Node *root = NULL;

	while (1) {
		if (!yaml_parser_parse(&parser, &event)) break;
		if (event.type == YAML_STREAM_END_EVENT) {
			yaml_event_delete(&event);
			break;
		}
		if (event.type == YAML_DOCUMENT_START_EVENT) {
			yaml_event_delete(&event);
			root = parse_node(&parser);
		} else {
			yaml_event_delete(&event);
		}
	}

	yaml_parser_delete(&parser);

	return root;
}

/* Reject unsupported settings before device initialization. */
static void config_check_protocols(Node *node)
{
	static const char *unsupported[] = {
		"dns", "http", "sctp", "quic", "ipv6", "outer_ipv6",
		"proto", "src-range", "dst-range",
	};
	for (; node; node = node->next) {
		for (size_t i = 0; i < sizeof(unsupported) / sizeof(unsupported[0]); i++) {
			if (node->key && !strcmp(node->key, unsupported[i])) {
				LOG_ERR("unsupported config setting: %s", node->key);
				exit(EXIT_FAILURE);
			}
		}
		if (node->key && (!strcmp(node->key, "hw-offload") ||
		    !strcmp(node->key, "mac"))) {
			for (Node *item = node->child; item; item = item->next) {
				for (size_t i = 0; i < sizeof(unsupported) / sizeof(unsupported[0]); i++) {
					if (item->value && !strcmp(item->value, unsupported[i])) {
						LOG_ERR("unsupported %s value: %s", node->key, item->value);
						exit(EXIT_FAILURE);
					}
				}
			}
		}
		config_check_protocols(node->child);
	}
}

/* Bootstrap: parse the config YAML from the given config_file_map
 * and return the root Node tree.  Calls parse_yaml() under the hood. */
Node *config_init(struct config_file_map *cfm)
{
	if (!cfm || !cfm->addr || !cfm->len) {
		fprintf(stderr, "config_init: invalid config_file_map\n");
		return NULL;
	}

	Node *root = parse_yaml_from_mem(cfm->addr, cfm->len);
	if (!root) {
		fprintf(stderr, "Failed to parse YAML from %s\n", cfm->name ? cfm->name : "(null)");
	}

	config_check_protocols(root);
	return root;
}

/* Free the config root Node tree allocated by config_init(). */
static void free_node_tree(Node *node)
{
	if (!node)
		return;

	/* depth-first: free children first, then siblings */
	free_node_tree(node->child);
	free_node_tree(node->next);

	free(node->key);
	free(node->value);
	free(node);
}

int config_exit(Node *root)
{
	free_node_tree(root);
	return 0;
}

/* Injector only forwards options implemented by its TX-only CLI. */
static int injector_option_supported(const char *key)
{
	static const char *const options[] = {
		"p", "q", "num", "batch", "batch-delay-us",
		"batch-jitter-us", "traffic-model", "bench-mode", "weight",
		"arp", "icmp", "tcp", "udp", "pps-rate", "pps-burst",
		"bps-rate", "bps-burst", "interleave", "interleave-depth",
		"seed", "log-format",
	};

	for (size_t i = 0; i < sizeof(options) / sizeof(options[0]); i++) {
		if (!strcmp(key, options[i]))
			return 1;
	}
	return 0;
}

/* Translate a DPDK or Injector YAML section into argv entries. */
static int config_parse_dpdk_internal(Node *node, int *targc, char ***targv,
		int i, int injector_section)
{
	char **argv = *targv;

	for (Node *n = node->child; n != NULL; n = n->next) {
		if (injector_section && n->key) {
			if (n->type == NODE_SCALAR && (!n->value ||
			    !strcmp(n->value, "null") ||
			    !strcmp(n->value, "NULL")))
				continue;
			if (!strcmp(n->key, "mode")) {
				if (!n->value || strcmp(n->value, "tx-only")) {
					LOG_ERR("injector supports only mode: tx-only");
					return -1;
				}
				continue;
			}
			if (!strcmp(n->key, "auto-start")) {
				if (!n->value || (strcmp(n->value, "true") &&
				    strcmp(n->value, "TRUE"))) {
					LOG_ERR("injector always starts TX workers; "
					        "auto-start must be true");
					return -1;
				}
				continue;
			}
			if (!injector_option_supported(n->key)) {
				LOG_ERR("unsupported injector config key: %s", n->key);
				return -1;
			}
			if (n->type == NODE_MAPPING) {
				LOG_ERR("injector option '%s' must be a scalar or sequence",
				        n->key);
				return -1;
			}
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
				perror("malloc(dpdk eal arg)");
				exit(EXIT_FAILURE);
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
				argv[i] = malloc(len);
				if (!argv[i]) {
					LOG_ERR("malloc(%s)", t->value);
					exit(EXIT_FAILURE);
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
		return 0;
	}

	Node *server_enable = find_by_path(system_node, "server.enable");
	if (server_enable && server_enable->value &&
	    (!strcmp(server_enable->value, "true") ||
	     !strcmp(server_enable->value, "TRUE"))) {
		LOG_ERR("injector does not support system.server.enable: true");
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

	return 0;
}

/* Top-level entry: parse the DPDK config section and return EAL argc/argv.
 * Returns the number of arguments parsed. */
int config_parse_dpdk(Node *root, int *targc, char ***targv)
{
#define MAX_EAL_PARAMS 128
	char *path = "dpdk";
	Node *node = find_by_path(root, path);
	if (!node) {
		return -1;
	}

	int argc = 1;
	char **argv = malloc(sizeof(char*) * (MAX_EAL_PARAMS + 1));
	if (!argv) {
		perror("malloc(dpdk argv)");
		exit(EXIT_FAILURE);
	}
	memset(argv, 0, sizeof(char*) * (MAX_EAL_PARAMS + 1));

	int i = 0;
	size_t len = strlen((*targv)[i]) + 1;
	argv[i] = malloc(len);
	if (!argv[i]) {
		perror("malloc(dpdk argv[0])");
		exit(EXIT_FAILURE);
	}
	strcpy(argv[i], (*targv)[i]);

	i = config_parse_dpdk_internal(node, &argc, &argv, ++i, 0);
	if (i < 0)
		return -1;

	// Add separator --
	argv[i] = strdup("--");
	if (!argv[i]) {
		perror("strdup(dpdk/injector separator)");
		exit(EXIT_FAILURE);
	}

	path = "injector";
	node = find_by_path(root, path);
	if (!node) {
		rte_exit(EXIT_FAILURE, "no injector config found\n");
	}

	i = config_parse_dpdk_internal(node, &argc, &argv, ++i, 1);
	if (i < 0)
		return -1;

	path = "bless";
	node = find_by_path(root, path);
	if (!node) {
		rte_exit(EXIT_FAILURE, "no bless config found\n");
	}

	*targc = argc;
	*targv = argv;
	(*targv)[*targc] = NULL;

	return 0;
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
				perror("malloc(injector arg)");
				exit(EXIT_FAILURE);
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
				perror("malloc(injector seq arg)");
				exit(EXIT_FAILURE);
			}

			char *p = argv[i];
			p += sprintf(p, "--%s=", fullkey);
			t = n->child;
			while (t) {
				p += sprintf(p, "%s", t->value);
				if (t->next) *p++ = ',';
				t = t->next;
			}
			*p = '\0';
			i++;

		} else if (n->type == NODE_MAPPING) {
			// recursively
			i = config_parse_generic(n, &i, &argv, i, fullkey);
		}
	}

	*targc = i;
	*targv = argv;

	return i;
}

static int parse_uint16(const char *s, uint16_t *out)
{
	if (s == NULL || *s == '\0' || out == NULL) {
		return 0;
	}

	char *endptr;
	errno = 0;

	// base=0: auto-detect 0x / 0 prefix
	unsigned long val = strtoul(s, &endptr, 0);
	if (errno != 0) {
		// overflow or other errors
		return 0;
	}
	if (*endptr != '\0') {
		// invalid characters
		return 0;
	}
	if (val > UINT16_MAX) {
		return 0;
	}

	*out = (uint16_t)val;

	return 1;
}

static int parse_uint32(const char *s, uint32_t *out)
{
	if (s == NULL || *s == '\0' || out == NULL) {
		return 0;
	}

	char *endptr;
	errno = 0;

	// base=0: auto-detect 0x / 0 prefix
	unsigned long long val = strtoull(s, &endptr, 0);
	if (errno != 0) {
		return 0;  // Overflow or other error
	}
	if (*endptr != '\0') {
		return 0;  // Invalid character present
	}
	if (val > UINT32_MAX) {
		return 0;  // Exceeds uint32_t range
	}

	*out = (uint32_t)val;

	return 1;
}

/**
 * config_parse_sequence_to_array - convert string in Node to int[].
 * Only for uint16_t / uint32_t
 */
static int config_parse_sequence_to_array(Node *node, void *array,
		int size, uint16_t limit)
{
	int n = 0;

	switch (node->type) {
		case NODE_SCALAR:
			if (sizeof(uint16_t) == size) {
				if (!parse_uint16(node->value, (uint16_t*)array))
					return -1;
			} else {
				if (!parse_uint32(node->value, (uint32_t*)array))
					return -1;
			}
			n++;
			break;
		case NODE_SEQUENCE:
			for (Node *i = node->child; i != NULL && n < limit;
					i = i->next, n++) {
				if (sizeof(uint16_t) == size) {
					if (!parse_uint16(i->value, (uint16_t*)((char*)array + size * n)))
						return -1;
				} else {
					if (!parse_uint32(i->value, (uint32_t*)((char*)array + size * n)))
						return -1;
				}
			}
			if (n >= limit) {
				int extra = 0;
				for (Node *i = node->child; i; i = i->next) extra++;
				if (extra > limit)
					LOG_WARN("sequence truncated to %d items (had %d)",
								(int)limit, extra);
			}
			break;
		case NODE_MAPPING:
			break;
		default:
			break;
	}

	return n;
}

static int config_parse_port_maybe_range_to_array(Node *node, uint16_t *port,
		int32_t *range, uint16_t limit)
{
	int n = 0;

	switch (node->type) {
		case NODE_SCALAR:
			{
				uint16_t p;
				int32_t ra;
				if (injector_parse_port_range(node->value, &p, &ra)) {
					goto ERROR;
				}
				port[0] = p;
				*range = ra;
				n++;
			}
			break;
		case NODE_SEQUENCE:
			for (Node *i = node->child; i != NULL && n < limit;
					i = i->next, n++) {
				if (!parse_uint16(i->value, &port[n]))
					goto ERROR;
			}
			if (n >= limit) {
				int extra = 0;
				for (Node *i = node->child; i; i = i->next) extra++;
				if (extra > limit)
					LOG_WARN("port sequence truncated to %d items (had %d)",
								(int)limit, extra);
			}
			*range = 0;
			break;
		case NODE_MAPPING:
			break;
		default:
			break;
	}

	return n;

ERROR:
	LOG_ERR("yaml config parse error: %s", node->value);
	exit(-1);
}

/* Parse a VNI token with optional +N range suffix (e.g. "100" or "100+5").
 * On success, sets *base and returns the range count (≥1).
 * On failure, logs an error and returns 0. */
static int parse_vni_with_range(const char *token, uint32_t *base)
{
	char *endptr;
	unsigned long val;
	int count;

	errno = 0;
	val = strtoul(token, &endptr, 0);
	if (errno || endptr == token || val > UINT32_MAX) {
		LOG_ERR("invalid VNI: %s", token);
		return 0;
	}
	*base = (uint32_t)val;
	count = 1;

	if (*endptr == '+') {
		errno = 0;
		unsigned long r = strtoul(endptr + 1, &endptr, 0);
		if (errno || *endptr != '\0' || r == 0) {
			LOG_ERR("invalid VNI range: %s", token);
			return 0;
		}
		count = (int)r;
	}
	return count;
}

static int config_parse_sequence_ipv4_vni_to_array(Node *node, uint32_t *array,
		uint32_t *vni, int size, uint16_t limit)
{
	int n = 0;
	char *saveptr = NULL;
	char *token = NULL;
	char *dup = NULL;

	switch (node->type) {
		case NODE_SCALAR:
			dup = strdup(node->value);
			/* vtep ipv4 */
			token = strtok_r(dup, ":", &saveptr);
			if (!token) {
				LOG_WARN("no vni");
				exit(-1);
			}
			if (inet_pton(AF_INET, token, (char*)array + size * n) != 1) {
				LOG_ERR("invalid VTEP IP: %s", token);
				exit(-1);
			}
			/* vni */
			token = strtok_r(NULL, ":", &saveptr);
			if (!token) {
				LOG_WARN("no vni");
				exit(-1);
			}
			/* parse VNI with optional range syntax: 100+5 */
			{
				uint32_t base_vni;
				int range_count = parse_vni_with_range(token, &base_vni);
				if (!range_count)
					exit(-1);
				for (int rn = 0; rn < range_count && n < (int)limit; rn++) {
					if (rn > 0) {
						memcpy((char*)array + size * n,
						       (char*)array + size * (n - 1),
						       size);
					}
					vni[n] = (uint32_t)(base_vni + rn);
					n++;
				}
			}
			break;
		case NODE_SEQUENCE:
			for (Node *i = node->child; i != NULL && n < limit;
					i = i->next) {
				dup = strdup(i->value);
				/* vtep ipv4 */
				token = strtok_r(dup, ":", &saveptr);
				if (!token) {
					LOG_WARN("no vni");
					exit(-1);
				}
				uint32_t ip_val;
				if (inet_pton(AF_INET, token, &ip_val) != 1) {
					LOG_ERR("invalid VTEP IP: %s", token);
					exit(-1);
				}
				/* vni */
				token = strtok_r(NULL, ":", &saveptr);
				if (!token) {
					LOG_WARN("no vni");
					exit(-1);
				}
				/* parse VNI with optional range syntax */
				{
					uint32_t base_vni;
					int range_count = parse_vni_with_range(token, &base_vni);
					if (!range_count)
						exit(-1);
					for (int rn = 0; rn < range_count && n < (int)limit; rn++) {
						array[n] = ip_val;
						vni[n] = (uint32_t)(base_vni + rn);
						n++;
					}
				}
			}
			if (n >= limit) {
				int extra = 0;
				for (Node *i = node->child; i; i = i->next) extra++;
				if (extra > limit)
					LOG_WARN("VXLAN VTEP sequence truncated to %d items (had %d)",
								(int)limit, extra);
			}
			break;
		case NODE_MAPPING:
			break;
		default:
			break;
	}

	return n;
}
/**
 * A scalar value provides an ipv4 address and range, like: 10.0.0.1+100.
 * A qequence value provides a series of ipv4 addresses: [ 1.0.0.1, 2.0.0.2 ]
 */
static int config_parse_ipv4_maybe_range_to_array(Node *node, uint32_t *addr,
		int64_t *range, uint32_t limit)
{
	uint32_t n = 0;

	switch (node->type) {
		case NODE_SCALAR:
			{
				uint32_t ipv4;
				int64_t ra;
				if (injector_parse_ip_range(node->value, &ipv4, &ra)) {
					break;
				}
				addr[0] = ipv4;
				*range = ra;
				n++;
			}
			break;
		case NODE_SEQUENCE:
			for (Node *i = node->child; i != NULL && n < limit;
					i = i->next, n++) {
				if (inet_pton(AF_INET, i->value, &addr[n]) != 1) {
					return -1;
				}
				// injector_print_ipv4(addr[n]);
			}
			if (n >= limit) {
				int extra = 0;
				for (Node *i = node->child; i; i = i->next) extra++;
				if (extra > limit)
					LOG_WARN("IPv4 sequence truncated to %d items (had %d)",
								(int)limit, extra);
			}
			*range = 0;
			break;
		case NODE_MAPPING:
			break;
		default:
			break;
	}

	return n;
}

static int config_parse_sequence_ipv4_to_array(Node *node, void *array,
		int size, uint16_t limit)
{
	int n = 0;

	switch (node->type) {
		case NODE_SCALAR:
			if (inet_pton(AF_INET, node->value, array) != 1)
				return -1;
			n++;
			break;
		case NODE_SEQUENCE:
			for (Node *i = node->child; i != NULL && n < limit;
					i = i->next, n++) {
				if (inet_pton(AF_INET, i->value, (char*)array + size * n) != 1)
					return -1;
			}
			break;
		case NODE_MAPPING:
			break;
		default:
			break;
	}

	return n;
}

/* Parse the ``injector.ether`` section (MAC addresses, MTU, offload). */
static int config_parse_injector_ether(Node *injector_node, Cnode *cnode)
{
	Node *ether_node = find_by_path(injector_node, "ether");
	if (!ether_node) return 0;

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
		for (Node *child = node->child; child && n < INJECTOR_CONFIG_MAX;
				child = child->next, n++) {
			if (!parse_uint16(child->value, &cnode->ether.imix[n])) {
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
					if (!*end && v > 0 && v < 100000)
						injector_set_type_weight(child->key, (int32_t)v);
				}
			}
		} else if (node->type == NODE_SEQUENCE) {
			/* dist: [ tcp: 50, udp: 30 ]  -- inline mapping entries */
			for (Node *child = node->child; child; child = child->next) {
				if (child->value && child->key) {
					char *end = NULL;
					long v = strtol(child->value, &end, 10);
					if (!*end && v > 0 && v < 100000)
						injector_set_type_weight(child->key, (int32_t)v);
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
		// injector_print_mac((struct rte_ether_addr*)cnode->ether.dst);
	} else {
		LOG_WARN("Invalid dst mac address");
		exit(-1);
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
			// injector_print_mac((struct rte_ether_addr*)cnode->ether.src);
			cnode->ether.n_src = 1;
		} else {
			LOG_INFO("Invalid src mac address, injector will try local port");
			cnode->ether.n_src = 0;
		}
	}

	return 1;

ERROR:
	LOG_ERR("yaml config parse error: ether.dst");
	exit(-1);
}

/* Parse the ``injector.vxlan.ether`` subsection (VXLAN outer MAC). */
static int config_parse_injector_vxlan_ether(Node *vxlan_node, Cnode *cnode)
{
	Node *node = find_by_path(vxlan_node, "ether.dst");
	if (!node) {
		return 0;  /* VXLAN ether section not configured -- skip */
	}

	cnode->vxlan.ether.n_dst = 0;
	if (node->value && strlen(node->value) &&
			(rte_ether_unformat_addr(node->value,
									 (struct rte_ether_addr*)&
									 (cnode->vxlan.ether.dst)) == 0)) {
		// injector_print_mac((struct rte_ether_addr*)cnode->vxlan.ether.dst);
		cnode->vxlan.ether.n_dst = 1;
	} else {
		goto ERROR;
	}

	node = find_by_path(vxlan_node, "ether.src");
	if (!node) {
		LOG_INFO("no vxlan src mac address node provided");
		cnode->vxlan.ether.n_src = 0;
	} else {
		if (node->value && strlen(node->value) &&
				(rte_ether_unformat_addr(node->value,
										 (struct rte_ether_addr*)&
										 (cnode->vxlan.ether.src)) == 0)) {
			// injector_print_mac((struct rte_ether_addr*)cnode->vxlan.ether.src);
			cnode->vxlan.ether.n_src = 1;
		} else {
			LOG_WARN("Invalid vxlan src mac address, "
					"injector will try local port");
			cnode->vxlan.ether.n_src = 0;
		}
	}

	return 1;

ERROR:
	LOG_ERR("yaml config parse error: vxlan.ether.dst");
	exit(-1);
}

/* Parse the ``injector.ether.type.arp`` section (ARP request/response config). */
static int config_parse_injector_ether_type_arp(Node *ether_node, Cnode *cnode)
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
	exit(-1);
}

/* Parse the ``injector.ether.type.ipv4.icmp`` section (ICMP Echo identifiers). */
static int config_parse_injector_ether_type_ipv4_icmp(Node *ether_node, Cnode *cnode)
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
	if (node && node->value && strlen(node->value)) {
		char *payload = strdup(node->value);
		cnode->ether.type.ipv4.icmp.payload = payload;
		cnode->ether.type.ipv4.icmp.payload_len = strlen(payload) + 1;
	}

	return n;

ERROR:
	LOG_ERR("yaml config parse error: type.ipv4.icmp.ident");
	exit(-1);
}

/* Parse ``injector.ether.type.ipv4.tcp`` section (TCP src/dst ports, flags, payload). */
static int config_parse_injector_ether_type_ip_tcp(Node *ether_node, Cnode *cnode)
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
	if (node && node->value && strlen(node->value)) {
		char *payload = strdup(node->value);
		cnode->ether.type.ipv4.tcp.payload = payload;
		cnode->ether.type.ipv4.tcp.payload_len = strlen(payload) + 1;
	}

	return n;

ERROR:
	LOG_ERR("yaml config parse error: type.ipv4.tcp");
	exit(-1);
}

/* Parse ``injector.ether.type.ipv4.udp`` section (UDP src/dst ports, payload). */
static int config_parse_injector_ether_type_ip_udp(Node *ether_node, Cnode *cnode)
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
	if (node && node->value && strlen(node->value)) {
		char *payload = strdup(node->value);
		cnode->ether.type.ipv4.udp.payload = payload;
		cnode->ether.type.ipv4.udp.payload_len = strlen(payload) + 1;
	}

	return n;

ERROR:
	LOG_ERR("yaml config parse error: type.ipv4.udp");
	exit(-1);
}

/* Parse the ``injector.ether.type.ipv4`` section (IP src/dst, proto, TOS, TTL). */
static int config_parse_injector_ether_type_ipv4(Node *ether_node, Cnode *cnode)
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
			cnode->ether.type.ipv4.n_dst = 0;
		} else {
			cnode->ether.type.ipv4.src_range = 0;
			cnode->ether.type.ipv4.n_dst = n;
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
	exit(-1);
}

/* Parse the ``injector.vxlan`` section (enable, ratio, VNI ranges, wire MTU). */
static int config_parse_injector_vxlan(Node *injector_node, Cnode *cnode)
{
	int n = 0;
	Node *vxlan_node = find_by_path(injector_node, "vxlan");
	if (!vxlan_node) return 0;

	/* enable */
	cnode->vxlan.enable = 1;
	Node *node = find_by_path(vxlan_node, "enable");
	if (!node || !strlen(node->value) || (
				strcmp("true", node->value) &&
				strcmp("TRUE", node->value)
				)
	   ) {
		LOG_HINT("vxlan disabled");
		cnode->vxlan.enable = 0;
	}

	/* ratio */
	node = find_by_path(vxlan_node, "ratio");
	if (!node || !strlen(node->value)) {
		LOG_HINT("vxlan disabled");
		cnode->vxlan.enable = 0;
		cnode->vxlan.ratio = 0;
	} else {
		/* No early return here: wire-mtu is still
		 * parsed below (needed by mutation_udp_vxlan even when
		 * VXLAN traffic generation is disabled).
		 * The full outer config (ether/IP/UDP) is skipped at the
		 * !enable guard below -- mutation gets zeroed defaults there,
		 * which is acceptable for corner-case usage. */
		char *endptr;
		errno = 0;
		long r = strtol(node->value, &endptr, 0);
		if (errno || endptr == node->value || *endptr != '\0' ||
		    r < 0 || r > 100) {
			LOG_WARN("invalid vxlan ratio \"%s\", disabling", node->value);
			cnode->vxlan.enable = 0;
			cnode->vxlan.ratio = 0;
		} else {
			cnode->vxlan.ratio = (uint16_t)r;
		}
	}

	/* wire-mtu -- VXLAN-aware wire-level MTU constraint (0 = disabled) */
	cnode->vxlan.wire_mtu = 0;
	node = find_by_path(vxlan_node, "wire-mtu");
	if (node && strlen(node->value)) {
		char *endptr;
		errno = 0;
		long wm = strtol(node->value, &endptr, 0);
		if (errno || endptr == node->value || *endptr != '\0') {
			LOG_WARN("invalid vxlan wire-mtu \"%s\", ignoring", node->value);
		} else if (wm > 0 && wm < 68) {
			LOG_WARN("vxlan wire-mtu %ld too small, setting to 0 (disabled)", wm);
		} else if (wm > 9000) {
			LOG_INFO("vxlan wire-mtu %ld clamped to 9000", wm);
			cnode->vxlan.wire_mtu = 9000;
		} else {
			cnode->vxlan.wire_mtu = (uint16_t)wm;
		}
	}

	/* VXLAN traffic generation disabled: skip outer config parsing
	 * so the user isn't forced to provide outer IPs/ports/MACs.
	 * Mutation udp_vxlan still works with zeroed defaults here. */
	if (!cnode->vxlan.enable) {
		return 0;
	}

	config_parse_injector_vxlan_ether(vxlan_node, cnode);

	/* VXLAN over IPv4 (default): parse ``vxlan.ether.type.ipv4`` */
	node = find_by_path(vxlan_node, "ether.type.ipv4.src");
	if (!node) {
		goto ERROR;
	}

	n = config_parse_sequence_ipv4_vni_to_array(node,
			cnode->vxlan.ether.type.ipv4.src, cnode->vxlan.ether.type.ipv4.vni,
			sizeof(uint32_t), PORT_MAX);
	if (n > 0) {
		cnode->vxlan.ether.type.ipv4.n_src = n;
	} else {
		goto ERROR;
	}

	node = find_by_path(vxlan_node, "ether.type.ipv4.dst");
	if (!node) {
		printf("Invalid vxlan ipv4 dst address.\n");
		exit(-1);
	}

	n = config_parse_sequence_ipv4_to_array(node, cnode->vxlan.ether.type.ipv4.dst,
			sizeof(uint32_t), IP_ADDR_MAX);
	if (n > 0) {
		cnode->vxlan.ether.type.ipv4.n_dst = n;
	} else {
		goto ERROR;
	}

	node = find_by_path(vxlan_node, "ether.type.ipv4.udp.src");
	if (!node) {
		goto ERROR;
	}

	n = config_parse_sequence_to_array(node, cnode->vxlan.ether.type.ipv4.udp.src,
			sizeof(uint16_t), PORT_MAX);
	if (n > 0) {
		cnode->vxlan.ether.type.ipv4.udp.n_src = n;
	} else {
		goto ERROR;
	}

	node = find_by_path(vxlan_node, "ether.type.ipv4.udp.dst");
	if (!node) {
		goto ERROR;
	}
	n = config_parse_sequence_to_array(node, cnode->vxlan.ether.type.ipv4.udp.dst,
			sizeof(uint16_t), PORT_MAX);
	if (n > 0) {
		cnode->vxlan.ether.type.ipv4.udp.n_dst = n;
	} else {
		goto ERROR;
	}

	return n;

ERROR:
	LOG_ERR("yaml config parse error: vxlan");
	exit(-1);
}

/* Parse the ``injector.erroneous`` section (ratio, per-class mutation lists). */
static int config_parse_injector_erroneous(Node *injector_node, Cnode *cnode)
{
	cnode->erroneous.ratio = 0;
	Node *node = find_by_path(injector_node, "erroneous.ratio");
	if (node && strlen(node->value)) {
		char *endptr;
		errno = 0;
		long r = strtol(node->value, &endptr, 0);
		if (!errno && endptr != node->value && *endptr == '\0' && r > 0) {
			cnode->erroneous.ratio = (uint16_t)r;
		}
	}
	if (!cnode->erroneous.ratio) {
		LOG_INFO("erroneous disabled due to no valid ratio");
		return 1;
	}
	node = find_by_path(injector_node, "erroneous.class");
	if (!node || !node->child ) {
		cnode->erroneous.ratio = 0;
		LOG_INFO("erroneous disabled due to no class");
		return 1;
	}

	int res = 0;
	int n_clas = 0;
	for (Node *n = node->child; n; n = n->next) {
		if (n_clas >= INJECTOR_ERRNONEOUS_CLASS_TYPE_MAX) {
			LOG_WARN("erroneous: at most %d classes, rest ignored",
				INJECTOR_ERRNONEOUS_CLASS_TYPE_MAX);
			break;
		}
		struct ec_clas *clas = &cnode->erroneous.clas[n_clas++];
		clas->name = strdup(n->key);
		int n_type = 0;
		for (Node *i = n->child; i; i = i->next) {
			if (n_type >= INJECTOR_CONFIG_MAX) {
				LOG_WARN("erroneous.class.%s: at most %d mutations, rest ignored",
					clas->name, INJECTOR_CONFIG_MAX);
				break;
			}
			clas->type[n_type++] = strdup(i->value);
			res++;
		}
		clas->n_type = n_type;
	}
	cnode->erroneous.n_clas = n_clas;
	cnode->erroneous.n_mutation = res;

	int pos = 0;
	mutation_func *func = malloc(sizeof(mutation_func) * (uint64_t)res);
	for (int i = 0; i < cnode->erroneous.n_clas; i++) {
		struct ec_clas *clas = &cnode->erroneous.clas[i];
		for (int j = 0; j < clas->n_type; j++) {
			mutation_func f = find_mutation_func(clas->name, clas->type[j]);
			if (!f) {
				LOG_WARN(" no mutation `%s:%s'", clas->name, clas->type[j]);
				exit(-1);
			}
			func[pos++] = f;
		}
	}
	cnode->erroneous.func = func;

	return res;
}

/* Plugin extension config parsing */
static void parse_one_plugin(const struct injector_ext_cfg *ext, void *ctx);

/* Dispatch plugin YAML parse callbacks for all registered extensions. */
static void config_parse_plugins(Node *ether_node, Cnode *cnode)
{
	injector_foreach_cfg_parser(&parse_one_plugin, &(struct {
		Node *ether_node;
		Cnode *cnode;
	}){ ether_node, cnode });
}

/* Invoke a single plugin's config parser callback on its YAML section. */
static void parse_one_plugin(const struct injector_ext_cfg *ext, void *ctx)
{
	struct { Node *ether_node; Cnode *cnode; } *p = ctx;
	Node *plugin_node = find_by_path(p->ether_node, ext->yaml_path);
	if (!plugin_node)
		return;

	void *cfg = calloc(1, ext->cfg_size);
	if (!cfg) return;

	if (injector_parse_cfg_fields(ext->fields, cfg, plugin_node) < 0) {
		free(cfg);
		return;
	}

	/* Run custom YAML sub-tree parser if registered. */
	if (ext->parse_cfg && ext->parse_cfg(plugin_node, cfg) < 0) {
		ext->free_cfg ? ext->free_cfg(cfg) : (void)0;
		free(cfg);
		return;
	}

	int i = p->cnode->n_ext;
	if (i >= INJECTOR_PLUGIN_MAX) {
		free(cfg);
		return;
	}
	p->cnode->ext[i].desc = ext;
	p->cnode->ext[i].cfg  = cfg;
	p->cnode->n_ext = (uint8_t)(i + 1);
}

Cnode *config_parse_injector(Node *root)
{
	config_check_protocols(root);
	struct Cnode *cnode = malloc(sizeof(struct Cnode));
	memset(cnode, 0, sizeof(struct Cnode));

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
	config_parse_injector_ether(bless_node, cnode);
	config_parse_injector_ether_type_arp(ether_node, cnode);
	config_parse_injector_ether_type_ipv4(ether_node, cnode);
	config_parse_injector_ether_type_ipv4_icmp(ether_node, cnode);
	config_parse_injector_ether_type_ip_tcp(ether_node, cnode);
	config_parse_injector_ether_type_ip_udp(ether_node, cnode);
	config_parse_injector_vxlan(bless_node, cnode);
	config_parse_injector_erroneous(bless_node, cnode);

	/* Parse extension plugin configs (registered via injector_register_cfg_parser) */
	{
		Node *ipv4_node = find_by_path(ether_node, "type.ipv4");
		config_parse_plugins(ipv4_node ? ipv4_node : ether_node, cnode);
	}

	return cnode;

ERROR:
	LOG_ERR("yaml config parse error");
	exit(-1);
}

/* Deep-copy a Cnode: allocates fresh arrays for all variable-length fields
 * (MAC addresses, IP addresses, port ranges, VNI list, etc.).
 * Returns 0 on success, -1 on allocation failure. */
int config_clone_cnode(Cnode *src, Cnode *dst)
{
	if (!src || !dst) {
		return -1;
	}

	memcpy(dst, src, sizeof(Cnode));

	/* icmp payload */
	if (src->ether.type.ipv4.icmp.payload &&
			src->ether.type.ipv4.icmp.payload_len) {
		char *payload = malloc(src->ether.type.ipv4.icmp.payload_len);
		if (!payload) {
			rte_exit(EXIT_FAILURE, "malloc(icmp.payload)\n");
		}
		memcpy(payload, src->ether.type.ipv4.icmp.payload,
				src->ether.type.ipv4.icmp.payload_len);
		dst->ether.type.ipv4.icmp.payload = payload;
		dst->ether.type.ipv4.icmp.payload_len =
			src->ether.type.ipv4.icmp.payload_len;
	}

	/* tcp payload */
	if (src->ether.type.ipv4.tcp.payload &&
			src->ether.type.ipv4.tcp.payload_len) {
		char *payload = malloc(src->ether.type.ipv4.tcp.payload_len);
		if (!payload) {
			rte_exit(EXIT_FAILURE, "malloc(tcp.payload)\n");
		}
		memcpy(payload, src->ether.type.ipv4.tcp.payload,
				src->ether.type.ipv4.tcp.payload_len);
		dst->ether.type.ipv4.tcp.payload = payload;
		dst->ether.type.ipv4.tcp.payload_len =
			src->ether.type.ipv4.tcp.payload_len;
	}

	/* udp payload */
	if (src->ether.type.ipv4.udp.payload &&
			src->ether.type.ipv4.udp.payload_len) {
		char *payload = malloc(src->ether.type.ipv4.udp.payload_len);
		if (!payload) {
			rte_exit(EXIT_FAILURE, "malloc(udp.payload)\n");
		}
		memcpy(payload, src->ether.type.ipv4.udp.payload,
				src->ether.type.ipv4.udp.payload_len);
		dst->ether.type.ipv4.udp.payload = payload;
		dst->ether.type.ipv4.udp.payload_len =
			src->ether.type.ipv4.udp.payload_len;
	}

	/* ERROREOUS mutation func pointer */	if (src->erroneous.n_mutation) {
		/* erroneous mutation */
		mutation_func *func =
			malloc(sizeof(mutation_func) * src->erroneous.n_mutation);
		for (int i = 0; i < src->erroneous.n_mutation; i++) {
			func[i] = src->erroneous.func[i];
		}
		dst->erroneous.func = func;
		/* from config.yaml */
	}

	/* Extension slots (plugin-registered configs) */	/* memcpy above duplicated the ext[] pointers -- deep-copy each slot */
	for (uint8_t i = 0; i < src->n_ext; i++) {
		const struct injector_ext_cfg *desc = src->ext[i].desc;
		if (!desc || !src->ext[i].cfg)
			continue;

		void *cfg = malloc(desc->cfg_size);
		if (!cfg)
			rte_exit(EXIT_FAILURE, "malloc(ext[%u].cfg)\n", i);
		memcpy(cfg, src->ext[i].cfg, desc->cfg_size);

		if (desc->clone_cfg)
			desc->clone_cfg(src->ext[i].cfg, cfg);

		dst->ext[i].cfg = cfg;
		dst->ext[i].desc = desc;
	}
	dst->n_ext = src->n_ext;

	return 0;
}

/* Print the full injector configuration (Cnode + dist_ratio). */
void config_show(struct config *cfg)
{
	LOG_HINT("config       %p", cfg);
	LOG_HINT("  file map   %p", &cfg->cfm);
	LOG_PATH("    name     %s", cfg->cfm.name);
	LOG_PATH("    fd       %d", cfg->cfm.fd);
	LOG_PATH("    len      %zu", cfg->cfm.len);
	LOG_PATH("    addr     %p", cfg->cfm.addr);
	LOG_HINT("  root       %p", cfg->root);
	LOG_HINT("  cnode      %p", cfg->cnode);
}

/* Print the parsed YAML root node tree for debugging. */
void config_show_root(struct config *cfg)
{
	traverse(cfg->root, 0, print_pre, print_post, NULL);
}
