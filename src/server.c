#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>

#include "civetweb.h"
#include "server.h"
#include "server_const.h"
#include "log.h"

/* ================================================================ */
/* Server options                                                    */
/* ================================================================ */
static const char *SERVER_OPTIONS[] = {
	"listening_ports", "8000",
	"num_threads", "4",
	"enable_keep_alive", "yes",
	"request_timeout_ms", "2000",
	NULL, NULL
};

static const char WS_URL[] = "/wsURL";

/* Define websocket sub-protocols. */
/* This must be static data, available between mg_start and mg_stop. */
static const char subprotocol_bin[] = "Company.ProtoName.bin";
static const char subprotocol_json[] = "Company.ProtoName.json";
static const char *subprotocols[] = {subprotocol_bin, subprotocol_json, NULL};
static struct mg_websocket_subprotocols wsprot = {2, subprotocols};

/* ================================================================ */
/* WebSocket clients                                                 */
/* ================================================================ */
#define MGC_SIZE 16
static const struct mg_connection *mgc[MGC_SIZE];
static int n_mgc = 0;
static pthread_mutex_t mgc_lock = PTHREAD_MUTEX_INITIALIZER;

struct tClientContext {
	uint32_t conn_id;
};

/* ================================================================ */
/* Stats snapshot (double buffer)                                   */
/* ================================================================ */
/* 全局双缓冲 */
static struct stats_snapshot g_stats_buf[2];
static _Atomic int g_stats_active_idx = 0;

/* ================================================================ */
/* Helper                                                           */
/* ================================================================ */
int stats_get_active_index(void)
{
	return atomic_load_explicit(&g_stats_active_idx, memory_order_acquire);
}

struct stats_snapshot * stats_get(int idx)
{
	return &g_stats_buf[idx];
}

void stats_set(int idx)
{
	atomic_store_explicit(&g_stats_active_idx,
			idx, memory_order_release);
}

const struct stats_snapshot * stats_get_active(void)
{
	int idx = atomic_load_explicit(&g_stats_active_idx, memory_order_acquire);
	return &g_stats_buf[idx];
}

static void server_show_request_headers(const struct mg_request_info *ri)
{
	if (!ri) {
		return;
	}

	LOG_SHOW("=== mg_request_headers ===");
	for (int i = 0; i < ri->num_headers; i++) {
		LOG_SHOW("hdr[%d] %s: %s", i,
				ri->http_headers[i].name,
				ri->http_headers[i].value);
	}
}

static void server_show_request_info(const struct mg_request_info *ri)
{
	if (!ri) {
		return;
	}

	LOG_SHOW("=== mg_request_info ===");
	LOG_SHOW("method       : %s", ri->request_method ?: "(null)");
	LOG_SHOW("request_uri  : %s", ri->request_uri ?: "(null)");
	LOG_SHOW("local_uri    : %s", ri->local_uri ?: "(null)");
	LOG_SHOW("query_string : %s", ri->query_string ?: "(null)");
	LOG_SHOW("http_version : %s", ri->http_version ?: "(null)");
	LOG_SHOW("remote_addr  : %s:%d", ri->remote_addr, ri->remote_port);
	LOG_SHOW("server_addr  : %s:%d", ri->server_addr, ri->server_port);
	LOG_SHOW("is_ssl       : %d", ri->is_ssl);
	LOG_SHOW("content_len  : %lld", ri->content_length);
	LOG_SHOW("num_headers  : %d", ri->num_headers);
}

/* ================================================================ */
/* WebSocket handlers                                                */
/* ================================================================ */
static int ws_connect_handler(const struct mg_connection *conn, void *ud)
{
	(void)ud;

	pthread_mutex_lock(&mgc_lock);
	if (n_mgc >= MGC_SIZE) {
		pthread_mutex_unlock(&mgc_lock);
		return 1;
	}

	static uint32_t id = 0;
	struct tClientContext *ctx = calloc(1, sizeof(*ctx));
	ctx->conn_id = ++id;
	mg_set_user_connection_data(conn, ctx);

	mgc[n_mgc++] = conn;
	pthread_mutex_unlock(&mgc_lock);

	const struct mg_request_info *ri = mg_get_request_info(conn);
	LOG_HINT("WS client %u connected with subprotocol %s", ctx->conn_id,
			ri->acceptedWebSocketSubprotocol);

	return 0;
}

static void ws_ready_handler(struct mg_connection *conn, void *ud)
{
	(void)ud;
	const char *hello = "{\"hello\":\"world\"}";
	mg_websocket_write(conn,
			MG_WEBSOCKET_OPCODE_TEXT,
			hello,
			strlen(hello));
}

static int ws_data_handler(struct mg_connection *conn, int opcode,
		char *data, size_t datasize, void *ud)
{
	(void)conn;

	if ((opcode & 0xf) == MG_WEBSOCKET_OPCODE_TEXT && datasize) {
		LOG_TRACE("data %lu size %s", datasize, data);
		struct ws_user_data *wsud = (struct ws_user_data*)ud;
		wsud->func(ud, data, datasize);
	}

	return 1;
}

static void ws_close_handler(const struct mg_connection *conn, void *ud)
{
	(void)ud;
	pthread_mutex_lock(&mgc_lock);
	for (int i = 0; i < n_mgc; i++) {
		if (mgc[i] == conn) {
			for (int j = i; j < n_mgc - 1; j++) {
				mgc[j] = mgc[j + 1];
			}
			n_mgc--;
			break;
		}
	}
	pthread_mutex_unlock(&mgc_lock);

	struct tClientContext *ctx = mg_get_user_connection_data(conn);
	LOG_HINT("conn %d closed", ctx->conn_id);
	free(ctx);
}

/* ================================================================ */
/* HTTP handlers                                                     */
/* ================================================================ */
#if 0
static int http_control_handler(struct mg_connection *conn, void *ud)
{
	printf("[%s %d\n", __func__, __LINE__);
	(void)ud;
	const struct stats_snapshot *s = stats_get_active();

	/*
	   mg_printf(conn,
	   "HTTP/1.1 200 OK\r\n"
	   "Content-Type: application/json\r\n"
	   "Cache-Control: no-cache\r\n\r\n"
	   "Content-Length: %zu\r\n\r\n%lu",
	   sizeof(uint64_t), s->ts_ns);
	   */
	char buf[32];
	int len = snprintf(buf, sizeof(buf), "%lu", s->ts_ns);

	mg_printf(conn,
			"HTTP/1.1 200 OK\r\n"
			"Content-Type: application/json\r\n"
			"Cache-Control: no-cache\r\n"
			"Content-Length: %d\r\n"
			"Connection: close\r\n\r\n"
			"%s",
			len, buf);

	return 1;
}
#endif
static int http_control_handler(struct mg_connection *conn, void *ud)
{
	const struct stats_snapshot *s = stats_get_active();
	char buf[32];
	int len = snprintf(buf, sizeof(buf), "%lu", s->ts_ns);

	mg_printf(conn,
			"HTTP/1.1 200 OK\r\n"
			"Content-Type: application/json\r\n"
			"Cache-Control: no-cache\r\n"
			"Content-Length: %d\r\n"
			"Connection: close\r\n\r\n"
			"%s",
			len, buf);

	return 1;
}

#if 0
static int http_stats_handler(struct mg_connection *conn, void *ud)
{
	printf("[%s %d\n", __func__, __LINE__);
	(void)ud;
	const struct stats_snapshot *s = stats_get_active();

	mg_printf(conn,
			"HTTP/1.1 200 OK\r\n"
			"Content-Type: application/json\r\n"
			"Cache-Control: no-cache\r\n\r\n"
			"Content-Length: %zu\r\n\r\n%s",
			s->json_len, s->json);

	return 200;
}
#endif

static int http_stats_handler(struct mg_connection *conn, void *ud)
{
	(void)ud;
	const struct stats_snapshot *s = stats_get_active();

	server_show_request_headers(mg_get_request_info(conn));
	server_show_request_info(mg_get_request_info(conn));
	mg_printf(conn,
			"HTTP/1.1 200 OK\r\n"
			"Content-Type: application/json\r\n"
			"Cache-Control: no-cache\r\n"
			"Content-Length: %zu\r\n"
			"Connection: close\r\n\r\n"
			"%s",
			s->json_len, s->json);

	return 1;
}

#if 0
static int http_metrics_handler(struct mg_connection *conn, void *ud)
{
	printf("[%s %d\n", __func__, __LINE__);
	(void)ud;
	const struct stats_snapshot *s = stats_get_active();

	/*
	   mg_printf(conn,
	   "HTTP/1.1 200 OK\r\n"
	   "Content-Type: text/plain; version=0.0.4\r\n"
	   "Cache-Control: no-cache\r\n\r\n"
	   "\r\n%s", s->metric);
	   */
	mg_printf(conn,
			"HTTP/1.1 200 OK\r\n"
			"Content-Type: text/plain\r\n"
			"Cache-Control: no-cache\r\n"
			"Content-Length: %zu\r\n"
			"Connection: close\r\n\r\n"
			"%s",
			strlen(s->metric), s->metric);

	return 1;


	return 200;
}
#endif

static int http_metrics_handler(struct mg_connection *conn, void *ud)
{
	(void)ud;
	const struct stats_snapshot *s = stats_get_active();
	size_t len = strlen(s->metric);

	mg_printf(conn,
			"HTTP/1.1 200 OK\r\n"
			"Content-Type: text/plain; version=0.0.4\r\n"
			"Cache-Control: no-cache\r\n"
			"Content-Length: %zu\r\n"
			"Connection: close\r\n\r\n"
			"%s",
			len, s->metric);

	return 1;   // ← 关键
}

/* ================================================================ */
/* Broadcast                                                         */
/* ================================================================ */
void ws_broadcast_stats(void)
{
	const struct stats_snapshot *s = stats_get_active();

	pthread_mutex_lock(&mgc_lock);
	for (int i = 0; i < n_mgc; i++) {
		mg_websocket_write(
				(struct mg_connection *)mgc[i],
				MG_WEBSOCKET_OPCODE_TEXT,
				s->json,
				s->json_len);
	}
	pthread_mutex_unlock(&mgc_lock);
}

void ws_broadcast_log(char *log, size_t len)
{
	pthread_mutex_lock(&mgc_lock);
	for (int i = 0; i < n_mgc; i++) {
		mg_websocket_write(
				(struct mg_connection *)mgc[i],
				MG_WEBSOCKET_OPCODE_TEXT,
				log, len);
	}
	pthread_mutex_unlock(&mgc_lock);
}

static int root_handler(struct mg_connection *conn, void *cbdata)
{
	server_show_request_headers(mg_get_request_info(conn));
	mg_printf(conn,
			"HTTP/1.1 200 OK\r\n"
			"Content-Type: text/html\r\n"
			"Cache-Control: no-cache\r\n"
			"Content-Length: %u\r\n"
			"Connection: close\r\n\r\n"
			"%s", index_html_len, index_html);

	return 1;
}

int ws_universal_handler(struct mg_connection *conn, void *cbdata)
{
	LOG_HINT("=== Universal handler called! ===");

	return 0;  // 返回0继续处理，返回非0表示已处理
}

/* ================================================================ */
/* Server lifecycle                                                 */
/* ================================================================ */
struct mg_context * ws_server_start(void *data)
{
	mg_init_library(0);

	struct ws_user_data *wsud = NULL;
	struct server *srv = NULL;

	if (data) {
		wsud = (struct ws_user_data*)data;
		srv = wsud->conf;
	}

	struct mg_callbacks cb = {0};
	struct mg_init_data init = {
		.callbacks = &cb,
		.user_data = data,
		// .configuration_options = srv->cfg.civet_opts[0] ? srv->cfg.civet_opts : SERVER_OPTIONS,
		.configuration_options = SERVER_OPTIONS,
	};

	struct mg_error_data mg_start_error_data = {0};
	char errtxtbuf[256] = {0};
	mg_start_error_data.text = errtxtbuf;
	mg_start_error_data.text_buffer_size = sizeof(errtxtbuf);

	struct mg_context *ctx = mg_start2(&init, &mg_start_error_data);
	if (!ctx) {
		LOG_ERR("mg_start2(%p)\n", &init);
		server_show(srv);
		fprintf(stderr, "Cannot start server: %s\n", errtxtbuf);
		mg_exit_library();
		return NULL;
	}

	mg_set_websocket_handler_with_subprotocols(ctx,
			// srv->svc.websocket_uri ? srv->svc.websocket_uri : WS_URL,
			WS_URL,
			&wsprot,
			ws_connect_handler,
			ws_ready_handler,
			ws_data_handler,
			ws_close_handler,
			data);

	mg_set_request_handler(ctx, "/", root_handler, NULL);
	mg_set_request_handler(ctx, "/api/control", http_control_handler, NULL);
	mg_set_request_handler(ctx, "/api/stats", http_stats_handler, NULL);
	mg_set_request_handler(ctx, "/metrics", http_metrics_handler, NULL);
	// 注册为捕获所有请求
	mg_set_request_handler(ctx, "**", ws_universal_handler, NULL);

	LOG_INFO("Websocket Server Started");

	return ctx;
}

int ws_server_stop(struct mg_context *ctx)
{
	/* Stop server, disconnect all clients. Then deinitialize CivetWeb library.
	*/
	mg_stop(ctx);
	mg_exit_library();

	return 0;
}

void server_options_set_defaults(struct server_options_cfg *cfg)
{
#define X(name, type, key, def) \
	cfg->name = def;
#include "server_options.def"
#undef X
}

size_t build_civet_options(const struct server_options_cfg *cfg, struct civet_kv *out, size_t max)
{
	size_t i = 0;

#define X(name, type, civet_key, def)                        \
	if (i < max) {                                      \
		out[i].key = civet_key;                     \
		SERVER_OPT_TO_STR_##type(                   \
				out[i].val, sizeof(out[i].val),    \
				cfg->name);                         \
		i++;                                        \
	}

#include "server_options.def"
#undef X

	return i;
}

void server_show_options_cfg_format(struct server_options_cfg *cfg, char *pref)
{
	LOG_HINT("%soptions %p", pref, cfg);
	for (int i = 0; i < (SERVER_OPTS_MAX << 1) && cfg->civet_opts[i * 2]; i++) {
		LOG_SHOW("%s  %s: %s", pref, cfg->civet_opts[i * 2], cfg->civet_opts[i * 2 + 1]);
	}
}

void server_show_options_cfg(struct server_options_cfg *cfg)
{
	server_show_options_cfg_format(cfg, "");
}

void server_show_service_format(struct server_service *svc, char *pref)
{
	LOG_HINT("%sservice %p", pref, svc);
	LOG_SHOW("%s  websocket url    %s", pref, svc->websocket_uri);
	LOG_HINT("%s  http", pref);
	for (int i = 0; i < svc->n_http; i++) {
		LOG_SHOW("%s    %s", pref, svc->http[i]);
	}
}

void server_show_service(struct server_service *svc)
{
	server_show_service_format(svc, "");
}

void server_show_format(struct server* srv, char *pref)
{
	LOG_INFO("%sserver   %p", pref, srv);
	LOG_SHOW("%sctx      %p", pref, srv->ctx);
	LOG_HINT("%swsud     %p", pref, &srv->wsud);
	LOG_SHOW("%s  conf   %p", pref, &srv->wsud.conf);
	LOG_SHOW("%s  data   %p", pref, &srv->wsud.data);
	LOG_SHOW("%s  func   %p", pref, &srv->wsud.func);
	server_show_service_format(&srv->svc, pref);
	server_show_options_cfg_format(&srv->cfg, pref);
}

void server_show(struct server* srv)
{
	server_show_format(srv, "");
}

void server_config_template()
{
	printf("%s\n", config_yaml);
}
