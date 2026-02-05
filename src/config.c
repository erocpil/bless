#include "bless.h"
#include "erroneous.h"
#include "server.h"
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

/* TODO more offloads */
static struct offload_table_item offload_table[] = {
	{ "ipv4", OF_IPV4 },
	{ "ipv6", OF_IPV6 },
	{ "tcp", OF_TCP },
	{ "udp", OF_UDP },
};

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
			/* FIXME free, log */
			return mapping;
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
				/* TODO */
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
			return sequence;
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

Node *find_by_path(Node *root, const char *path)
{
	char *copy = strdup(path);
	char *token = strtok(copy, ".");
	Node *cur = root;

	while (token && cur) {
		// 检查是否带有 [index]
		char key[128];
		int idx = -1;
		if (sscanf(token, "%127[^[][%d]", key, &idx) >= 1) {
			// 找 key
			Node *child = cur->child;
			while (child && (!child->key || strcmp(child->key, key) != 0)) {
				child = child->next;
			}

			if (!child) {
				LOG_ERR("no child found from key %s", key);
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
			LOG_ERR("sscanf(%s)", token);
			return NULL;
		}
		token = strtok(NULL, ".");
	}
	free(copy);

	return cur;
}

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

int config_check_file_map(struct config_file_map *cfm)
{
	LOG_HINT("检测配置文件 \"%s\"。", cfm->name);
	if (access(cfm->name, F_OK) != 0) {
		LOG_ERR("文件 \"%s\" 不存在。", cfm->name);
		return -1;
	}
	LOG_INFO("文件 \"%s\" 存在。", cfm->name);

	LOG_HINT("打开和映射文件 \"%s\"。", cfm->name);
	if (config_file_map_open(cfm)) {
		LOG_ERR("文件 \"%s\" 错误。", cfm->name);
		return -1;
	}

	LOG_HINT("读取文件 \"%s\"。", cfm->name);
	if (!cfm->len) {
		LOG_ERR("文件 \"%s\" 内容空。", cfm->name);
		goto err;
	}
	LOG_INFO("文件 \"%s\" 内容不空。", cfm->name);

	size_t binary_count = 0;
	for (size_t i = 0; i < cfm->len; i++) {
		unsigned char c = ((char*)cfm->addr)[i];
		// 允许常见控制符：换行(10)、回车(13)、制表符(9)
		if (!(isprint(c) || c == '\n' || c == '\r' || c == '\t')) {
			binary_count++;
		}
	}

	LOG_HINT("检测文件 \"%s\" 中的控制字符。", cfm->name);
	// 如果二进制字符占比超过 10%，认为是二进制文件
	if ((double)binary_count / cfm->len > 0.1) {
		LOG_WARN("文件 \"%s\" 中的控制字符超过 10%% 。", cfm->name);
		LOG_ERR("文件 \"%s\" 不是文本文件。", cfm->name);
		goto err;
	}
	LOG_HINT("文件 \"%s\" 是文本文件。", cfm->name);
	LOG_HINT("关闭文件 \"%s\" 。", cfm->name);
	close(cfm->fd);

	return 0;

err:
	config_file_unmap_close(cfm);
	free(cfm);

	return -1;
}

Node *config_init(char *f)
{
	Node *root = parse_yaml(f);
	if (!root) {
		fprintf(stderr, "Failed to parse YAML\n");
	}

	return root;
}

int config_exit(Node *root)
{
	if (root) {
		/* TODO traverse_free() */
		free(root);
	}

	return 0;
}

int config_parse_dpdk_internal(Node *node, int *targc, char ***targv, int i)
{
	char **argv = *targv;

	for (Node *n = node->child; n != NULL; n = n->next) {
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
				perror("malloc");
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

/** config_parse_system - set system cfg from root
*/
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
				strlen(node->value) < 16) {
			strncpy(cfg->theme, node->value, 16);
		} else {
			rte_exit(EXIT_FAILURE, "Invalid server theme: %s\n", node->value);
		}
	} else {
		strncpy(cfg->theme, "default", strlen("default") + 1);
	}

	if (config_parse_server(system_node, &cfg->server) < 0) {
		rte_exit(EXIT_FAILURE, "Invalid server arguments\n");
	}

	return 0;
}

int config_parse_server(Node *root, struct server *srv)
{
	if (!srv) {
		printf("NULL server\n");
		return -1;
	}

	/* 1. defaults */
	server_options_set_defaults(&srv->cfg);

	struct server_options_cfg *cfg = &srv->cfg;

	char *path = "server";
	Node *node = find_by_path(root, path);
	if (!node) {
		printf("No server found\n");
		return -1;
	}

	/* 2. YAML override */
	path = "server.options";
	node = find_by_path(root, path);
	if (node) {
		if (NODE_MAPPING == node->type) {
			for (Node *n = node->child; n; n = n->next) {
				const char *k = n->key, *v = n->value;
				if (strlen(n->key) > SERVER_KV_MAX ||
						strlen(n->value) > SERVER_KV_MAX) {
					printf("String too long %s => %s\n, omit.",
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
			printf("No valid server options found, use default\n");
		}
	} else {
		LOG_WARN("No server options found, use default");
	}

	/* 3. cfg -> kv[] */
	/* 每个 option 一项 */
	int n = build_civet_options(cfg, cfg->kv, 16);
	/* 4. kv[] -> civet const char * */
	int i = 0;
	for (i = 0; i < n; i++) {
		cfg->civet_opts[i * 2]     = cfg->kv[i].key;
		cfg->civet_opts[i * 2 + 1] = cfg->kv[i].val;
	}
	cfg->civet_opts[i * 2] = NULL;

	struct server_service *svc = &srv->svc;
	path = "server.service";
	node = find_by_path(root, path);
	if (node) {
		path = "server.service.websocket";
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
		path = "server.service.http";
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
		perror("malloc");
		exit(EXIT_FAILURE);
	}
	memset(argv, 0, sizeof(char*) * (MAX_EAL_PARAMS + 1));

	int i = 0;
	size_t len = strlen((*targv)[i]) + 1;
	argv[i] = malloc(len);
	if (!argv[i]) {
		perror("malloc");
		exit(EXIT_FAILURE);
	}
	strcpy(argv[i], (*targv)[i]);

	i = config_parse_dpdk_internal(node, &argc, &argv, ++i);

	// 添加分隔符 --
	argv[i] = strdup("--");
	if (!argv[i]) {
		perror("strdup");
		exit(EXIT_FAILURE);
	}

	path = "injector";
	node = find_by_path(root, path);
	if (!node) {
		return -1;
	}

	i = config_parse_dpdk_internal(node, &argc, &argv, ++i);

	path = "bless";
	node = find_by_path(root, path);
	if (!node) {
		return -1;
	}

	*targc = argc;
	*targv = argv;
	(*targv)[*targc] = NULL;  // 正确设置 argv 结尾

	return 0;
}

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
				perror("malloc");
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
				LOG_ERR("malloc(%s)", t->value);
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
			// 递归处理
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

	// base=0 自动识别 0x / 0 前缀
	unsigned long val = strtoul(s, &endptr, 0);
	if (errno != 0) {
		return 0;  // 溢出或其他错误
	}
	if (*endptr != '\0') {
		return 0;  // 存在无效字符
	}
	if (val > UINT16_MAX) {
		return 0;  // 超出 uint16_t 范围
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

	// base=0 自动识别 0x / 0 前缀
	unsigned long long val = strtoull(s, &endptr, 0);
	if (errno != 0) {
		return 0;  // 溢出或其他错误
	}
	if (*endptr != '\0') {
		return 0;  // 存在无效字符
	}
	if (val > UINT32_MAX) {
		return 0;  // 超出 uint32_t 范围
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
	int val = 0;

	switch (node->type) {
		case NODE_SCALAR:
			val = atoi(node->value);
			memcpy(array, &val, size);
			n++;
			break;
		case NODE_SEQUENCE:
			for (Node *i = node->child; i != NULL && n <= limit;
					i = i->next, n++) {
				val = atoi(i->value);
				if (sizeof(uint16_t) == size) {
					parse_uint16(i->value, (uint16_t*)((char*)array + size * n));
				} else {
					parse_uint32(i->value, (uint32_t*)((char*)array + size * n));
				}
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
				if (bless_parse_port_range(node->value, &p, &ra)) {
					goto ERROR;
				}
				port[0] = p;
				*range = ra;
				n++;
			}
			break;
		case NODE_SEQUENCE:
			for (Node *i = node->child; i != NULL && n <= limit;
					i = i->next, n++) {
				port[n] = atoi(i->value);
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

static int config_parse_sequence_ipv4_vni_to_array(Node *node, uint32_t *array,
		uint32_t *vni, int size, uint16_t limit)
{
	int n = 0;
	int val = 0;
	char *saveptr = NULL;
	char *token = NULL;
	char *dup = NULL;

	switch (node->type) {
		case NODE_SCALAR:
			dup = strdup(node->value);
			/* vtep ipv4 */
			token = strtok_r(dup, ":", &saveptr);
			if (!token) {
				LOG_ERR("no vni");
				exit(-1);
			}
			inet_pton(AF_INET, token, (char*)array + size * n);
			/* vni */
			token = strtok_r(NULL, ":", &saveptr);
			if (!token) {
				LOG_ERR("no vni");
				exit(-1);
			}
			val = atoi(token);
			memcpy(vni, &val, sizeof(uint32_t));
			/* TODO free(dup) ? */
			n++;
			break;
		case NODE_SEQUENCE:
			for (Node *i = node->child; i != NULL && n <= limit;
					i = i->next, n++) {
				dup = strdup(i->value);
				/* vtep ipv4 */
				token = strtok_r(dup, ":", &saveptr);
				if (!token) {
					LOG_ERR("no vni");
					exit(-1);
				}
				inet_pton(AF_INET, token, &array[n]);
				/* vni */
				token = strtok_r(NULL, ":", &saveptr);
				if (!token) {
					LOG_ERR("no vni");
					exit(-1);
				}
				val = atoi(token);
				memcpy(&vni[n], &val, sizeof(uint32_t));
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
				if (bless_parse_ip_range(node->value, &ipv4, &ra)) {
					break;
				}
				addr[0] = ipv4;
				*range = ra;
				n++;
			}
			break;
		case NODE_SEQUENCE:
			for (Node *i = node->child; i != NULL && n <= limit;
					i = i->next, n++) {
				/* FIXME */
				if (inet_pton(AF_INET, i->value, &addr[n]) != 1) {
					return -1;
				}
				// bless_print_ipv4(addr[n]);
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
	int val = 0;

	switch (node->type) {
		case NODE_SCALAR:
			inet_pton(AF_INET, node->value, array);
			n++;
			break;
		case NODE_SEQUENCE:
			for (Node *i = node->child; i != NULL && n <= limit;
					i = i->next, n++) {
				/* FIXME */
				if (inet_pton(AF_INET, i->value, &val) != 1) {
					return -1;
				}
				inet_pton(AF_INET, i->value, (char*)array + size * n);
			}
			break;
		case NODE_MAPPING:
			break;
		default:
			break;
	}

	return n;
}

static int config_parse_bless_ether(Node *root, Cnode *cnode)
{
	char *path = NULL;
	Node *node = NULL;

	/* TODO */
	int64_t mtu = 0;
	path = "bless.ether.mtu";
	node = find_by_path(root, path);
	if (node) {
		mtu = atoll(node->value);
		if (mtu < 46) {
			LOG_INFO("set invalid mtu value %lu to 0(disabled)", mtu);
			mtu = 0;
		} else if (mtu > 1500) {
			LOG_INFO("set invalid mtu value %lu to 0(disabled)", mtu);
			mtu = 1500;
		}
	}
	cnode->ether.mtu = (uint16_t)mtu;

	path = "bless.ether.copy-payload";
	cnode->ether.copy_payload = 0;
	node = find_by_path(root, path);
	if (node || strlen(node->value) || (
				!strcmp("true", node->value) ||
				!strcmp("TRUE", node->value)
				)
	   ) {
		cnode->ether.copy_payload = 1;
	}

	path = "bless.ether.dst";
	node = find_by_path(root, path);
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
		LOG_ERR("Invalid dst mac address");
		exit(-1);
	}
	cnode->ether.n_dst = 1;

	path = "bless.ether.src";
	node = find_by_path(root, path);
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
	LOG_ERR("yaml config parse error %s: %s", path, node->value);
	exit(-1);
}

static int config_parse_bless_vxlan_ether(Node *root, Cnode *cnode)
{
	char *path = NULL;

	path = "bless.vxlan.ether.dst";
	Node *node = find_by_path(root, path);
	if (!node) {
		goto ERROR;
	}

	cnode->vxlan.ether.n_dst = 0;
	if (node->value && strlen(node->value) &&
			(rte_ether_unformat_addr(node->value,
									 (struct rte_ether_addr*)&
									 (cnode->vxlan.ether.dst)) == 0)) {
		// bless_print_mac((struct rte_ether_addr*)cnode->vxlan.ether.dst);
		cnode->vxlan.ether.n_dst = 1;
	} else {
		goto ERROR;
	}

	path = "bless.vxlan.ether.src";
	node = find_by_path(root, path);
	if (!node) {
		LOG_INFO("no vxlan src mac address node provided");
		cnode->vxlan.ether.n_src = 0;
	} else {
		if (node->value && strlen(node->value) &&
				(rte_ether_unformat_addr(node->value,
										 (struct rte_ether_addr*)&
										 (cnode->vxlan.ether.src)) == 0)) {
			// bless_print_mac((struct rte_ether_addr*)cnode->vxlan.ether.src);
			cnode->vxlan.ether.n_src = 1;
		} else {
			LOG_WARN("Invalid vxlan src mac address, "
					"injector will try local port");
			cnode->vxlan.ether.n_src = 0;
		}
	}

	return 1;

ERROR:
	LOG_ERR("yaml config parse error %s: %s", path, node->value);
	exit(-1);
}

static int config_parse_bless_ether_type_arp(Node *root, Cnode *cnode)
{
	int n = 0;
	char *path = NULL;

	path = "bless.ether.type.arp.src";
	Node *node = find_by_path(root, path);
	if (!node) {
		goto ERROR;
	}

	n = config_parse_sequence_ipv4_to_array(node, cnode->ether.type.arp.src,
			sizeof(uint32_t), IP_ADDR_MAX);
	if (n > 0) {
		cnode->ether.type.arp.n_src = n;
	} else {
		goto ERROR;
	}
	/*
	   printf("  src(%d):\n", n);
	   for (int i = 0; i < n; i++) {
	   bless_print_ipv4(cnode->ether.type.arp.src[i]);
	   }
	   printf("\n");
	   */
	path = "bless.ether.type.arp.dst";
	node = find_by_path(root, path);
	if (!node) {
		LOG_WARN("path %s not found", path);
		return -1;
	}
	n = config_parse_sequence_ipv4_to_array(node, cnode->ether.type.arp.dst,
			sizeof(uint32_t), IP_ADDR_MAX);
	if (n > 0) {
		cnode->ether.type.arp.n_dst = n;
	} else {
		goto ERROR;
	}
	/*
	   printf("  dst:\n");
	   for (int i = 0; i < n; i++) {
	   bless_print_ipv4(cnode->ether.type.arp.dst[i]);
	   }
	   printf("\n");
	   */

	return n;

ERROR:
	LOG_ERR("yaml config parse error %s: %s", path, node->value);
	exit(-1);
}

static int config_parse_bless_ether_type_ipv4_icmp(Node *root, Cnode *cnode)
{
	int n = 0;
	char *path = NULL;

	path = "bless.ether.type.ipv4.icmp.ident";
	Node *node = find_by_path(root, path);
	if (!node) {
		goto ERROR;
	}

	n = config_parse_sequence_to_array(node,
			cnode->ether.type.ipv4.icmp.ident, sizeof(uint16_t), PORT_MAX);
	if (n > 0) {
		cnode->ether.type.ipv4.icmp.n_ident = n;
	} else {
		goto ERROR;
	}
	/*
	   printf("  ident:\n");
	   for (int i = 0; i < n; i++) {
	   printf("%u 0x%x ", cnode->ether.type.ipv4.icmp.ident[i], cnode->ether.type.ipv4.icmp.ident[i]);
	   }
	   printf("\n");
	   */

	path = "bless.ether.type.ipv4.icmp.payload";
	node = find_by_path(root, path);
	if (node && node->value && strlen(node->value)) {
		char *payload = strdup(node->value);
		cnode->ether.type.ipv4.icmp.payload = payload;
		cnode->ether.type.ipv4.icmp.payload_len = strlen(payload) + 1;
	}

	return n;

ERROR:
	LOG_ERR("yaml config parse error %s: %s", path, node->value);
	exit(-1);
}

static int config_parse_bless_ether_type_ip_tcp(Node *root, Cnode *cnode)
{
	int n = 0;
	char *path = NULL;

	path = "bless.ether.type.ipv4.tcp.src";
	Node *node = find_by_path(root, path);
	if (!node) {
		goto ERROR;
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

	path = "bless.ether.type.ipv4.tcp.dst";
	node = find_by_path(root, path);
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

	path = "bless.ether.type.ipv4.tcp.payload";
	node = find_by_path(root, path);
	if (node && node->value && strlen(node->value)) {
		char *payload = strdup(node->value);
		cnode->ether.type.ipv4.tcp.payload = payload;
		cnode->ether.type.ipv4.tcp.payload_len = strlen(payload) + 1;
	}

	return n;

ERROR:
	LOG_ERR("yaml config parse error %s: %s\n", path, node->value);
	exit(-1);
}

static int config_parse_bless_ether_type_ip_udp(Node *root, Cnode *cnode)
{
	int n = 0;
	char *path = NULL;

	path = "bless.ether.type.ipv4.udp.src";
	Node *node = find_by_path(root, path);
	if (!node) {
		goto ERROR;
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

	path = "bless.ether.type.ipv4.udp.dst";
	node = find_by_path(root, path);
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
	/*
	   printf("  dst:\n");
	   for (int i = 0; i < n; i++) {
	   printf("%u ", cnode->ether.type.ipv4.udp.dst[i]);
	   }
	   printf("\n");
	   */

	path = "bless.ether.type.ipv4.udp.payload";
	node = find_by_path(root, path);
	if (node && node->value && strlen(node->value)) {
		char *payload = strdup(node->value);
		cnode->ether.type.ipv4.udp.payload = payload;
		cnode->ether.type.ipv4.udp.payload_len = strlen(payload) + 1;
	}

	return n;

ERROR:
	LOG_ERR("yaml config parse error %s: %s", path, node->value);
	exit(-1);
}

static int config_parse_bless_ether_type_ipv4(Node *root, Cnode *cnode)
{
	int n = 0;
	char *path = NULL;

	path = "bless.ether.type.ipv4.src";
	Node *node = find_by_path(root, path);
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
	/*
	   printf("  src:\n");
	   for (int i = 1; i <= cnode->ether.type.ipv4.n_src; i++) {
	   bless_print_ipv4(cnode->ether.type.ipv4.src[i]);
	   }
	   printf("\n");
	   */

	path = "bless.ether.type.ipv4.dst";
	node = find_by_path(root, path);
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
	/*
	   printf("  dst:\n");
	   for (int i = 0; i <= cnode->ether.type.ipv4.n_dst; i++) {
	   bless_print_ipv4(cnode->ether.type.ipv4.dst[i]);
	   }
	   printf("\n");
	   */

	return n;

ERROR:
	LOG_ERR("yaml config parse error %s: %s", path, node->value);
	exit(-1);
}

static int config_parse_bless_vxlan(Node *root, Cnode *cnode)
{
	int n = 0;
	char *path = NULL;
	Node *node = NULL;

	path = "bless.vxlan.enable";
	cnode->vxlan.enable = 1;
	node = find_by_path(root, path);
	if (!node || !strlen(node->value) || (
				strcmp("true", node->value) &&
				strcmp("TRUE", node->value)
				)
	   ) {
		LOG_HINT("vxlan disabled");
		cnode->vxlan.enable = 0;
	}

	path = "bless.vxlan.ratio";
	node = find_by_path(root, path);
	if (!node || !strlen(node->value)) {
		LOG_HINT("vxlan disabled");
		cnode->vxlan.enable = 0;
	}

	/* XXX do not return, mutation may use vxlan */
	cnode->vxlan.ratio = atoi(node->value);
	if (cnode->vxlan.ratio < 0 || cnode->vxlan.ratio > 100) {
		LOG_HINT("vxlan disabled");
		cnode->vxlan.enable = 0;
		// return 1;
	}

	n = config_parse_bless_vxlan_ether(root, cnode);
	// bless_print_mac((struct rte_ether_addr*)cnode->vxlan.ether.src);
	// bless_print_mac((struct rte_ether_addr*)cnode->vxlan.ether.dst);

	path = "bless.vxlan.ether.type.ipv4.src";
	node = find_by_path(root, path);
	if (!node) {
		goto ERROR;
	}

	n = config_parse_sequence_ipv4_vni_to_array(node,
			cnode->vxlan.ether.type.ipv4.src, cnode->vxlan.ether.type.ipv4.vni,
			sizeof(uint16_t), PORT_MAX);
	// n = config_parse_sequence_ipv4_to_array(node, cnode->vxlan.ether.type.ipv4.src, sizeof(uint32_t), IP_ADDR_MAX);
	if (n > 0) {
		cnode->vxlan.ether.type.ipv4.n_src = n;
	} else {
		goto ERROR;
	}
	/*
	   printf("  src:\n");
	   for (int i = 0; i < n; i++) {
	   bless_print_ipv4(cnode->vxlan.ether.type.ipv4.src[i]);
	   printf("  vni: %u\n", cnode->vxlan.ether.type.ipv4.vni[i]);
	   }
	   printf("\n");
	   */

	path = "bless.vxlan.ether.type.ipv4.dst";
	node = find_by_path(root, path);
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
	/*
	   printf("  dst:\n");
	   for (int i = 0; i < n; i++) {
	   bless_print_ipv4(cnode->vxlan.ether.type.ipv4.dst[i]);
	   }
	   printf("\n");
	   */

	path = "bless.vxlan.ether.type.ipv4.udp.src";
	node = find_by_path(root, path);
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
	/*
	   printf("  src:\n");
	   for (int i = 0; i < n; i++) {
	   printf("%u ", cnode->vxlan.ether.type.ipv4.udp.src[i]);
	   }
	   printf("\n");
	   */

	path = "bless.vxlan.ether.type.ipv4.udp.dst";
	node = find_by_path(root, path);
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
	/*
	   printf("  dst:\n");
	   for (int i = 0; i < n; i++) {
	   printf("%u ", cnode->vxlan.ether.type.ipv4.udp.dst[i]);
	   }
	   printf("\n");
	   */

	return n;

ERROR:
	LOG_ERR("yaml config parse error %s: %s", path, node->value);
	exit(-1);
}

static int config_parse_bless_erroneous(Node *root, Cnode *cnode)
{
	char *path = NULL;
	Node *node = NULL;

	path = "bless.erroneous.ratio";
	node = find_by_path(root, path);
	if (!node || !strlen(node->value) || '0' == node->value[0]) {
		cnode->erroneous.ratio = 0;
		LOG_INFO("erroneous disabled");
		return 1;
	}
	cnode->erroneous.ratio = atoi(node->value);
	if (!cnode->erroneous.ratio) {
		LOG_INFO("erroneous disabled due to no ratio");
		return 1;
	}
	path = "bless.erroneous.class";
	node = find_by_path(root, path);
	if (!node || !node->child ) {
		cnode->erroneous.ratio = 0;
		LOG_INFO("erroneous disabled due to no class");
		return 1;
	}

	int res = 0;
	int n_clas = 0;
	for (Node *n = node->child; n; n = n->next) {
		struct ec_clas *clas = &cnode->erroneous.clas[n_clas++];
		clas->name = strdup(n->key);
		int n_type = 0;
		for (Node *i = n->child; i; i = i->next) {
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
		// printf("  %d_%s: ", i, clas->name);
		for (int j = 0; j < clas->n_type; j++) {
			// printf(" %s ", clas->type[j]);
			mutation_func f = find_mutation_func(clas->name, clas->type[j]);
			if (!f) {
				LOG_ERR(" no mutation `%s:%s'\n", clas->name, clas->type[j]);
				exit(-1);
			}
			func[pos++] = f;
		}
		// printf("\n");
	}
	cnode->erroneous.func = func;
	// printf("\n");

	return res;
}

Cnode *config_parse_bless(Node *root)
{
	struct Cnode *cnode = malloc(sizeof(struct Cnode));
	memset(cnode, 0, sizeof(struct Cnode));

	char *path = "bless";
	Node *node = find_by_path(root, path);
	if (!node) {
		goto ERROR;
	}

	/* hw offload */
	path = "bless.hw-offload";
	node = find_by_path(root, path);
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
	config_parse_bless_ether(root, cnode);
	path = "bless.ether.dst";
	node = find_by_path(root, path);
	if (!node) {
		goto ERROR;
	}

	config_parse_bless_ether_type_arp(root, cnode);
	config_parse_bless_ether_type_ipv4(root, cnode);
	config_parse_bless_ether_type_ipv4_icmp(root, cnode);
	config_parse_bless_ether_type_ip_tcp(root, cnode);
	config_parse_bless_ether_type_ip_udp(root, cnode);
	config_parse_bless_vxlan(root, cnode);
	config_parse_bless_erroneous(root, cnode);

	return cnode;

ERROR:
	LOG_ERR("yaml config parse error %s: %s", path, node->value);
	exit(-1);
}

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

	/* TODO vxlan payload, not used */

	if (src->erroneous.n_mutation) {
		/* erroneous mutation */
		mutation_func *func =
			malloc(sizeof(mutation_func) * src->erroneous.n_mutation);
		for (int i = 0; i < src->erroneous.n_mutation; i++) {
			func[i] = src->erroneous.func[i];
		}
		dst->erroneous.func = func;
		/* from config.yaml */
	}

	return 0;
}

void config_show(struct config *cfg)
{
	LOG_HINT("config %p", cfg);
	LOG_HINT("  file map %p", &cfg->cfm);
	LOG_PATH("    name   %s", cfg->cfm.name);
	LOG_PATH("    fd     %d", cfg->cfm.fd);
	LOG_PATH("    len    %lu", cfg->cfm.len);
	LOG_PATH("    addr   %p", cfg->cfm.addr);
	LOG_HINT("  root     %p", cfg->root);
	LOG_HINT("  cnode    %p", cfg->cnode);
}

void config_show_root(struct config *cfg)
{
	traverse(cfg->root, 0, print_pre, print_post, NULL);
}
