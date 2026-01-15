#include "metric.h"

#define APPEND(fmt, ...) do {                                     \
	if (len >= max_len) {                                         \
		return max_len;                                           \
	}                                                             \
	int n = snprintf(msg + len, max_len - len, fmt, __VA_ARGS__); \
	if (n < 0) {                                                  \
		return len;                                               \
	}                                                             \
	if ((size_t)n >= max_len - len) {                             \
		len = max_len;                                            \
		return len;                                               \
	}                                                             \
	len += (size_t)n;                                             \
} while (0)

void* (*cbfn)(void) = NULL;

void metric_set_cbfn(void*(*metric_cbfn)())
{
	cbfn = metric_cbfn;
}

void metric_cpuset_to_str(const cpu_set_t *set, char *buf, size_t len)
{
	int first = 1;
	int start = -1;
	size_t off = 0;

	for (int cpu = 0; cpu <= CPU_SETSIZE; cpu++) {
		int is_set = (cpu < CPU_SETSIZE) && CPU_ISSET(cpu, set);

		if (is_set) {
			if (start < 0) {
				start = cpu;
			}
		} else if (start >= 0) {
			int end = cpu - 1;
			int n;

			if (!first) {
				n = snprintf(buf + off, len - off, ",");
			} else {
				n = 0;
			}

			off += n;

			if (start == end) {
				off += snprintf(buf + off, len - off, "%d", start);
			} else {
				off += snprintf(buf + off, len - off, "%d-%d", start, end);
			}

			first = 0;
			start = -1;
		}
	}
}

int bless_handle_system(const char *cmd, const char *params __rte_unused, struct rte_tel_data *d)
{
	RTE_SET_USED(cmd);
	RTE_SET_USED(params);

	struct system_status *sysstat = cbfn();
	rte_tel_data_start_dict(d);
	rte_tel_data_add_dict_int(d, "ppid", sysstat->ppid);
	rte_tel_data_add_dict_int(d, "pid", sysstat->pid);

#if 0
	char buf[256];
	metric_cpuset_to_str(&sysstat->cpuset, buf, sizeof(buf));
	rte_tel_data_add_dict_string(d, "cpu_affinity", buf);
#endif

	struct rte_tel_data *arr;
	arr = rte_tel_data_alloc();
	rte_tel_data_start_array(arr, RTE_TEL_UINT_VAL);
	for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
		if (CPU_ISSET(cpu, &sysstat->cpuset)) {
			rte_tel_data_add_array_uint(arr, cpu);
		}
	}
	rte_tel_data_add_dict_container(d, "cpu_affinity", arr, 0);

	return 0;
}

size_t encode_stats_to_text(uint32_t port_mask, char *msg, size_t max_len)
{
	size_t len = 0;
	uint16_t portid;
	RTE_ETH_FOREACH_DEV(portid) {
		if ((port_mask & (1u << portid)) == 0) {
			continue;
		}
		struct rte_eth_stats stats;
		if (rte_eth_stats_get(portid, &stats) != 0) {
			return 0;
		}

		APPEND("dpdk_ipackets{port=\"%u\"} %" PRIu64 "\n",
				portid, stats.ipackets);
		APPEND("dpdk_opackets{port=\"%u\"} %" PRIu64 "\n",
				portid, stats.opackets);
		APPEND("dpdk_ibytes{port=\"%u\"} %" PRIu64 "\n",
				portid, stats.ibytes);
		APPEND("dpdk_obytes{port=\"%u\"} %" PRIu64 "\n",
				portid, stats.obytes);
		APPEND("dpdk_imissed{port=\"%u\"} %" PRIu64 "\n",
				portid, stats.imissed);
		APPEND("dpdk_ierrors{port=\"%u\"} %" PRIu64 "\n",
				portid, stats.ierrors);
		APPEND("dpdk_oerrors{port=\"%u\"} %" PRIu64 "\n",
				portid, stats.oerrors);
		APPEND("dpdk_rx_nobuf{port=\"%u\"} %" PRIu64 "\n",
				portid, stats.rx_nombuf);
	}

	return len;
}

static cJSON * encode_eth_stats(const struct rte_eth_stats *s)
{
	cJSON *obj = cJSON_CreateObject();

	cJSON_AddNumberToObject(obj, "ipackets", s->ipackets);
	cJSON_AddNumberToObject(obj, "opackets", s->opackets);
	cJSON_AddNumberToObject(obj, "ibytes",   s->ibytes);
	cJSON_AddNumberToObject(obj, "obytes",   s->obytes);
	cJSON_AddNumberToObject(obj, "imissed",  s->imissed);
	cJSON_AddNumberToObject(obj, "ierrors",  s->ierrors);
	cJSON_AddNumberToObject(obj, "oerrors",  s->oerrors);
	cJSON_AddNumberToObject(obj, "rx_nombuf", s->rx_nombuf);

	return obj;
}

static cJSON * encode_xstats(uint16_t portid)
{
	int n = rte_eth_xstats_get_names(portid, NULL, 0);
	if (n <= 0) {
		return NULL;
	}

	struct rte_eth_xstat_name *names = rte_malloc(NULL, sizeof(*names) * n, 0);
	struct rte_eth_xstat *values = rte_malloc(NULL, sizeof(*values) * n, 0);

	if (!names || !values) {
		goto fail;
	}

	if (rte_eth_xstats_get_names(portid, names, n) != n) {
		goto fail;
	}

	if (rte_eth_xstats_get(portid, values, n) != n) {
		goto fail;
	}

	/* root xstats object */
	cJSON *root = cJSON_CreateObject();

	for (int i = 0; i < n; i++) {
		cJSON_AddNumberToObject(root, names[i].name, values[i].value);
	}

	rte_free(names);
	rte_free(values);

	return root;

fail:
	rte_free(names);
	rte_free(values);

	return NULL;
}

static cJSON * encode_port(uint16_t portid)
{
	struct rte_eth_stats stats;
	if (rte_eth_stats_get(portid, &stats) != 0)
		return NULL;

	cJSON *port = cJSON_CreateObject();

	cJSON_AddItemToObject(port,
			"stats",
			encode_eth_stats(&stats));

	cJSON *xstats = encode_xstats(portid);
	if (xstats)
		cJSON_AddItemToObject(port, "xstats", xstats);

	return port;
}

char * encode_cmdReply_to_json(const char *reply)
{
	cJSON *root = cJSON_CreateObject();

	cJSON_AddStringToObject(root, "cmdReply", reply ? reply : "null");
	char *out = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);

	return out;   /* caller free() */
}

char * encode_log_to_json(const char *log_text)
{
	cJSON *root = cJSON_CreateObject();
	cJSON *log = cJSON_CreateObject();

	cJSON_AddItemToObject(root, "log", log);
	cJSON_AddStringToObject(log, "text", log_text ? log_text : "null");
	char *out = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);

	return out;   /* caller free() */
}

char * encode_stats_to_json(uint32_t port_mask, char *log_text)
{
	cJSON *root = cJSON_CreateObject();

	/* meta */
	cJSON *meta = cJSON_CreateObject();

	uint64_t cycles = rte_get_timer_cycles();
	uint64_t hz = rte_get_timer_hz();
	uint64_t sec  = cycles / hz;
	uint64_t rem  = cycles % hz;
	uint64_t timestamp_ns = sec * 1000000000ULL + rem * 1000000000ULL / hz;

	cJSON_AddNumberToObject(meta, "timestamp_ns", timestamp_ns);
	cJSON_AddStringToObject(meta, "source", "Bless Injector");
	cJSON_AddNumberToObject(meta, "schema_version", 1);
	cJSON_AddItemToObject(root, "meta", meta);

	/* ports */
	cJSON *ports = cJSON_CreateObject();
	cJSON_AddItemToObject(root, "ports", ports);

	uint16_t portid;
	RTE_ETH_FOREACH_DEV(portid) {
		if ((port_mask & (1u << portid)) == 0) {
			continue;
		}
		char key[8];
		snprintf(key, sizeof(key), "%u", portid);

		cJSON *port = encode_port(portid);
		if (port)
			cJSON_AddItemToObject(ports, key, port);
	}

	cJSON *log = cJSON_CreateObject();
	cJSON_AddItemToObject(root, "log", log);
	cJSON_AddStringToObject(log, "text", log_text ? log_text : "null");

	char *out = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	return out;   /* caller free() */
}
