#include "ws_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>

#include "civetweb.h"

/* ------------------------------------------------------------------ */
/* Global options                                                      */
/* ------------------------------------------------------------------ */

static const char WS_URL[] = "/wsURL";

static const char *SERVER_OPTIONS[] = {
	"listening_ports", "8000",
	"num_threads", "4",
	"enable_keep_alive", "yes",
	"request_timeout_ms", "2000",
	NULL
};

/* ------------------------------------------------------------------ */
/* WebSocket sub-protocols                                             */
/* ------------------------------------------------------------------ */

static const char subprotocol_bin[]  = "Company.ProtoName.bin";
static const char subprotocol_json[] = "Company.ProtoName.json";

static const char *subprotocols[] = {
	subprotocol_bin,
	subprotocol_json,
	NULL
};

static struct mg_websocket_subprotocols wsprot = {
	2,
	subprotocols
};

/* ------------------------------------------------------------------ */
/* Client tracking (thread-safe)                                      */
/* ------------------------------------------------------------------ */

#define MGC_SIZE 16

static const struct mg_connection *mgc[MGC_SIZE];
static int n_mgc = 0;

static pthread_mutex_t mgc_lock = PTHREAD_MUTEX_INITIALIZER;

/* ------------------------------------------------------------------ */
/* WebSocket client context                                           */
/* ------------------------------------------------------------------ */

struct tClientContext {
	uint32_t connectionNumber;
	uint32_t demo_var;
};

/* ------------------------------------------------------------------ */
/* WebSocket handlers                                                 */
/* ------------------------------------------------------------------ */

static int ws_connect_handler(const struct mg_connection *conn, void *user_data)
{
	(void)user_data;

	pthread_mutex_lock(&mgc_lock);
	if (n_mgc >= MGC_SIZE) {
		pthread_mutex_unlock(&mgc_lock);
		return 1;
	}

	struct tClientContext *ctx =
		calloc(1, sizeof(struct tClientContext));
	if (!ctx) {
		pthread_mutex_unlock(&mgc_lock);
		return 1;
	}

	static uint32_t conn_counter = 0;
	ctx->connectionNumber =
		__sync_add_and_fetch(&conn_counter, 1);

	mg_set_user_connection_data(conn, ctx);

	mgc[n_mgc++] = conn;
	pthread_mutex_unlock(&mgc_lock);

	const struct mg_request_info *ri =
		mg_get_request_info(conn);

	printf("WS client %u connected, subprotocol=%s\n",
			ctx->connectionNumber,
			ri->acceptedWebSocketSubprotocol ?
			ri->acceptedWebSocketSubprotocol : "(none)");

	return 0;
}

static void ws_ready_handler(struct mg_connection *conn, void *user_data)
{
	(void)user_data;

	struct tClientContext *ctx =
		mg_get_user_connection_data(conn);

	const char *hello = "{ \"hello\": \"world\" }\n";
	mg_websocket_write(conn,
			MG_WEBSOCKET_OPCODE_TEXT,
			hello,
			strlen(hello));

	printf("WS client %u ready\n", ctx->connectionNumber);
}

static int ws_data_handler(struct mg_connection *conn,
		int opcode,
		char *data,
		size_t datasize,
		void *user_data)
{
	struct ws_user_data *wsud =
		(struct ws_user_data *)user_data;

	struct tClientContext *ctx =
		mg_get_user_connection_data(conn);

	int type = opcode & 0xf;

	if (type == MG_WEBSOCKET_OPCODE_TEXT) {
		char buf[512];
		size_t n = datasize < sizeof(buf) - 1 ?
			datasize : sizeof(buf) - 1;

		memcpy(buf, data, n);
		buf[n] = '\0';

		printf("WS client %u: %s\n",
				ctx->connectionNumber, buf);

		if (strcmp(buf, "start") == 0) {
			atomic_store_explicit(wsud->state,
					STATE_RUNNING,
					memory_order_release);
		} else if (strcmp(buf, "stop") == 0) {
			atomic_store_explicit(wsud->state,
					STATE_STOPPED,
					memory_order_release);
		}
	}

	return 1;
}

static void ws_close_handler(const struct mg_connection *conn, void *user_data)
{
	(void)user_data;

	struct tClientContext *ctx =
		mg_get_user_connection_data(conn);

	pthread_mutex_lock(&mgc_lock);

	for (int i = 0; i < n_mgc; i++) {
		if (mgc[i] == conn) {
			for (int j = i; j < n_mgc - 1; j++) {
				mgc[j] = mgc[j + 1];
			}
			n_mgc--;
			mgc[n_mgc] = NULL;
			break;
		}
	}

	pthread_mutex_unlock(&mgc_lock);

	printf("WS client %u disconnected\n",
			ctx->connectionNumber);

	free(ctx);
}

/* ------------------------------------------------------------------ */
/* HTTP handlers                                                      */
/* ------------------------------------------------------------------ */
/**
  curl http://127.0.0.1:8000/api/status
  */
static int http_status_handler(struct mg_connection *conn, void *user_data)
{
	struct ws_user_data *wsud =
		(struct ws_user_data *)user_data;

	int state =
		atomic_load_explicit(wsud->state,
				memory_order_acquire);

	pthread_mutex_lock(&mgc_lock);
	int clients = n_mgc;
	pthread_mutex_unlock(&mgc_lock);

	mg_printf(conn,
			"HTTP/1.1 200 OK\r\n"
			"Content-Type: application/json\r\n"
			"Cache-Control: no-cache\r\n\r\n"
			"{ \"state\": %d, \"clients\": %d }\n",
			state, clients);

	return 200;
}

/**
  curl -X POST http://127.0.0.1:8000/api/control \
  -H "Content-Type: text/plain" \
  -d "start"

  curl -X POST http://127.0.0.1:8000/api/control \
  -H "Content-Type: application/json" \
  -d '{"cmd":"start"}'
  */
/*
   curl -v -X POST http://127.0.0.1:8000/api/control -d "start"

   curl --connect-timeout 1 --max-time 2 \
   -X POST http://127.0.0.1:8000/api/control \
   -d "stop"

#!/bin/bash
curl -s -X POST http://127.0.0.1:8000/api/control -d "$1"

*/
static int http_control_handler(struct mg_connection *conn, void *user_data)
{
	struct ws_user_data *wsud =
		(struct ws_user_data *)user_data;

	const struct mg_request_info *ri =
		mg_get_request_info(conn);

	if (strcmp(ri->request_method, "POST") != 0) {
		mg_send_http_error(conn, 405, "Method Not Allowed");
		return 405;
	}

	char buf[256];
	int len = mg_read(conn, buf, sizeof(buf) - 1);
	if (len <= 0) {
		mg_send_http_error(conn, 400, "Bad Request");
		return 400;
	}
	buf[len] = '\0';

	if (strcmp(buf, "start") == 0) { // strstr
		atomic_store_explicit(wsud->state,
				STATE_RUNNING,
				memory_order_release);
		printf("Control> start\n");
	} else if (strcmp(buf, "stop") == 0) {
		atomic_store_explicit(wsud->state,
				STATE_STOPPED,
				memory_order_release);
		printf("Control> stop\n");
	}

	mg_printf(conn,
			"HTTP/1.1 200 OK\r\n"
			"Content-Type: application/json\r\n\r\n"
			"{ \"result\": \"ok\" }\n");

	return 200;
}

/* ------------------------------------------------------------------ */
/* Server lifecycle                                                   */
/* ------------------------------------------------------------------ */

struct mg_context * ws_server_start(void *wsud)
{
	mg_init_library(0);

	struct mg_callbacks callbacks = {0};

	struct mg_init_data init = {0};
	init.callbacks = &callbacks;
	init.user_data = wsud;
	init.configuration_options = SERVER_OPTIONS;

	struct mg_error_data err = {0};
	char errbuf[256] = {0};
	err.text = errbuf;
	err.text_buffer_size = sizeof(errbuf);

	struct mg_context *ctx =
		mg_start2(&init, &err);

	if (!ctx) {
		fprintf(stderr, "mg_start failed: %s\n", errbuf);
		mg_exit_library();
		return NULL;
	}

	mg_set_websocket_handler_with_subprotocols(
			ctx,
			WS_URL,
			&wsprot,
			ws_connect_handler,
			ws_ready_handler,
			ws_data_handler,
			ws_close_handler,
			wsud
			);

	mg_set_request_handler(ctx,
			"/api/status",
			http_status_handler,
			wsud);

	mg_set_request_handler(ctx,
			"/api/control",
			http_control_handler,
			wsud);

	printf("WS/HTTP server started on port 8000\n");

	return ctx;
}

int ws_server_stop(struct mg_context *ctx)
{
	printf("WS/HTTP server stopping\n");
	mg_stop(ctx);
	mg_exit_library();
	return 0;
}

/* ------------------------------------------------------------------ */
/* Broadcast                                                          */
/* ------------------------------------------------------------------ */
int ws_broadcast(const char *msg)
{
	if (!msg)
		return -1;

	size_t len = strlen(msg);

	pthread_mutex_lock(&mgc_lock);
	for (int i = 0; i < n_mgc; i++) {
		mg_websocket_write(
				(struct mg_connection *)mgc[i],
				MG_WEBSOCKET_OPCODE_TEXT,
				msg,
				len);
	}
	pthread_mutex_unlock(&mgc_lock);

	return 0;
}
