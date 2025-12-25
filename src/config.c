#include "config.h"
#include "bless.h"
#include "erroneous.h"
#include "server.h"

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
			return mapping;
		}
		if (event.type == YAML_MAPPING_END_EVENT) {
			yaml_event_delete(&event);
			break;
		}
		if (event.type == YAML_SCALAR_EVENT) {
			char *key = strdup((char *)event.data.scalar.value);
			yaml_event_delete(&event);
			Node *val = parse_node(parser);
			if (val) {
				val->key = key;
				add_child(mapping, val);
			}
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
			Node *child = create_node(NULL, (char *)event.data.scalar.value, NODE_SCALAR);
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
			node = create_node(NULL, (char *)event.data.scalar.value, NODE_SCALAR);
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
		perror("fopen");
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

void free_tree(Node *node) {
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

void traverse(Node *node, int depth, NodeVisitor pre, NodeVisitor post, void *userdata)
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

void traverse_node(Node *node, int depth, NodeVisitor pre, NodeVisitor post, void *userdata)
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

void print_pre(Node *node, int depth, void *userdata) {
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
	if (node->type == NODE_MAPPING) {
		for (int i = 0; i < depth; i++) printf("  ");
		printf("}\n");
	} else if (node->type == NODE_SEQUENCE) {
		for (int i = 0; i < depth; i++) printf("  ");
		printf("]\n");
	}
}

// 路径解析：例如 "dpdk[1].app"
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
				free(copy);
				return NULL;
			}
			cur = child;

			// 如果是数组，取索引
			if (idx >= 0 && cur->type == NODE_SEQUENCE) {
				Node *elem = cur->child;
				for (int i = 0; elem && i < idx; i++) {
					elem = elem->next;
				}
				cur = elem;
			}
		}
		token = strtok(NULL, ".");
	}
	free(copy);

	return cur;
}

#define BUFFER_SIZE 1024
int config_check_file(char *filename)
{
	// 检查文件是否存在
	if (access(filename, F_OK) != 0) {
		printf("文件 \"%s\" 不存在。\n", filename);
		return EXIT_FAILURE;
	}
	printf("文件 \"%s\" 存在。\n", filename);

	int res = 1;
	FILE *fp = fopen(filename, "rb");
	if (!fp) {
		perror("fopen");
		res = -1;
		goto DONE;
	}
	printf("打开文件 \"%s\"。\n", filename);

	unsigned char buffer[BUFFER_SIZE];
	size_t nread = fread(buffer, 1, BUFFER_SIZE, fp);
	fclose(fp);
	printf("读取并关闭文件 \"%s\"。\n", filename);

	if (nread == 0) {
		// 空文件可以认为是文本文件
		res = -1;
	}
	printf("文件 \"%s\" 内容不为空。\n", filename);

	size_t binary_count = 0;
	for (size_t i = 0; i < nread; i++) {
		unsigned char c = buffer[i];
		// 允许常见控制符：换行(10)、回车(13)、制表符(9)
		if (!(isprint(c) || c == '\n' || c == '\r' || c == '\t')) {
			binary_count++;
		}
	}
	printf("检测文件 \"%s\" 中的控制字符。\n", filename);

	// 如果二进制字符占比超过 10%，认为是二进制文件
	if ((double)binary_count / nread > 0.1) {
		printf("文件 \"%s\" 中的控制字符超过 10%% 。\n", filename);
		res = 0;
	}

DONE:
	if (res == -1) {
		printf("无法打开文件 \"%s\"。\n", filename);
		return -EXIT_FAILURE;
	} else if (res == 1) {
		printf("文件 \"%s\" 是文本文件。\n", filename);
	} else {
		printf("文件 \"%s\" 不是文本文件。\n", filename);
	}

	return res;
}

Node *config_init(char *f)
{
	Node *root = parse_yaml(f);
	if (!root) {
		fprintf(stderr, "Failed to parse YAML\n");
	}

	return root;

	/* XXX test */
	printf("===== Traverse All =====\n");
	traverse(root, 0, print_pre, print_post, NULL);

	printf("\n===== Traverse Path:  =====\n");
	char *path = "bless.vxlan.ether.type.udp.dst-range";
	Node *target = find_by_path(root, path);
	if (target) {
		printf("print %s: \"%s\" %lu\n", path, target->value, strlen(target->value));
		traverse(target, 0, print_pre, print_post, NULL);
	} else {
		printf("Path not found\n");
	}

	path = "bless.erroneous.class.ipv4";
	target = find_by_path(root, path);
	if (target) {
		printf("print %s:\n", path);
		if (NODE_SCALAR == target->type && target->value) {
			printf("\"%s\"(%lu)\n", target->value, strlen(target->value));
		} else {
			traverse_node(target, 0, print_pre, print_post, NULL);
		}
	} else {
		printf("Path not found\n");
	}

	return 0;
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
		printf("%s: %s\n", n->key, n->value);

		if (n->type == NODE_SCALAR) {
			if (!n->value || !strcmp(n->value, "null") || !strcmp(n->value, "NULL")) {
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
			// printf("malloc(argv[%d]) %p %p\n", i, argv[i], &argv[i]);

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
				len = 1 + 1 + strlen(n->key) + 1;   // "-k "
			} else {
				len = 2 + strlen(n->key) + 1;       // "--key="
			}

			Node *t = n->child;
			while (t) {
				len += strlen(t->value) + 1;  // 加上逗号或空格
				t = t->next;
			}
			len += 1; // '\0'

			t = n->child;
			while (t) {
				argv[i] = malloc(len);
				if (!argv[i]) {
					perror("malloc");
					exit(EXIT_FAILURE);
				}
				// printf("malloc(argv[%d]) %p %p\n", i, argv[i], &argv[i]);

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
			// NODE_MAPPING 其它情况略过
		}

		i++;
	}

	*targc = i;
	*targv = argv;
	return i;
}

int config_parse_server(Node *root, struct server_options_cfg *cfg)
{
	if (!cfg) return -1;

	/* 1. defaults */
	server_options_set_defaults(cfg);

	char *path = "server.options";
	Node *node = find_by_path(root, path);
	if (!node) {
		printf("No server found\n");
		return 0;
	}

	/* 2. YAML override */
	path = "server.options";
	node = find_by_path(root, path);
	if (node) {
		if (NODE_MAPPING == node->type) {
			printf("%s:\n", path);
			for (Node *n = node->child; n; n = n->next) {
				const char *k = n->key, *v = n->value;
				if (strlen(n->key) > SERVER_KV_MAX || strlen(n->value) > SERVER_KV_MAX) {
					printf("String too long %s => %s\n, omit.", n->key, n->value);
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
		printf("No server options found, use default\n");
	}
	/* 3. cfg -> kv[] */
	// struct civet_kv kv[16];              /* 每个 option 一项 */
	int n = build_civet_options(cfg, cfg->kv, 16);
	/* 4. kv[] -> civet const char * */
	// const char *civet_opts[32 + 1];      /* key,value,...,NULL */
	int i = 0;
	for (i = 0; i < n; i++) {
		cfg->civet_opts[i * 2]     = cfg->kv[i].key;
		cfg->civet_opts[i * 2 + 1] = cfg->kv[i].val;
	}
	cfg->civet_opts[i * 2] = NULL;

	for (int i = 0; i < n; i++) {
		printf("%s => %s\n", cfg->civet_opts[i * 2], cfg->civet_opts[i * 2 + 1]);
	}

	path = "server.service";
	node = find_by_path(root, path);
	if (node) {
		path = "server.service.websocket";
		node = find_by_path(root, path);
		if (NODE_SCALAR == node->type) {
			printf("%s %s\n", path, node->value);
			int len = strlen(node->value);
			if (len > 0) {
			cfg->uri = (char*)malloc(len + 1);
			strncpy(cfg->uri, node->value, len + 1);
			} else {
				cfg->uri = NULL;
			}
		}
		path = "server.service.http";
		node = find_by_path(root, path);
		if (NODE_SEQUENCE == node->type) {
			printf("%s:\n", path);
			for (Node *n = node->child; n; n = n->next) {
				printf("  %s\n", n->value);
			}
		}
	} else {
		printf("No server service found\n");
	}

	return 0;
}

#define MAX_EAL_PARAMS 128

int config_parse_dpdk(Node *root, int *targc, char ***targv)
{
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
	// printf("malloc(argv) %p\n", argv);

	int i = 0;
	size_t len = strlen((*targv)[i]) + 1;
	argv[i] = malloc(len);
	if (!argv[i]) {
		perror("malloc");
		exit(EXIT_FAILURE);
	}
	// printf("malloc(argv[%d]) %p %p\n", i, argv[i], &argv[i]);
	strcpy(argv[i], (*targv)[i]);

	i = config_parse_dpdk_internal(node, &argc, &argv, ++i);

	// 添加分隔符 --
	argv[i] = strdup("--");
	if (!argv[i]) {
		perror("strdup");
		exit(EXIT_FAILURE);
	}
	// printf("malloc(argv[%d]) %p %p\n", i, argv[i], &argv[i]);

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

	// i = config_parse_generic(node, &argc, &argv, ++i, path);

	*targc = argc;
	*targv = argv;
	(*targv)[*targc] = NULL;  // 正确设置 argv 结尾

	return 0;
}

int config_parse_generic(Node *node, int *targc, char ***targv, int i, const char *prefix)
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
			if (!n->value || !strcmp(n->value, "null") || !strcmp(n->value, "NULL")) {
				continue;
			}

			size_t len = 2 + strlen(fullkey) + 1 + strlen(n->value) + 1; // --key=value
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
				perror("malloc");
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

	unsigned long val = strtoul(s, &endptr, 0);  // base=0 自动识别 0x / 0 前缀
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

	unsigned long long val = strtoull(s, &endptr, 0);  // base=0 自动识别 0x / 0 前缀
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
static int config_parse_sequence_to_array(Node *node, void *array, int size, uint16_t limit)
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
			for (Node *i = node->child; i != NULL && n <= limit; i = i->next, n++) {
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

static int config_parse_port_maybe_range_to_array(Node *node, uint16_t *port, int32_t *range, uint16_t limit)
{
	int n = 0;

	switch (node->type) {
		case NODE_SCALAR:
			{
				uint16_t p;
				int32_t ra;
				if (bless_parse_port_range(node->value, &p, &ra)) {
					break;
				}
				port[0] = p;
				*range = ra;
				n++;
			}
			break;
		case NODE_SEQUENCE:
			for (Node *i = node->child; i != NULL && n <= limit; i = i->next, n++) {
				port[n] = atoi(i->value);
				printf("%u ", port[n]);
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

static int config_parse_sequence_ipv4_vni_to_array(Node *node, uint32_t *array, uint32_t *vni, int size, uint16_t limit)
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
				printf("no vni\n");
				exit(-1);
			}
			inet_pton(AF_INET, token, (char*)array + size * n);
			/* vni */
			token = strtok_r(NULL, ":", &saveptr);
			if (!token) {
				printf("no vni\n");
				exit(-1);
			}
			val = atoi(token);
			memcpy(vni, &val, sizeof(uint32_t));
			/* TODO free(dup) ? */
			n++;
			break;
		case NODE_SEQUENCE:
			for (Node *i = node->child; i != NULL && n <= limit; i = i->next, n++) {
				dup = strdup(i->value);
				/* vtep ipv4 */
				token = strtok_r(dup, ":", &saveptr);
				if (!token) {
					printf("no vni\n");
					exit(-1);
				}
				inet_pton(AF_INET, token, &array[n]);
				/* vni */
				token = strtok_r(NULL, ":", &saveptr);
				if (!token) {
					printf("no vni\n");
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
static int config_parse_ipv4_maybe_range_to_array(Node *node, uint32_t *addr, int64_t *range, uint32_t limit)
{
	int n = 0;

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
			for (Node *i = node->child; i != NULL && n <= limit; i = i->next, n++) {
				/* FIXME */
				if (inet_pton(AF_INET, i->value, &addr[n]) != 1) {
					return -1;
				}
				bless_print_ipv4(addr[n]);
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

static int config_parse_sequence_ipv4_to_array(Node *node, void *array, int size, uint16_t limit)
{
	int n = 0;
	int val = 0;

	switch (node->type) {
		case NODE_SCALAR:
			inet_pton(AF_INET, node->value, array);
			n++;
			break;
		case NODE_SEQUENCE:
			for (Node *i = node->child; i != NULL && n <= limit; i = i->next, n++) {
				/* FIXME */
				if (inet_pton(AF_INET, i->value, &val) != 1) {
					return -1;
				}
				// bless_print_ipv4(val);
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
	int64_t mtu = 64 - 8;
	path = "bless.ether.mtu";
	node = find_by_path(root, path);
	if (node) {
		mtu = atoll(node->value);
		if (mtu < 64 - 8) {
			mtu = 64 - 8;
		} else if (mtu > 1500) {
			mtu = 1500;
		}
	}
	cnode->ether.mtu = (uint16_t)mtu;
	printf("%s: %d\n", path, cnode->ether.mtu);

	path = "bless.ether.dst";
	node = find_by_path(root, path);
	if (!node) {
		printf("no dst mac address provided.\n");
		exit(-1);
	}
	printf("%s: %s\n", path, node->value);

	/* take only 1 mac address */
	if (node->value && strlen(node->value) && NODE_SCALAR == node->type &&
			(rte_ether_unformat_addr(node->value, (struct rte_ether_addr*)&(cnode->ether.dst)) == 0)) {
		bless_print_mac((struct rte_ether_addr*)cnode->ether.dst);
	} else {
		printf("Invalid dst mac address.\n");
		exit(-1);
	}
	cnode->ether.n_dst = 1;

	path = "bless.ether.src";
	node = find_by_path(root, path);
	if (!node) {
		printf("no src mac address node provided.\n");
		cnode->ether.n_src = 0;
	} else {
		printf("%s: %s\n", path, node->value);
		if (node->value && strlen(node->value) && NODE_SCALAR == node->type &&
				(rte_ether_unformat_addr(node->value, (struct rte_ether_addr*)&(cnode->ether.src)) == 0)) {
			bless_print_mac((struct rte_ether_addr*)cnode->ether.src);
			cnode->ether.n_src = 1;
		} else {
			printf("Invalid src mac address, injector will try it's local port.\n");
			cnode->ether.n_src = 0;
		}
	}

	return 1;
}

static int config_parse_bless_vxlan_ether(Node *root, Cnode *cnode)
{
	char *path = NULL;

	path = "bless.vxlan.ether.dst";
	Node *node = find_by_path(root, path);
	if (!node) {
		printf("Invalid vxlan dst mac address.\n");
		exit(-1);
	}
	printf("%s: %s\n", path, node->value);

	if (node->value && strlen(node->value) &&
			(rte_ether_unformat_addr(node->value, (struct rte_ether_addr*)&(cnode->vxlan.ether.dst)) == 0)) {
		bless_print_mac((struct rte_ether_addr*)cnode->vxlan.ether.dst);
	} else {
		printf("Invalid vxlan dst mac address.\n");
		exit(-1);
	}

	path = "bless.vxlan.ether.src";
	node = find_by_path(root, path);
	if (!node) {
		printf("no vxlan src mac address node provided.\n");
		cnode->vxlan.ether.n_src = 0;
	} else {
		printf("%s: %s\n", path, node->value);
		if (node->value && strlen(node->value) &&
				(rte_ether_unformat_addr(node->value, (struct rte_ether_addr*)&(cnode->vxlan.ether.src)) == 0)) {
			bless_print_mac((struct rte_ether_addr*)cnode->vxlan.ether.src);
			cnode->vxlan.ether.n_src = 1;
		} else {
			printf("Invalid vxlan src mac address, injector will try it's local port.\n");
			cnode->vxlan.ether.n_src = 1;
		}
	}

	return 1;
}

static int config_parse_bless_ether_type_arp(Node *root, Cnode *cnode)
{
	int n = 0;
	char *path = NULL;

	path = "bless.ether.type.arp.src";
	Node *node = find_by_path(root, path);
	if (!node) {
		return -1;
	}
	printf("%s: %s\n", path, node->value);

	n = config_parse_sequence_ipv4_to_array(node, cnode->ether.type.arp.src, sizeof(uint32_t), IP_ADDR_MAX);
	if (n > 0) {
		cnode->ether.type.arp.n_dst = n;
	} else {
		goto ERROR;
	}
	printf("  src:\n");
	for (int i = 0; i < n; i++) {
		bless_print_ipv4(cnode->ether.type.arp.src[i]);
	}
	printf("\n");
	path = "bless.ether.type.arp.dst";
	node = find_by_path(root, path);
	if (!node) {
		return -1;
	}
	printf("%s: %s\n", path, node->value);
	n = config_parse_sequence_ipv4_to_array(node, cnode->ether.type.arp.dst, sizeof(uint32_t), IP_ADDR_MAX);
	if (n > 0) {
		cnode->ether.type.arp.n_dst = n;
	} else {
		goto ERROR;
	}
	printf("  dst:\n");
	for (int i = 0; i < n; i++) {
		bless_print_ipv4(cnode->ether.type.arp.dst[i]);
	}
	printf("\n");

	return n;

ERROR:
	printf("yaml config parse error %s: %s\n", path, node->value);
	exit(-1);
}

static int config_parse_bless_ether_type_ipv4_icmp(Node *root, Cnode *cnode)
{
	int n = 0;
	char *path = NULL;

	path = "bless.ether.type.ipv4.icmp.ident";
	Node *node = find_by_path(root, path);
	if (!node) {
		return -1;
	}
	printf("%s: %s\n", path, node->value);

	n = config_parse_sequence_to_array(node, cnode->ether.type.ipv4.icmp.ident, sizeof(uint16_t), PORT_MAX);
	if (n > 0) {
		cnode->ether.type.ipv4.icmp.n_ident = n;
	} else {
		goto ERROR;
	}
	printf("  ident:\n");
	for (int i = 0; i < n; i++) {
		printf("%u 0x%x ", cnode->ether.type.ipv4.icmp.ident[i], cnode->ether.type.ipv4.icmp.ident[i]);
	}
	printf("\n");

	path = "bless.ether.type.ipv4.icmp.payload";
	node = find_by_path(root, path);
	if (node && node->value && strlen(node->value)) {
		char *payload = strdup(node->value);
		cnode->ether.type.ipv4.icmp.payload = payload;
		cnode->ether.type.ipv4.icmp.payload_len = strlen(payload) + 1;
		printf("%s: %s\n", path, node->value);
	}

	return n;

ERROR:
	printf("yaml config parse error %s: %s\n", path, node->value);
	exit(-1);
}

static int config_parse_bless_ether_type_ip_tcp(Node *root, Cnode *cnode)
{
	int n = 0;
	char *path = NULL;

	path = "bless.ether.type.ipv4.tcp.src";
	Node *node = find_by_path(root, path);
	if (!node) {
		return -1;
	}
	printf("%s: %s\n", path, node->value);

	int32_t range = 0;
	n = config_parse_port_maybe_range_to_array(node, cnode->ether.type.ipv4.tcp.src, &range, PORT_MAX);
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
	printf("  src:\n");
	/* FIXME */
	for (int i = 0; i < n; i++) {
		printf("%u ", cnode->ether.type.ipv4.tcp.src[i]);
	}
	printf("\n");

	path = "bless.ether.type.ipv4.tcp.dst";
	node = find_by_path(root, path);
	if (!node) {
		return -1;
	}
	printf("%s: %s\n", path, node->value);
	range = 0;
	n = config_parse_port_maybe_range_to_array(node, cnode->ether.type.ipv4.tcp.dst, &range, PORT_MAX);
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
	printf("  dst:\n");
	/* FIXME */
	for (int i = 0; i < n; i++) {
		printf("%u ", cnode->ether.type.ipv4.tcp.dst[i]);
	}
	printf("\n");

	path = "bless.ether.type.ipv4.tcp.payload";
	node = find_by_path(root, path);
	if (node && node->value && strlen(node->value)) {
		char *payload = strdup(node->value);
		cnode->ether.type.ipv4.tcp.payload = payload;
		cnode->ether.type.ipv4.tcp.payload_len = strlen(payload) + 1;
		printf("%s: %s\n", path, node->value);
	}

	return n;

ERROR:
	printf("yaml config parse error %s: %s\n", path, node->value);
	exit(-1);
}

static int config_parse_bless_ether_type_ip_udp(Node *root, Cnode *cnode)
{
	int n = 0;
	char *path = NULL;

	path = "bless.ether.type.ipv4.udp.src";
	Node *node = find_by_path(root, path);
	if (!node) {
		return -1;
	}
	printf("%s: %s\n", path, node->value);

	int32_t range = 0;
	n = config_parse_port_maybe_range_to_array(node, cnode->ether.type.ipv4.udp.src, &range, PORT_MAX);
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
	printf("  src:\n");
	for (int i = 0; i < n; i++) {
		printf("%u ", cnode->ether.type.ipv4.udp.src[i]);
	}
	printf("\n");
	path = "bless.ether.type.ipv4.udp.dst";
	node = find_by_path(root, path);
	if (!node) {
		return -1;
	}
	printf("%s: %s\n", path, node->value);
	range = 0;
	n = config_parse_port_maybe_range_to_array(node, cnode->ether.type.ipv4.udp.dst, &range, PORT_MAX);
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
	printf("  dst:\n");
	for (int i = 0; i < n; i++) {
		printf("%u ", cnode->ether.type.ipv4.udp.dst[i]);
	}
	printf("\n");

	path = "bless.ether.type.ipv4.udp.payload";
	node = find_by_path(root, path);
	if (node && node->value && strlen(node->value)) {
		char *payload = strdup(node->value);
		cnode->ether.type.ipv4.udp.payload = payload;
		cnode->ether.type.ipv4.udp.payload_len = strlen(payload) + 1;
		printf("%s: %s\n", path, node->value);
	}

	return n;

ERROR:
	printf("yaml config parse error %s: %s\n", path, node->value);
	exit(-1);
}

static int config_parse_bless_ether_type_ipv4(Node *root, Cnode *cnode)
{
	int n = 0;
	char *path = NULL;

	path = "bless.ether.type.ipv4.src";
	Node *node = find_by_path(root, path);
	if (!node) {
		return -1;
	}
	printf("%s: %s\n", path, node->value);

	int64_t range = 0;
	n = config_parse_ipv4_maybe_range_to_array(node, cnode->ether.type.ipv4.src, &range, IP_ADDR_MAX);
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
	printf("  src:\n");
	for (int i = 1; i <= cnode->ether.type.ipv4.n_src; i++) {
		bless_print_ipv4(cnode->ether.type.ipv4.src[i]);
	}
	printf("\n");
	path = "bless.ether.type.ipv4.dst";
	node = find_by_path(root, path);
	if (!node) {
		return -1;
	}
	printf("%s: %s\n", path, node->value);
	n = config_parse_ipv4_maybe_range_to_array(node, cnode->ether.type.ipv4.dst, &range, IP_ADDR_MAX);
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
	printf("  dst:\n");
	for (int i = 0; i <= cnode->ether.type.ipv4.n_dst; i++) {
		bless_print_ipv4(cnode->ether.type.ipv4.dst[i]);
	}
	printf("\n");

	return n;

ERROR:
	printf("yaml config parse error %s: %s\n", path, node->value);
	exit(-1);
}

static int config_parse_bless_vxlan(Node *root, Cnode *cnode)
{
	int n = 0;
	char *path = NULL;
	Node *node = NULL;

	path = "bless.vxlan.enable";
	node = find_by_path(root, path);
	if (!node || !strlen(node->value) || (
				strcmp("true", node->value) &&
				strcmp("TRUE", node->value)
				)
	   ) {
		printf("vxlan disabled\n");
		// return 1;
	}

	path = "bless.vxlan.ratio";
	node = find_by_path(root, path);
	if (!node || !strlen(node->value)) {
		printf("vxlan disabled\n");
		return 1;
	}

	/* XXX do not return, mutation may use vxlan */
	cnode->vxlan.ratio = atoi(node->value);
	if (cnode->vxlan.ratio < 0 || cnode->vxlan.ratio > 100) {
		printf("vxlan disabled\n");
		cnode->vxlan.enable = 0;
		// return 1;
	}
	cnode->vxlan.enable = 1;
	printf("%s: %s\n", path, node->value);

	n = config_parse_bless_vxlan_ether(root, cnode);
	bless_print_mac((struct rte_ether_addr*)cnode->vxlan.ether.src);
	bless_print_mac((struct rte_ether_addr*)cnode->vxlan.ether.dst);

	path = "bless.vxlan.ether.type.ipv4.src";
	node = find_by_path(root, path);
	if (!node) {
		printf("Invalid vxlan ipv4 src address.\n");
		exit(-1);
	}
	printf("%s: %s\n", path, node->value);

	n = config_parse_sequence_ipv4_vni_to_array(node, cnode->vxlan.ether.type.ipv4.src, cnode->vxlan.ether.type.ipv4.vni, sizeof(uint16_t), PORT_MAX);
	// n = config_parse_sequence_ipv4_to_array(node, cnode->vxlan.ether.type.ipv4.src, sizeof(uint32_t), IP_ADDR_MAX);
	if (n > 0) {
		cnode->vxlan.ether.type.ipv4.n_src = n;
	} else {
		goto ERROR;
	}
	printf("  src:\n");
	for (int i = 0; i < n; i++) {
		bless_print_ipv4(cnode->vxlan.ether.type.ipv4.src[i]);
		printf("  vni: %u\n", cnode->vxlan.ether.type.ipv4.vni[i]);
	}
	printf("\n");

	path = "bless.vxlan.ether.type.ipv4.dst";
	node = find_by_path(root, path);
	if (!node) {
		printf("Invalid vxlan ipv4 dst address.\n");
		exit(-1);
	}
	printf("%s: %s\n", path, node->value);

	n = config_parse_sequence_ipv4_to_array(node, cnode->vxlan.ether.type.ipv4.dst, sizeof(uint32_t), IP_ADDR_MAX);
	if (n > 0) {
		cnode->vxlan.ether.type.ipv4.n_dst = n;
	} else {
		goto ERROR;
	}
	printf("  dst:\n");
	for (int i = 0; i < n; i++) {
		bless_print_ipv4(cnode->vxlan.ether.type.ipv4.dst[i]);
	}
	printf("\n");

	path = "bless.vxlan.ether.type.ipv4.udp.src";
	node = find_by_path(root, path);
	if (!node) {
		return -1;
	}
	printf("%s: %s\n", path, node->value);

	n = config_parse_sequence_to_array(node, cnode->vxlan.ether.type.ipv4.udp.src, sizeof(uint16_t), PORT_MAX);
	if (n > 0) {
		cnode->vxlan.ether.type.ipv4.udp.n_src = n;
	} else {
		goto ERROR;
	}
	printf("  src:\n");
	for (int i = 0; i < n; i++) {
		printf("%u ", cnode->vxlan.ether.type.ipv4.udp.src[i]);
	}
	printf("\n");
	path = "bless.vxlan.ether.type.ipv4.udp.dst";
	node = find_by_path(root, path);
	if (!node) {
		return -1;
	}
	printf("%s: %s\n", path, node->value);
	n = config_parse_sequence_to_array(node, cnode->vxlan.ether.type.ipv4.udp.dst, sizeof(uint16_t), PORT_MAX);
	if (n > 0) {
		cnode->vxlan.ether.type.ipv4.udp.n_dst = n;
	} else {
		goto ERROR;
	}
	printf("  dst:\n");
	for (int i = 0; i < n; i++) {
		printf("%u ", cnode->vxlan.ether.type.ipv4.udp.dst[i]);
	}
	printf("\n");

	return n;

ERROR:
	printf("yaml config parse error %s: %s\n", path, node->value);
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
		printf("erroneous disabled\n");
		return 1;
	}
	cnode->erroneous.ratio = atoi(node->value);
	if (!cnode->erroneous.ratio) {
		printf("erroneous disabled due to no ratio\n");
		return 1;
	}
	path = "bless.erroneous.class";
	node = find_by_path(root, path);
	if (!node || !node->child ) {
		cnode->erroneous.ratio = 0;
		printf("erroneous disabled due to no class\n");
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
	printf("> parsed %d erroneous mutations:\n", res);
	int pos = 0;
	mutation_func *func = malloc(sizeof(mutation_func) * res);
	for (int i = 0; i < cnode->erroneous.n_clas; i++) {
		struct ec_clas *clas = &cnode->erroneous.clas[i];
		printf("  %d_%s: ", i, clas->name);
		for (int j = 0; j < clas->n_type; j++) {
			printf(" %s ", clas->type[j]);
			mutation_func f = find_mutation_func(clas->name, clas->type[j]);
			if (!f) {
				printf(" no mutation `%s:%s'\n", clas->name, clas->type[j]);
				exit(-1);
			}
			func[pos++] = f;
		}
		printf("\n");
	}
	cnode->erroneous.func = func;
	printf("\n");

	return res;
}

Cnode *config_parse_bless(Node *root)
{
	struct Cnode *cnode = malloc(sizeof(struct Cnode));
	memset(cnode, 0, sizeof(struct Cnode));
	Node *node = find_by_path(root, "bless");
	if (!node) {
		return NULL;
	}

	char *path = NULL;
	node = NULL;

	/* hw offload */
	path = "bless.hw-offload";
	node = find_by_path(root, path);
	printf("hw offload: ");
	if (node) {
		int n_item = sizeof(offload_table) / sizeof(offload_table[0]);
		for (Node *t = node->child; t; t = t->next) {
			for (int i = 0; i < n_item; i++) {
				if (strcmp(t->value, offload_table[i].name)) {
					// printf("unknown offload type %s\n", t->value);
					continue;
				}
				cnode->offload |= 1 << offload_table[i].type;
				printf("found offload type %s ", t->value);
			}
		}
	}
	printf("\n%lu\n", cnode->offload);

	/* ether */
	config_parse_bless_ether(root, cnode);
	path = "bless.ether.dst";
	node = find_by_path(root, path);
	if (!node) {
		return NULL;
	}
	printf("%s: %s\n", path, node->value);

	config_parse_bless_ether_type_arp(root, cnode);
	config_parse_bless_ether_type_ipv4(root, cnode);
	config_parse_bless_ether_type_ipv4_icmp(root, cnode);
	config_parse_bless_ether_type_ip_tcp(root, cnode);
	config_parse_bless_ether_type_ip_udp(root, cnode);
	config_parse_bless_vxlan(root, cnode);
	config_parse_bless_erroneous(root, cnode);

	return cnode;
}

uint16_t random_array_elem_uint16_t(uint16_t *array, uint16_t num, int32_t range)
{
	uint64_t ra = rte_rdtsc();
	uint16_t tsc = 0;

	if (num) {
		tsc =  (uint16_t)(ra ^ (ra >> 8)) & (BLESS_CONFIG_MAX - 1);
		return rte_cpu_to_be_16(array[tsc % num]);
	}

	if (!range) {
		return rte_cpu_to_be_16(*array);
	}

	tsc = (ra ^ (ra >> 8)) & (uint16_t)(-1);
	int offset = tsc % labs(range);
	return rte_cpu_to_be_16(*array + (range > 0 ? offset : -offset));
}

uint32_t random_array_elem_uint32_t(uint32_t *array, uint16_t num, int64_t range)
{
	uint64_t ra = rte_rdtsc();
	uint32_t tsc = 0;

	if (num) {
		tsc = (uint32_t)(ra ^ (ra >> 8)) & (BLESS_CONFIG_MAX - 1);
		return array[tsc % num];
	}

	if (!range) {
		return *array;
	}

	tsc = (uint32_t)(ra ^ (ra >> 8));
	int offset = tsc % labs(range);
	return rte_cpu_to_be_32(rte_cpu_to_be_32(*array) + (range > 0 ? offset : -offset));
}

/** random_array_elem_uint32_t_with_peer - special case for ipv4:vni
 * @return: the lower 32 bits is ipv4 address with net order, the upper 32 bits is vni
 * with host order.
 */
uint64_t random_array_elem_uint32_t_with_peer(uint32_t *array, uint32_t *peer, uint16_t num, int64_t range)
{
	uint32_t index = 0;
	uint64_t tsc = rdtsc64();
	tsc = tsc ^ (tsc >> 8);

	/* pure array: [ 0, 1, ..., n ] */
	if (num) {
		index = tsc % num;
		uint64_t val = (uint64_t)array[index];
		val |= ((uint64_t)peer[index] << 32);
		return val;
	}

	/* range [ val, val + n ] */
	if (!range) {
		index = 0;
		return (uint64_t)*array | ((uint64_t)*peer << 32);
	}

	index = tsc % labs(range);
	return rte_cpu_to_be_32(rte_cpu_to_be_32(*array) + (range > 0 ? index : -index))
		| ((uint64_t)(*peer + index) << 32);
}
