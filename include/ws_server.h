#ifndef __WS_SERVER_H__
#define __WS_SERVER_H__

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h> /* for sleep() */
#include "civetweb.h"
#include "define.h"

struct mg_context *ws_server_start(void *wsud);
int ws_server_stop(struct mg_context *ctx);
int ws_broadcast(const char *msg);

#endif
