#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>

#include "civetweb.h"
#include "server.h"

/* ================================================================ */
/* Server options                                                    */
/* ================================================================ */
static const char *SERVER_OPTIONS[] = {
	"listening_ports", "8000",
	"num_threads", "4",
	"enable_keep_alive", "yes",
	"request_timeout_ms", "2000",
	NULL
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
	return atomic_load_explicit(&g_stats_active_idx,
			memory_order_acquire);
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
	int idx = atomic_load_explicit(&g_stats_active_idx,
			memory_order_acquire);
	return &g_stats_buf[idx];
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
	printf("WS client %u connected with subprotocol %s\n", ctx->conn_id,
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
	(void)datasize;

	if ((opcode & 0xf) == MG_WEBSOCKET_OPCODE_TEXT) {
		printf("WS recv: %.*s\n", (int)datasize, data);
	}

	if (datasize) {
		struct ws_user_data *wsud = (struct ws_user_data*)ud;
		wsud->func(data, datasize);
	}

	return 1;
}

static void ws_close_handler(const struct mg_connection *conn, void *ud)
{
	(void)ud;
	pthread_mutex_lock(&mgc_lock);
	for (int i = 0; i < n_mgc; i++) {
		if (mgc[i] == conn) {
			for (int j = i; j < n_mgc - 1; j++)
				mgc[j] = mgc[j + 1];
			n_mgc--;
			break;
		}
	}
	pthread_mutex_unlock(&mgc_lock);
	struct tClientContext *ctx = mg_get_user_connection_data(conn);
	printf("conn %d closed", ctx->conn_id);
	free(ctx);
}

/* ================================================================ */
/* HTTP handlers                                                     */
/* ================================================================ */
static int http_control_handler(struct mg_connection *conn, void *ud)
{
	(void)ud;
	const struct stats_snapshot *s = stats_get_active();

	mg_printf(conn,
			"HTTP/1.1 200 OK\r\n"
			"Content-Type: application/json\r\n"
			"Cache-Control: no-cache\r\n\r\n"
			"Content-Length: %zu\r\n\r\n%lu",
			sizeof(uint64_t), s->ts_ns);

	return 200;
}

static int http_stats_handler(struct mg_connection *conn, void *ud)
{
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

static int http_metrics_handler(struct mg_connection *conn, void *ud)
{
	(void)ud;
	const struct stats_snapshot *s = stats_get_active();

	mg_printf(conn,
			"HTTP/1.1 200 OK\r\n"
			"Content-Type: text/plain; version=0.0.4\r\n"
			"Cache-Control: no-cache\r\n\r\n"
			"\r\n%s", s->metric);

	return 200;
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

/* ================================================================ */
/* Server lifecycle                                                 */
/* ================================================================ */
struct mg_context * ws_server_start(void *data)
{
	mg_init_library(0);

	struct ws_user_data *wsud = (struct ws_user_data*)data;
	struct server_options_cfg *cfg = wsud->data;

	struct mg_callbacks cb = {0};
	struct mg_init_data init = {
		.callbacks = &cb,
		.user_data = data,
		.configuration_options = cfg->civet_opts[0] ? cfg->civet_opts : SERVER_OPTIONS,
	};

	struct mg_context *ctx = mg_start2(&init, NULL);
	if (!ctx) {
		return NULL;
	}

	mg_set_websocket_handler_with_subprotocols(ctx,
			cfg->uri ? cfg->uri : WS_URL,
			&wsprot,
			ws_connect_handler,
			ws_ready_handler,
			ws_data_handler,
			ws_close_handler,
			data);

	mg_set_request_handler(ctx, "/api/control",
			http_control_handler, NULL);
	mg_set_request_handler(ctx, "/api/stats",
			http_stats_handler, NULL);
	mg_set_request_handler(ctx, "/metrics",
			http_metrics_handler, NULL);

	printf("WS/HTTP server started\n");

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
