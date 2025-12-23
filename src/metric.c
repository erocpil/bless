#include "metric.h"

static int collect_port_stats(uint16_t port_id,
		struct rte_eth_stats *stats,
		struct rte_eth_xstat_name **xnames,
		struct rte_eth_xstat **xvalues,
		int *xcount)
{
	if (rte_eth_stats_get(port_id, stats) != 0) {
		return -1;
	}

	int n = rte_eth_xstats_get_names(port_id, NULL, 0);
	if (n <= 0) {
		return -1;
	}

	*xnames  = calloc(n, sizeof(**xnames));
	*xvalues = calloc(n, sizeof(**xvalues));
	if (!*xnames || !*xvalues) {
		return -1;
	}

	rte_eth_xstats_get_names(port_id, *xnames, n);
	rte_eth_xstats_get(port_id, *xvalues, n);
	*xcount = n;

	return 0;
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

/*
 * 解析 rx_q0_packets / tx_q1_bytes
 * 返回 1 表示是 queue xstat
 */
static int parse_queue_xstat(const char *name,
		char *dir,        /* "rx" or "tx" */
		int *qid,
		const char **metric)
{
	/* rx_q0_packets */
	if (!(strncmp(name, "rx_q", 4) == 0 ||
				strncmp(name, "tx_q", 4) == 0))
		return 0;

	*dir = name[0];  /* r or t */

	const char *p = name + 4; /* q<id>_xxx */
	if (!isdigit(*p))
		return 0;

	*qid = atoi(p);

	p = strchr(p, '_');
	if (!p || *(p + 1) == '\0')
		return 0;

	*metric = p + 1;
	return 1;
}

/*
static const char * xstat_group(const char *name)
{
	if (strstr(name, "_q") || strstr(name, "queue")) {
		char dir;
		int qid;
		const char *metric;
		printf("queue(%s):\n\t", name);
		if (parse_queue_xstat(name, &dir, &qid, &metric)) {
			char qkey[16];
			snprintf(qkey, sizeof(qkey), "q%d", qid);
			printf("dir %c name %s qkey %s metric %s ", dir, name, qkey, metric);
		}
		return "queue";
	}
	if (strncmp(name, "rx_", 3) == 0)
		return "rx";
	if (strncmp(name, "tx_", 3) == 0)
		return "tx";
	if (strstr(name, "drop") || strstr(name, "discard"))
		return "drop";
	return "other";
}
*/

static cJSON * encode_xstats(uint16_t portid)
{
	int n = rte_eth_xstats_get_names(portid, NULL, 0);
	if (n <= 0)
		return NULL;

	struct rte_eth_xstat_name *names =
		rte_malloc(NULL, sizeof(*names) * n, 0);
	struct rte_eth_xstat *values =
		rte_malloc(NULL, sizeof(*values) * n, 0);

	if (!names || !values)
		goto fail;

	if (rte_eth_xstats_get_names(portid, names, n) != n)
		goto fail;

	if (rte_eth_xstats_get(portid, values, n) != n)
		goto fail;

	/* root xstats object */
	cJSON *root = cJSON_CreateObject();

#if 0
	/* fixed groups */
	cJSON *groups[5];
	const char *group_names[] = {
		"rx", "tx", "queue", "drop", "other"
	};

	for (int i = 0; i < 5; i++) {
		groups[i] = cJSON_CreateObject();
		cJSON_AddItemToObject(root, group_names[i], groups[i]);
	}
#endif

	for (int i = 0; i < n; i++) {
		/*
		const char *g = xstat_group(names[i].name);
		if (!strncmp(g, "queue", 5)) {
			printf("value %lu\n", values[i].value);
		}
		cJSON *grp = cJSON_GetObjectItem(root, g);
		*/
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


char * encode_stats_to_json(uint32_t port_mask, char *log_text)
{
	cJSON *root = cJSON_CreateObject();

	/* meta */
	cJSON *meta = cJSON_CreateObject();
	cJSON_AddNumberToObject(meta,
			"timestamp_ns",
			rte_get_timer_cycles() * 1000000000ULL /
			rte_get_timer_hz());
	cJSON_AddStringToObject(meta, "source", "Bless Injector");
	cJSON_AddNumberToObject(meta, "schema_version", 1);
	cJSON_AddItemToObject(root, "meta", meta);

	/* ports */
	cJSON *ports = cJSON_CreateObject();
	cJSON_AddItemToObject(root, "ports", ports);

	uint16_t portid = -1;
	RTE_ETH_FOREACH_DEV(portid) {
		if ((port_mask & (1 << portid)) == 0) {
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
	cJSON_AddStringToObject(log, "text", log_text ? log_text : "");

	char *out = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	return out;   /* caller free() */
}



#if 0
static void encode_port_to_json(cJSON *ports,
		uint16_t port_id,
		const struct rte_eth_stats *stats,
		const struct rte_eth_xstat_name *xnames,
		const struct rte_eth_xstat *xvalues,
		int xcount)
{
	char port_key[16];
	snprintf(port_key, sizeof(port_key), "%u", port_id);

	cJSON *port = cJSON_CreateObject();
	cJSON_AddItemToObject(ports, port_key, port);

	/* ---------- eth_stats ---------- */

	cJSON *stats_obj = cJSON_CreateObject();
	cJSON_AddItemToObject(port, "stats", stats_obj);

#define ADD_STAT(name) \
	cJSON_AddNumberToObject(stats_obj, #name, stats->name)

	ADD_STAT(ipackets);
	ADD_STAT(opackets);
	ADD_STAT(ibytes);
	ADD_STAT(obytes);
	ADD_STAT(imissed);
	ADD_STAT(ierrors);
	ADD_STAT(oerrors);
	ADD_STAT(rx_nombuf);

#undef ADD_STAT

	/* ---------- xstats ---------- */

	cJSON *xstats = cJSON_CreateObject();
	cJSON_AddItemToObject(port, "xstats", xstats);

	for (int i = 0; i < xcount; i++) {
		const char *name = xnames[xvalues[i].id].name;
		if (!name) {
			continue;
		}


		const char *group = xstat_group(name);

		cJSON *grp = cJSON_GetObjectItem(xstats, group);

		if (!grp) {
			grp = cJSON_CreateObject();
			cJSON_AddItemToObject(xstats, group, grp);
		}

		cJSON_AddNumberToObject(grp, name, xvalues[i].value);
	}
}

char * encode_all_ports_json(uint32_t port_mask)
{
	cJSON *root = cJSON_CreateObject();
	cJSON *ports = cJSON_CreateObject();
	cJSON_AddItemToObject(root, "ports", ports);

	uint16_t port_id = -1;
	RTE_ETH_FOREACH_DEV(port_id) {
		if ((port_mask & (1 << port_id)) == 0) {
			continue;
		}
		struct rte_eth_stats stats;
		struct rte_eth_xstat_name *xnames = NULL;
		struct rte_eth_xstat *xvalues = NULL;
		int xcount = 0;

		if (collect_port_stats(port_id, &stats,
					&xnames, &xvalues, &xcount) == 0) {

			encode_port_to_json(ports, port_id,
					&stats, xnames, xvalues, xcount);
		}

		free(xnames);
		free(xvalues);
	}

	char *json = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	return json;  /* caller free() */
}
#endif
