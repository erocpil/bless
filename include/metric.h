#ifndef __METRICS_H__
#define __METRICS_H__

#include <pthread.h>
#include <unistd.h>

#include <rte_lcore.h>
#include <rte_telemetry.h>
#include <rte_debug.h>

#include "bless.h"
#include "cJSON.h"
int bless_handle_system(const char *cmd, const char *params, struct rte_tel_data *info);
char * encode_all_ports_json(uint32_t port_mask);
char * encode_cmdReply_to_json(const char *reply);
char * encode_log_to_json(const char *log_text);
char * encode_stats_to_json(uint32_t port_mask, char *log_text);
size_t encode_stats_to_text(uint32_t port_mask, char *msg, size_t len);
void metric_set_cbfn(void*(*metric_cbfn)());

#endif
