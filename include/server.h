#ifndef __WS_SERVER_H__
#define __WS_SERVER_H__

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <unistd.h> /* for sleep() */
#include "civetweb.h"

#define STATS_JSON_MAX     8192
#define STATS_METRIC_MAX   8192

struct ws_user_data {
	void *conf;
	void *data;
	void (*func)(void *user, void *data, size_t size);
};

struct stats_snapshot {
    uint64_t ts_ns;

    size_t json_len;
    char   json[STATS_JSON_MAX << 1];

    size_t metric_len;
    char   metric[STATS_METRIC_MAX << 1];
};

/* ====== option field types (MUST be visible to X-macro) ====== */
typedef const char * STR;
typedef int          INT;
typedef int          BOOL;

#define SERVER_OPTS_MAX 16
#define SERVER_KV_MAX 128

/* civetweb options kv */
struct civet_kv {
	const char *key;
	char        val[SERVER_KV_MAX + 1];
};

/* ====== YAML → struct ====== */
struct server_options_cfg {
#define X(name, type, key, def) \
	type name;
#include "server_options.def"
#undef X
	/* 每个 option 一项 */
	struct civet_kv kv[SERVER_OPTS_MAX];
	/* key,value,...,NULL */
	const char *civet_opts[(SERVER_OPTS_MAX << 1) + 1];
};

#define SERVER_SERVICE_HTTP_MAX 16
#define SERVER_SERVICE_HTTP_LEN_MAX 128
struct server_service {
	char *websocket_uri;
	/* TODO */
	uint16_t n_http;
	char http[SERVER_SERVICE_HTTP_MAX][SERVER_SERVICE_HTTP_LEN_MAX];
};

struct server {
	struct mg_context *ctx;
	struct ws_user_data wsud;
	struct server_service svc;
	struct server_options_cfg cfg;
};

/* YAML value -> cfg */
#define SERVER_PARSE_STR(dst, v)   ((dst) = (v))
#define SERVER_PARSE_INT(dst, v)   ((dst) = atoi(v))
#define SERVER_PARSE_BOOL(dst, v)  \
	((dst) = (!strcmp(v,"yes") || !strcmp(v,"true") || !strcmp(v,"1")))

/* 值 → 字符串 */
#define SERVER_OPT_TO_STR_STR(dst, sz, val) \
	snprintf(dst, sz, "%s", val)
#define SERVER_OPT_TO_STR_INT(dst, sz, val) \
	snprintf(dst, sz, "%d", val)
#define SERVER_OPT_TO_STR_BOOL(dst, sz, val) \
	snprintf(dst, sz, "%s", (val) ? "yes" : "no")

/* API */
void server_options_set_defaults(struct server_options_cfg *cfg);
int server_options_from_yaml(struct server_options_cfg *cfg, void *yaml_node);
size_t build_civet_options(const struct server_options_cfg *cfg, struct civet_kv *out, size_t max);
void server_show(struct server* srv);
void server_show_format(struct server* srv, char *pref);

struct mg_context *ws_server_start(void*);
int ws_server_stop(struct mg_context *ctx);
void ws_broadcast_stats();
void ws_broadcast_log(char *log, size_t len);
const struct stats_snapshot * stats_get_active(void);
int stats_get_active_index(void);
struct stats_snapshot * stats_get(int idx);
void stats_set(int idx);

#endif
