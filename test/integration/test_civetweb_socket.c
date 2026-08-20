#include <assert.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "civetweb.h"

static int health(struct mg_connection *c, void *unused)
{
	(void)unused;
	mg_printf(c, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nOK");
	return 1;
}

static int stream(struct mg_connection *c, void *unused)
{
	(void)unused;
	static const char chunk[4096] = {0};
	mg_printf(c, "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n\r\n");
	for (int i = 0; i < 1024; i++)
		if (mg_write(c, chunk, sizeof(chunk)) <= 0)
			break;
	return 1;
}

static int ws_connect(const struct mg_connection *c, void *unused)
{
	(void)unused;
	const struct mg_request_info *ri = mg_get_request_info(c);
	return !(ri && ri->query_string &&
	         strstr(ri->query_string, "key=test-key") != NULL);
}
static void ws_ready(struct mg_connection *c, void *unused)
{ (void)c; (void)unused; }
static int ws_data(struct mg_connection *c, int opcode, char *data,
	 size_t len, void *unused)
{ (void)c; (void)opcode; (void)data; (void)len; (void)unused; return 1; }
static void ws_close(const struct mg_connection *c, void *unused)
{ (void)c; (void)unused; }

int main(void)
{
	static const char *opts[] = {"listening_ports", "127.0.0.1:18765",
		"num_threads", "1", NULL};
	struct mg_callbacks cb = {0};
	struct mg_init_data init = { .callbacks = &cb,
		.configuration_options = opts };
	struct mg_error_data err = {0};
	char errbuf[256] = {0};
	err.text = errbuf; err.text_buffer_size = sizeof(errbuf);
	struct mg_context *ctx = mg_start2(&init, &err);
	if (!ctx) { fprintf(stderr, "mg_start2 failed: %s\n", errbuf); return 1; }
	mg_set_request_handler(ctx, "/health", health, NULL);
	mg_set_request_handler(ctx, "/stream", stream, NULL);
	mg_set_websocket_handler(ctx, "/wsURL", ws_connect, ws_ready,
	                         ws_data, ws_close, NULL);
	int fd = socket(AF_INET, SOCK_STREAM, 0); assert(fd >= 0);
	struct sockaddr_in addr = { .sin_family = AF_INET,
		.sin_port = htons(18765) };
	assert(inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) == 1);
	assert(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
	const char req[] = "GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n";
	assert(send(fd, req, sizeof(req) - 1, 0) == (ssize_t)(sizeof(req) - 1));
	char response[256] = {0};
	ssize_t n = recv(fd, response, sizeof(response) - 1, 0);
	assert(n > 0 && strstr(response, "200 OK") && strstr(response, "OK"));
	close(fd);
	/* A client that never reads must not make the server process hang after
	 * it disconnects.  The handler writes 4 MiB to induce back-pressure. */
	fd = socket(AF_INET, SOCK_STREAM, 0); assert(fd >= 0);
	int rcv = 4096;
	(void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof(rcv));
	assert(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
	const char stream_req[] = "GET /stream HTTP/1.1\r\nHost: localhost\r\n\r\n";
	assert(send(fd, stream_req, sizeof(stream_req) - 1, 0) > 0);
	struct timespec pause = {0, 20000000};
	nanosleep(&pause, NULL);
	close(fd);
	fd = socket(AF_INET, SOCK_STREAM, 0); assert(fd >= 0);
	assert(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
	const char ws_req[] =
		"GET /wsURL?key=test HTTP/1.1\r\n"
		"Host: localhost\r\nUpgrade: websocket\r\n"
		"Connection: Upgrade\r\nSec-WebSocket-Version: 13\r\n"
		"Sec-WebSocket-Key: SGVsbG9TdGF0ZUtleQ==\r\n\r\n";
	assert(send(fd, ws_req, sizeof(ws_req) - 1, 0) ==
	       (ssize_t)(sizeof(ws_req) - 1));
	memset(response, 0, sizeof(response));
	n = recv(fd, response, sizeof(response) - 1, 0);
	/* CivetWeb may reject an unauthorized upgrade by closing without a
	 * response; either that or a non-101 response is the expected result. */
	assert(n <= 0 || strstr(response, "101 Switching Protocols") == NULL);
	close(fd);
	fd = socket(AF_INET, SOCK_STREAM, 0); assert(fd >= 0);
	assert(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
	const char ws_ok[] =
		"GET /wsURL?key=test-key HTTP/1.1\r\n"
		"Host: localhost\r\nUpgrade: websocket\r\n"
		"Connection: Upgrade\r\nSec-WebSocket-Version: 13\r\n"
		"Sec-WebSocket-Key: SGVsbG9TdGF0ZUtleQ==\r\n\r\n";
	assert(send(fd, ws_ok, sizeof(ws_ok) - 1, 0) ==
	       (ssize_t)(sizeof(ws_ok) - 1));
	memset(response, 0, sizeof(response));
	n = recv(fd, response, sizeof(response) - 1, 0);
	assert(n > 0 && strstr(response, "101 Switching Protocols"));
	close(fd);
	mg_stop(ctx);
	puts("civetweb loopback socket: PASS");
	return 0;
}
