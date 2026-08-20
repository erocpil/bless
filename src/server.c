#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include "civetweb.h"
#include "server.h"
#include "server_const.h"
#include "control_policy.h"
#include "log.h"
#include "ws_frame.h"
#include "stats_guard.h"
#include "stats_payload.h"

/* Entropy Dashboard HTML (served at /entropy) */#include "entropy_html.inc"
/* Observe Dashboard HTML (served at /observe) */#include "observe_html.inc"
/* Chart.js bundle (embedded at build time, served at /assets/chart.min.js) */#include "chartjs.inc"

/* ================================================================ */
/* Server options                                                    */
/* ================================================================ */
static const char *SERVER_OPTIONS[] = {
	"listening_ports", "127.0.0.1:8000",
	"num_threads", "4",
	"enable_keep_alive", "yes",
	"request_timeout_ms", "2000",
	NULL, NULL
};

static const char WS_URL[] = "/wsURL";

/* ================================================================ */
/* WebSocket clients                                                 */
/* ================================================================ */
#define MGC_SIZE 16
static const struct mg_connection *mgc[MGC_SIZE];
static int n_mgc = 0;
static pthread_mutex_t mgc_lock = PTHREAD_MUTEX_INITIALIZER;

/* Dedicated broadcast thread -- isolates blocking WS writes from the DPDK
 * main loop so a slow client (full TCP window) can't stall stats generation
 * or prevent new WS connections. */
static pthread_t        broadcast_thread;
static atomic_int       broadcast_stop;

struct tClientContext {
	uint32_t conn_id;
};

/* ================================================================ */
/* Stats snapshot (double buffer)                                   */
/* ================================================================ */
/* Global double buffer */
static struct stats_snapshot g_stats_buf[2];
static struct stats_guard g_stats_guard = {
	.active = ATOMIC_VAR_INIT(0),
	.readers = {ATOMIC_VAR_INIT(0), ATOMIC_VAR_INIT(0)},
};

/* ================================================================ */
/* Helper                                                           */
/* ================================================================ */
int stats_get_active_index(void)
{
	return stats_guard_active(&g_stats_guard);
}

struct stats_snapshot * stats_get(int idx)
{
	return &g_stats_buf[idx];
}

void stats_set(int idx)
{
	stats_guard_publish(&g_stats_guard, idx);
}

/*
 * Pin the currently active slot before accessing any of its non-atomic data.
 * The active index is validated after incrementing the slot's reader count
 * to close the stale-pointer race with a producer that already flipped
 * buffers.
 */
const struct stats_snapshot *stats_snapshot_acquire(void)
{
	int idx = stats_guard_acquire(&g_stats_guard);
	return &g_stats_buf[idx];
}

void stats_snapshot_release(const struct stats_snapshot *snapshot)
{
	if (!snapshot) {
		return;
	}
	ptrdiff_t idx = snapshot - g_stats_buf;
	if (idx >= 0 && idx < 2) {
		stats_guard_release(&g_stats_guard, (int)idx);
	}
}

int stats_snapshot_writable(int idx)
{
	return idx >= 0 && idx < 2
		&& stats_guard_writable(&g_stats_guard, idx);
}

/* ================================================================ */
/* API key authentication                                           */
/* ================================================================ */

/* encodeURIComponent() may encode every input byte as "%XX". */
#define API_KEY_URL_MAX_LEN  (CONTROL_API_KEY_MAX_LEN * 3)

static const char *g_api_key     = NULL;
static size_t      g_api_key_len = 0;
static int         g_api_key_enabled = 0;  /* 0=no, 1=yes */

/* Called once during ws_server_start(), before mg_start2() spawns threads.
 * Returns 0 on success, -1 if BLESS_API_KEY is explicitly set but too short
 * (startup is aborted — no silent fallback to unauthenticated mode). */
static int api_key_init(void)
{
	const char *key = getenv("BLESS_API_KEY");
	if (!key) {
		LOG_INFO("BLESS_API_KEY not set — WS auth disabled");
		return 0;
	}

	/* Explicitly set but empty — deployment misconfiguration.
	 * Reject startup; never silently disable auth. */
	if (!key[0]) {
		LOG_ERR("BLESS_API_KEY is set but empty — refusing to start");
		return -1;
	}

	size_t len = strlen(key);
	if (!control_policy_key_length_valid(len)) {
		LOG_ERR("BLESS_API_KEY length invalid (%zu; expected %d..%d chars) — refusing to start",
			len, CONTROL_API_KEY_MIN_LEN, CONTROL_API_KEY_MAX_LEN);
		return -1;
	}

	g_api_key     = key;
	g_api_key_len = len;
	g_api_key_enabled = 1;
	LOG_INFO("WS API key authentication enabled (%zu bytes)", len);
	return 0;
}

static int api_key_not_required(void)
{
	return !g_api_key_enabled;
}

/* Returns 1 if `candidate` matches g_api_key (constant-time). */
static int api_key_verify(const char *candidate, size_t candidate_len)
{
	if (candidate_len != g_api_key_len) {
		return 0;
	}
	return control_policy_key_equal(g_api_key, g_api_key_len,
		candidate, candidate_len);
}

/* ================================================================ */
/* WebSocket callbacks                                              */
/* ================================================================ */
static int ws_connect_handler(const struct mg_connection *conn, void *ud)
{
	(void)ud;

	/* ── API key authentication ── */
	if (!api_key_not_required()) {
		const struct mg_request_info *ri = mg_get_request_info(conn);
		char key_buf[API_KEY_URL_MAX_LEN + 1];
		size_t raw_len = control_policy_query_key(
			ri ? ri->query_string : NULL, key_buf, sizeof(key_buf));

		if (raw_len == 0) {
			LOG_WARN("WS auth rejected (no api_key)");
			return 1;
		}

		/* URL-decode: the dashboard sends encodeURIComponent(key).
		 * mg_url_decode handles %XX, +, and form-encoding. */
		char decoded[256];
		int dlen = mg_url_decode(key_buf, (int)raw_len,
					 decoded, (int)sizeof(decoded), 0);
		if (dlen < 0) {
			LOG_WARN("WS auth rejected (invalid url-encoded key)");
			return 1;
		}

		if (!api_key_verify(decoded, (size_t)dlen)) {
			LOG_WARN("WS auth rejected (api_key mismatch)");
			return 1;  /* reject connection */
		}
	}

	pthread_mutex_lock(&mgc_lock);
	if (n_mgc >= MGC_SIZE) {
		pthread_mutex_unlock(&mgc_lock);
		LOG_WARN("WS client rejected: max connections (%d)", MGC_SIZE);
		return 1;
	}

	static uint32_t id = 0;
	struct tClientContext *ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		pthread_mutex_unlock(&mgc_lock);
		LOG_ERR("WS connect: calloc failed");
		return 1;
	}
	ctx->conn_id = ++id;
	mg_set_user_connection_data(conn, ctx);

	mgc[n_mgc++] = conn;
	pthread_mutex_unlock(&mgc_lock);

	LOG_HINT("WS client %u connected", ctx->conn_id);

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
	enum bless_ws_frame_result result = bless_ws_frame_validate(
		opcode, data, datasize, BLESS_WS_COMMAND_MAX);
	if (result != BLESS_WS_FRAME_OK) {
		const char *message = bless_ws_frame_error(result);
		char reply[192];
		int len = snprintf(reply, sizeof(reply),
			"{\"error\":\"%s\",\"code\":400}", message);
		LOG_WARN("WS frame rejected: %s", message);
		if (len > 0 && (size_t)len < sizeof(reply)) {
			mg_websocket_write(conn, MG_WEBSOCKET_OPCODE_TEXT,
					   reply, (size_t)len);
		}
		return 1;
	}

	LOG_TRACE("data %zu size %.*s", datasize, (int)datasize, data);
	struct ws_user_data *wsud = (struct ws_user_data*)ud;
	wsud->func(conn, ud, data, datasize);

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
	if (ctx) {
		LOG_HINT("conn %u closed", ctx->conn_id);
	}
	free(ctx);
}

/* ================================================================ */
/* HTTP handlers                                                     */
/* ================================================================ */
/* HTTP handler: ``/api/control`` -- runtime control (start/stop/config). */
static int http_control_handler(struct mg_connection *conn, void *ud)
{
	(void)ud;
	const struct stats_snapshot *s = stats_snapshot_acquire();
	char buf[32];
	int len = snprintf(buf, sizeof(buf), "%lu", s->tsc_cycles);
	stats_snapshot_release(s);

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

/* HTTP handler: ``/api/stats`` -- JSON stats endpoint. */
static int http_stats_handler(struct mg_connection *conn, void *ud)
{
	(void)ud;
	const struct stats_snapshot *s = stats_snapshot_acquire();

	/* Copy json to a local buffer before any blocking IO. */
	char json_copy[sizeof(s->json)];
	size_t len = stats_payload_copy(json_copy, sizeof(json_copy),
		s->json, s->json_len);

	stats_snapshot_release(s);

	mg_printf(conn,
			"HTTP/1.1 200 OK\r\n"
			"Content-Type: application/json\r\n"
			"Cache-Control: no-cache\r\n"
			"Content-Length: %zu\r\n"
			"Connection: close\r\n\r\n"
			"%s",
			len, json_copy);

	return 1;
}

static int http_config_handler(struct mg_connection *conn, void *ud)
{
	(void)ud;
	const struct stats_snapshot *s = stats_snapshot_acquire();
	char body[512];
	int len = snprintf(body, sizeof(body),
		"{\"effective_config\":{\"batch\":%u,\"traffic_model\":%u,"
		"\"batch_delay_us\":%llu,\"batch_jitter_us\":%llu,"
		"\"sample_interval\":%u}}",
		s->effective_batch, s->effective_traffic_model,
		(unsigned long long)s->effective_batch_delay_us,
		(unsigned long long)s->effective_batch_jitter_us,
		s->effective_sample_interval);
	stats_snapshot_release(s);
	if (len < 0 || (size_t)len >= sizeof(body)) {
		return 0;
	}
	mg_printf(conn, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
		"Cache-Control: no-cache\r\nContent-Length: %d\r\n"
		"Connection: close\r\n\r\n%s", len, body);
	return 1;
}

/* HTTP handler: ``/metrics`` -- Prometheus / OpenMetrics export. */
static int http_metrics_handler(struct mg_connection *conn, void *ud)
{
	(void)ud;
	const struct stats_snapshot *s = stats_snapshot_acquire();

	/* Copy metric to a local buffer before any blocking IO */
	char metric_copy[sizeof(s->metric)];
	size_t len = stats_payload_copy(metric_copy, sizeof(metric_copy),
		s->metric, s->metric_len);

	stats_snapshot_release(s);

	mg_printf(conn,
			"HTTP/1.1 200 OK\r\n"
			"Content-Type: text/plain; version=0.0.4\r\n"
			"Cache-Control: no-cache\r\n"
			"Content-Length: %zu\r\n"
			"Connection: close\r\n\r\n"
			"%s",
			len, metric_copy);

	return 1;   // KEY: handled by this callback
}

/* ================================================================ */
/* Broadcast                                                         */
/* ================================================================ */
/**
 * Keep the connection list locked until all writes finish. CivetWeb owns the
 * connection objects and may free one as soon as its close callback returns;
 * copying pointers and writing after releasing this lock would race with that
 * callback. Socket send timeouts bound the time a slow client can hold the
 * list lock.
 */
static void ws_broadcast(const char *data, size_t len)
{
	pthread_mutex_lock(&mgc_lock);
	for (int i = 0; i < n_mgc; i++) {
		mg_websocket_write((struct mg_connection *)mgc[i],
				   MG_WEBSOCKET_OPCODE_TEXT,
				   data, len);
	}
	pthread_mutex_unlock(&mgc_lock);
}

/* Push the current stats snapshot to all WebSocket clients as JSON.
 *
 * Claims a reader slot on the active buffer, copies the json payload
 * to a local buffer, releases the slot, then performs blocking WS
 * writes from the copy.  This prevents the producer from overwriting
 * the buffer mid-copy (eliminates the C-level data race). */
void ws_broadcast_stats(void)
{
	const struct stats_snapshot *s = stats_snapshot_acquire();
	if (s->json_len == 0) {
		stats_snapshot_release(s);
		return;
	}

	char json_copy[sizeof(s->json)];
	size_t len = stats_payload_copy(json_copy, sizeof(json_copy),
		s->json, s->json_len);

	stats_snapshot_release(s);

	ws_broadcast(json_copy, len);
}

/* Push a log message to all WebSocket clients. */
void ws_broadcast_log(char *log, size_t len)
{
	ws_broadcast(log, len);
}

/* Background broadcast thread */
static void *ws_broadcast_loop(void *arg)
{
	(void)arg;
	while (!atomic_load(&broadcast_stop)) {
		ws_broadcast_stats();
		usleep(333333);  /* 3 Hz -- enough for readable dashboards */
	}
	return NULL;
}

/* HTTP handler: ``/entropy`` -- serve the entropy dashboard HTML page. */
static int entropy_handler(struct mg_connection *conn, void *cbdata)
{
	(void)cbdata;
	mg_printf(conn,
			"HTTP/1.1 200 OK\r\n"
			"Content-Type: text/html\r\n"
			"Cache-Control: no-cache\r\n"
			"Content-Length: %u\r\n"
			"Connection: close\r\n\r\n"
			"%s", entropy_html_len, entropy_html);

	return 1;
}

/* HTTP handler: ``/observe`` -- serve the real-time observation HTML page. */
static int observe_handler(struct mg_connection *conn, void *cbdata)
{
	(void)cbdata;
	mg_printf(conn,
			"HTTP/1.1 200 OK\r\n"
			"Content-Type: text/html\r\n"
			"Cache-Control: no-cache\r\n"
			"Content-Length: %u\r\n"
			"Connection: close\r\n\r\n"
			"%s", observe_html_len, observe_html);

	return 1;
}

/* HTTP handler: ``/assets/chart.min.js`` -- serve embedded Chart.js from memory. */
static int chartjs_handler(struct mg_connection *conn, void *cbdata)
{
	(void)cbdata;
	mg_printf(conn,
		"HTTP/1.1 200 OK\r\n"
		"Content-Type: application/javascript\r\n"
		"Cache-Control: max-age=3600\r\n"
		"Content-Length: %u\r\n"
		"Connection: close\r\n\r\n",
		chartjs_len);
	mg_write(conn, chartjs,
		chartjs_len);
	return 1;
}

/* Catch-all WebSocket handler (currently passthrough). */
int ws_universal_handler(struct mg_connection *conn, void *cbdata)
{
	(void)conn;
	(void)cbdata;
	LOG_HINT("=== Universal handler called! ===");

	return 0;  // 0=continue, non-zero=handled
}

/* ================================================================ */
/* Server lifecycle                                                 */
/* ================================================================ */
struct mg_context * ws_server_start(void *data)
{
	mg_init_library(0);

	/* API key init happens before mg_start2 creates any server threads. */
	if (api_key_init() != 0) {
		LOG_ERR("ws_server_start: API key init failed");
		mg_exit_library();
		return NULL;
	}

	struct ws_user_data *wsud = NULL;
	struct server *srv;

	if (!data) {
		LOG_ERR("ws_server_start: NULL data -- cannot determine server config");
		return NULL;
	}

	wsud = (struct ws_user_data*)data;
	srv = wsud->conf;

	/* ── server-level policy checks (before mg_start2) ── */
	if (!srv->enable) {
		LOG_INFO("server.enable is false — HTTP server not started");
		mg_exit_library();
		return NULL;
	}

	/* If listening_ports contains non-loopback addresses, remote
	 * control must be explicitly opted-in. */
	const char *listen_ports = srv->cfg.civet_opts[0]
		? srv->cfg.listening_ports
		: (const char *)"127.0.0.1:8000";  /* SERVER_OPTIONS default */
	if (!control_policy_is_loopback_only(listen_ports)) {
		if (!srv->remote_control_enable) {
			fprintf(stderr,
				"ERROR: listening_ports includes non-loopback "
				"addresses (\"%s\"),\n"
				"       but remote_control_enable is not set.\n"
				"       Add 'remote_control_enable: true' to the "
				"server config to allow remote control,\n"
				"       or change listening_ports to a loopback-only "
				"address (e.g. \"127.0.0.1:8000\").\n",
				listen_ports);
			LOG_ERR("ws_server_start: non-loopback listener without "
				"remote_control_enable");
			mg_exit_library();
			return NULL;
		}
		if (api_key_not_required()) {
			fprintf(stderr,
				"ERROR: remote_control_enable is true but "
				"BLESS_API_KEY is not set.\n"
				"       Remote WebSocket control MUST be "
				"authenticated — set BLESS_API_KEY\n"
				"       to a strong random string and restart.\n");
			LOG_ERR("ws_server_start: remote control without "
				"API key — refusing to start");
			mg_exit_library();
			return NULL;
		}
	}

	struct mg_callbacks cb = {0};
	struct mg_init_data init = {
		.callbacks = &cb,
		.user_data = data,
		.configuration_options = srv->cfg.civet_opts[0] ? srv->cfg.civet_opts : SERVER_OPTIONS,
	};

	struct mg_error_data mg_start_error_data = {0};
	char errtxtbuf[256] = {0};
	mg_start_error_data.text = errtxtbuf;
	mg_start_error_data.text_buffer_size = sizeof(errtxtbuf);

	struct mg_context *ctx = mg_start2(&init, &mg_start_error_data);
	if (!ctx) {
		LOG_ERR("mg_start2 failed: %s", errtxtbuf);
		server_show(srv);
		fprintf(stderr, "Cannot start server: %s\n", errtxtbuf);
		mg_exit_library();
		return NULL;
	}

	/* Observation endpoints — always available regardless of control_enable. */
	mg_set_request_handler(ctx, "/api/stats", http_stats_handler, NULL);
	mg_set_request_handler(ctx, "/api/config", http_config_handler, NULL);
	mg_set_request_handler(ctx, "/metrics", http_metrics_handler, NULL);
	mg_set_request_handler(ctx, "/entropy", entropy_handler, NULL);
	mg_set_request_handler(ctx, "/observe", observe_handler, NULL);
	/* Static assets -- register BEFORE "/" so civetweb matches
	 * specific paths before the catch-all root handler. */
	mg_set_request_handler(ctx, "/assets/chart.min.js", chartjs_handler, NULL);

	/* ── control_enable gate: WS commands vs observation-only ── */
	if (srv->control_enable) {
		mg_set_request_handler(ctx, "/api/control",
				       http_control_handler, NULL);
		mg_set_websocket_handler(ctx,
				WS_URL,
				ws_connect_handler,
				ws_ready_handler,
				ws_data_handler,
				ws_close_handler,
				data);
		LOG_INFO("WS control handler registered (%s)", WS_URL);
	} else {
		LOG_INFO("server.control_enable is false — "
			 "read-only observation mode");
	}
	/* Root dashboard: write index.html to document_root so civetweb
	 * serves it as a static file for GET /.  We DON'T register a
	 * "/" handler because civetweb's "/" matches ALL paths as a
	 * prefix, shadowing more specific handlers and static assets. */
	{
		FILE *f = fopen("/tmp/index.html", "w");
		if (f) {
			fwrite(index_html, 1, index_html_len, f);
			fclose(f);
		} else {
			LOG_ERR("Cannot write /tmp/index.html: %s", strerror(errno));
		}
	}

	LOG_INFO("Websocket Server Started");

	/* Launch background broadcast thread -- its own thread blocks on
	 * WS writes so the DPDK main loop never stalls. */
	atomic_store(&broadcast_stop, 0);
	if (pthread_create(&broadcast_thread, NULL,
			   ws_broadcast_loop, NULL) != 0) {
		LOG_ERR("Cannot create broadcast thread: %s", strerror(errno));
	}

	return ctx;
}

/* Stop the background broadcast thread and civetweb server. */
int ws_server_stop(struct mg_context *ctx)
{
	/* Stop background broadcast thread first. */
	atomic_store(&broadcast_stop, 1);
	pthread_join(broadcast_thread, NULL);

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
	for (int i = 0; i < SERVER_OPTS_MAX && cfg->civet_opts[i * 2]; i++) {
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
	LOG_SHOW("%senable   %d", pref, srv->enable);
	LOG_SHOW("%scontrol_enable %d", pref, srv->control_enable);
	LOG_SHOW("%sremote_control_enable %d", pref, srv->remote_control_enable);
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
