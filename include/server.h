#ifndef __WS_SERVER_H__
#define __WS_SERVER_H__

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <unistd.h> /* for sleep() */
#include "civetweb.h"
#include "entropy_diag.h"

#define STATS_JSON_MAX       16384
#define STATS_METRIC_MAX     16384
#define BLESS_WS_COMMAND_MAX 4096   /* max WS text frame payload (bytes) */

struct ws_user_data {
	void *conf;
	void *data;
	void (*func)(struct mg_connection *conn, void *user,
		     void *data, size_t size);
};

struct stats_snapshot {
    uint64_t tsc_cycles;
	uint16_t effective_batch;
	uint8_t effective_traffic_model;
	uint64_t effective_batch_delay_us;
	uint64_t effective_batch_jitter_us;
	uint32_t effective_sample_interval;

    size_t json_len;
    char   json[STATS_JSON_MAX << 1];

    size_t metric_len;
    char   metric[STATS_METRIC_MAX << 1];

    /* per-dimension Shannon entropy (bits) */    double entropy_protocol;     /* protocol distribution */
    double entropy_src_ip;       /* src IP space (inner) */
    double entropy_dst_ip;       /* dst IP space (inner) */
    double entropy_src_port;     /* src port space (inner) */
    double entropy_dst_port;     /* dst port space (inner) */
    double entropy_pkt_size;     /* observed packet sizes (IMIX) */
    double entropy_vxlan_encap;  /* VXLAN vs non-VXLAN mix */
    double entropy_outer_src_ip;       /* outer src IP (VXLAN) */
    double entropy_outer_dst_ip;       /* outer dst IP (VXLAN) */
    double entropy_outer_src_port;     /* outer UDP src port (VXLAN) */
    double entropy_vni;                /* VXLAN VNI space */
    double entropy_tcp_flags;          /* TCP flag byte distribution */
    double entropy_total_5tuple; /* combined inner 5-tuple + pkt_size */

    /* P0a: timing entropy */    double entropy_delta_tsc;    /* inter-packet timing (TSC delta) */

    /* P0b: distinct flow count */    double flow_distinct;        /* number of distinct flows seen */
    double flow_total;           /* total packets tracked for flow ratio */
    double flow_ratio;           /* distinct / total (0-1) */
    double sampler_samples;      /* samples used by the current calculation */
    double sampler_overwritten;  /* cumulative unread samples replaced */
    double sampler_overwritten_window; /* replacements since last calculation */

    /* P0c: usage ratio (max possible vs measured) */    double max_src_ip;           /* log₂(config src_ip range) */
    double max_dst_ip;           /* log₂(config dst_ip range) */
    double max_src_port;         /* log₂(config src_port range) */
    double max_dst_port;         /* log₂(config dst_port range) */
    double max_outer_src_ip;     /* log₂(VXLAN config outer_src range) */
    double max_outer_dst_ip;     /* log₂(VXLAN config outer_dst range) */
    double max_vni;              /* log₂(VXLAN config VNI count) */

    /* P1: min-entropy per dimension */    double entropy_min_protocol;
    double entropy_min_src_ip;
    double entropy_min_dst_ip;
    double entropy_min_src_port;
    double entropy_min_dst_port;
    double entropy_min_pkt_size;
    double entropy_min_tcp_flags;
    double entropy_min_delta_tsc;
    double entropy_min_outer_src_ip;
    double entropy_min_outer_dst_ip;
    double entropy_min_vni;
    double entropy_min_total_5tuple;
	struct entropy_min_diagnostic min_diag[ENTROPY_MIN_DIM_COUNT];

    /* P1: joint 5-tuple entropy (cross-dimension correlation) */    double entropy_joint_5tuple;

    /* mutual information matrix (key pairs) */    double mi_sip_dip;      /* I(src_ip; dst_ip) -- source-destination affinity */
    double mi_spt_dpt;      /* I(src_port; dst_port) -- port-pair correlation */
    double mi_proto_spt;    /* I(protocol; src_port) -- protocol-port binding */
    /* Tier 2: traffic profile correlation */
    double mi_size_dpt;     /* I(pkt_size; dst_port) -- service-specific payload sizes */
    double mi_size_proto;   /* I(pkt_size; protocol) -- protocol-specific frame sizes */
    double mi_dtsc_proto;   /* I(delta_tsc; protocol) -- protocol timing patterns */
    double mi_dtsc_flow;    /* I(delta_tsc; flow_key) -- temporal locality of flows (interleave) */
    /* Tier 3: TCP behaviour */
    double mi_tcpf_sz;      /* I(tcp_flags; pkt_size) -- flag-size correlation */
    double mi_tcpf_spt;     /* I(tcp_flags; src_port) -- per-flow flag usage */
    double mi_tcpf_dpt;     /* I(tcp_flags; dst_port) -- service-specific flag patterns */
    /* Tier 4: VXLAN tunnel */
    double mi_osip_odip;    /* I(outer_src_ip; outer_dst_ip) -- VTEP pair affinity */
    double mi_vni_osip;     /* I(vni; outer_src_ip) -- VNI-to-VTEP mapping */

    /* per-pair observed upper bound I(X;Y) <= min(H(X), H(Y)) */ double mi_max_sip_dip;
    double mi_max_spt_dpt;
    double mi_max_proto_spt;
    double mi_max_size_dpt;
    double mi_max_size_proto;
    double mi_max_dtsc_proto;
    double mi_max_dtsc_flow;
    double mi_max_tcpf_sz;
    double mi_max_tcpf_spt;
    double mi_max_tcpf_dpt;
    double mi_max_osip_odip;
    double mi_max_vni_osip;

    /* handshake mode stats (0.0 when not in handshake mode) */    double hs_syn_sent;
    double hs_syn_recv;
    double hs_synack_sent;
    double hs_synack_recv;
    double hs_ack_sent;
    double hs_established;
    double hs_rst_sent;
    double hs_rst_recv;
    double hs_timed_out;
    double hs_conn_current;
    double hs_conn_max;
    double hs_success_rate;    /* established / syn_sent */
    double hs_cps;             /* connections per second */

    /* flow-level entropy (handshake mode) */    double flow_entropy_5tuple;     /* per-flow 5-tuple distribution */
    double flow_entropy_lifetime;   /* connection lifetime distribution */
    double flow_entropy_event;      /* CREATED/ESTABLISHED/TIMEOUT/RST mix */
    double flow_count;              /* total flow events sampled */
	double flow_lifetime_count;     /* terminal-flow samples in window */
	double flow_event_count;        /* lifecycle samples in window */

    /* observe panel: throughput, loss, CPU, memory */    double rx_mpps, tx_mpps;
    double rx_gbps, tx_gbps;
    double rx_loss_rate;            /* imissed / (ipackets + imissed) over interval */
    double process_cpu_cores;        /* process CPU seconds / wall-clock second */
    double enabled_lcore_utilization_ratio; /* process_cpu_cores / enabled_lcores */
    uint32_t enabled_lcores;         /* rte_lcore_count() normalization denominator */
    double cpu_busy_pct;             /* deprecated: min(ratio * 100, 100) */
    /* TX hot-path timing breakdown (per-packet TSC cycles, windowed).
     * build + submit = real send cost (excludes pacing busy-wait);
     * wait_ratio = pacing idle as a fraction of build+wait+submit. */
    double tx_build_cycles_per_pkt;
    double tx_submit_cycles_per_pkt;
    double tx_cycles_per_pkt;       /* build + submit */
    double tx_wait_ratio;           /* 0.0 – 1.0 */
    uint64_t mem_rss_kb;            /* VmRSS from /proc/self/status */
    uint64_t cpu_freq_min_khz;
    uint64_t cpu_freq_max_khz;
    uint64_t voluntary_ctx_switches;
    uint64_t involuntary_ctx_switches;

    /* host-to-PMD burst submission timing; not physical wire timing */
    double tx_submit_target_us;
    double tx_submit_overshoot_p50_us;
    double tx_submit_overshoot_p99_us;
    double tx_submit_overshoot_max_us;
    double tx_burst_duration_p50_us;
    double tx_burst_duration_p99_us;
    double tx_burst_duration_max_us;
    uint64_t tx_submit_samples;

    /* rate PSD (frequency-domain analysis of TX rate) */
    double psd_dominant_hz;        /* compatibility alias for strongest peak */
    double psd_strongest_peak_hz;  /* frequency with highest spectral power */
    double psd_fundamental_hz;     /* lowest supported harmonic-series peak */
    double psd_spectral_flatness;  /* 0 = pure tone, 1 = white noise */
    double psd_mean_ppms;           /* mean TX rate normalized to packets/ms */
    double psd_variation_rms_ppms;  /* RMS non-DC TX variation, packets/ms */
    int psd_signal_valid;           /* non-zero when non-DC spectral power exists */
    double psd_bins[256];          /* PSD power per frequency bin (0..Nyquist) */

    /* latency histogram (percentiles in µs) */    double lat_p50, lat_p95, lat_p99, lat_p999;
    uint64_t lat_samples;           /* total latency samples in this window */
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

/* ====== YAML -> struct ====== */
struct server_options_cfg {
#define X(name, type, key, def) \
	type name;
#include "server_options.def"
#undef X
	/* One entry per option */
	struct civet_kv kv[SERVER_OPTS_MAX];
	/* key,value,...,NULL */
	const char *civet_opts[(SERVER_OPTS_MAX << 1) + 1];
};

#define SERVER_SERVICE_HTTP_MAX 16
#define SERVER_SERVICE_HTTP_LEN_MAX 128
struct server_service {
	char *websocket_uri;
	uint16_t n_http;
	char http[SERVER_SERVICE_HTTP_MAX][SERVER_SERVICE_HTTP_LEN_MAX];
};

struct server {
	struct mg_context *ctx;
	struct ws_user_data wsud;
	struct server_service svc;
	struct server_options_cfg cfg;
	int enable;                  /* server.enable */
	int control_enable;          /* server.control_enable */
	int remote_control_enable;   /* server.remote_control_enable */
};

/* YAML value -> cfg */
#define SERVER_PARSE_STR(dst, v)   ((dst) = (v))
#define SERVER_PARSE_INT(dst, v)   ((dst) = (int)strtol((v), NULL, 10))
#define SERVER_PARSE_BOOL(dst, v)  \
	((dst) = (!strcmp(v,"yes") || !strcmp(v,"true") || !strcmp(v,"1")))

/* Value to string */
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
const struct stats_snapshot *stats_snapshot_acquire(void);
void stats_snapshot_release(const struct stats_snapshot *snapshot);
int stats_snapshot_writable(int idx);
int stats_get_active_index(void);
struct stats_snapshot * stats_get(int idx);
void stats_set(int idx);
void server_config_template();

#endif
