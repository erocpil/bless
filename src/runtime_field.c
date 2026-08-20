/* runtime_field.c -- field descriptor table and validation helpers.
 *
 * Single source of truth for every field exposed via the WebSocket
 * ``set`` / ``get`` API.  Generated from the descriptor table — no
 * hand-maintained if-else chains. */

#include "runtime_field.h"
#include "bless.h"
#include "worker.h"

#include <math.h>    /* isnormal, isfinite */
#include <string.h>

/* ──────────────────────────────────────────────────────────────────────
 * Forward declarations for apply callbacks (defined below the table)
 * ──────────────────────────────────────────────────────────────────── */

static void apply_sample_interval(void *bconf);
static void apply_pps_rate(void *bconf);
static void apply_entropy_target(void *bconf);
static void apply_entropy_dim(void *bconf);
static void apply_entropy_adapt_gain(void *bconf);

/* ──────────────────────────────────────────────────────────────────────
 * Field descriptor table
 *
 * Sorted alphabetically by name for linear scan.  Fields not listed
 * here are NOT accessible through the WebSocket API.
 *
 * Special fields (handled manually in ws_user_func):
 *   seed  — writes to global PRNG state, not bless_conf
 *
 * Mutability guidelines (Phase 1):
 *   RUNTIME  — safe to change mid-flight (propagated or self-reading)
 *   STARTUP  — requires restart or buffer reallocation
 * ──────────────────────────────────────────────────────────────────── */

const struct runtime_field runtime_fields[] = {
	{ "batch",             FIELD_U16, offsetof(struct bless_conf, batch),
	    1, 16384, FIELD_STARTUP, "buffer resize required; restart to apply",
	    validate_uint, NULL },

	{ "batch_delay_us",   FIELD_U64, offsetof(struct bless_conf, batch_delay_us),
	    0, UINT64_MAX, FIELD_STARTUP,
	    "worker snapshots at init; restart required",
	    validate_uint, NULL },

	{ "batch_jitter_us",  FIELD_U64, offsetof(struct bless_conf, batch_jitter_us),
	    0, UINT64_MAX, FIELD_STARTUP,
	    "worker snapshots at init; restart required",
	    validate_uint, NULL },

	{ "bps_burst",        FIELD_U32, offsetof(struct bless_conf, bps_burst),
	    0, UINT32_MAX, FIELD_STARTUP,
	    "token-bucket configured at init; restart required",
	    validate_uint, NULL },

	{ "bps_rate",         FIELD_U32, offsetof(struct bless_conf, bps_rate),
	    0, UINT32_MAX, FIELD_STARTUP,
	    "token-bucket configured at init; restart required",
	    validate_uint, NULL },

	{ "entropy_adapt_gain", FIELD_DOUBLE, offsetof(struct bless_conf, entropy_adapt_gain),
	    0, 1000, FIELD_RUNTIME,
	    "workers read runtime entropy_adapt_gain shadow",
	    validate_double, apply_entropy_adapt_gain },

	{ "entropy_dim",      FIELD_U8, offsetof(struct bless_conf, entropy_dim),
	    0, 8, FIELD_RUNTIME, NULL, validate_uint, apply_entropy_dim },

	{ "entropy_target",   FIELD_DOUBLE, offsetof(struct bless_conf, entropy_target),
	    0, 32, FIELD_RUNTIME,
	    "workers read runtime entropy_target shadow",
	    validate_double, apply_entropy_target },

	{ "hs_mix_ratio",     FIELD_U16, offsetof(struct bless_conf, hs_mix_ratio),
	    0, 1000, FIELD_STARTUP,
	    "worker snapshots at init; restart required",
	    validate_uint, NULL },

	{ "hs_rate",          FIELD_U32, offsetof(struct bless_conf, hs_rate),
	    0, UINT32_MAX, FIELD_STARTUP,
	    "token-bucket configured at init; restart required",
	    validate_uint, NULL },

	{ "hs_timeout_us",    FIELD_U64, offsetof(struct bless_conf, hs_timeout_us),
	    0, UINT64_MAX, FIELD_STARTUP,
	    "worker snapshots at init; restart required",
	    validate_uint, NULL },

	{ "num",              FIELD_I64, offsetof(struct bless_conf, num),
	    -1, INT64_MAX, FIELD_STARTUP,
	    "packet-count semantics require restart; use -1 for unlimited",
	    validate_sint, NULL },

	{ "pps_burst",        FIELD_U32, offsetof(struct bless_conf, pps_burst),
	    0, UINT32_MAX, FIELD_STARTUP,
	    "token-bucket configured at init; restart required",
	    validate_uint, NULL },

	{ "pps_rate",         FIELD_U32, offsetof(struct bless_conf, pps_rate),
	    0, UINT32_MAX, FIELD_RUNTIME, NULL, validate_uint, apply_pps_rate },

	{ "sample_interval",  FIELD_U32, offsetof(struct bless_conf, sample_interval),
	    0, UINT32_MAX, FIELD_RUNTIME, NULL, validate_uint, apply_sample_interval },

	{ "traffic_model",    FIELD_U8, offsetof(struct bless_conf, traffic_model),
	    0, 2, FIELD_STARTUP, "model change requires restart",
	    validate_uint, NULL },
};

const unsigned int runtime_field_count =
	sizeof(runtime_fields) / sizeof(runtime_fields[0]);

/* ──────────────────────────────────────────────────────────────────────
 * Lookup (linear scan — table is small, ~17 entries)
 * ──────────────────────────────────────────────────────────────────── */

const struct runtime_field *runtime_field_lookup(const char *name)
{
	for (unsigned int i = 0; i < runtime_field_count; i++) {
		if (strcmp(runtime_fields[i].name, name) == 0) {
			return &runtime_fields[i];
		}
	}
	return NULL;
}

/* ──────────────────────────────────────────────────────────────────────
 * Per-field apply callbacks
 * ──────────────────────────────────────────────────────────────────── */

static void apply_sample_interval(void *bconf_)
{
	struct bless_conf *bconf = (struct bless_conf *)bconf_;
	for (unsigned lc = 0; lc < RTE_MAX_LCORE; lc++) {
		if (entropy_samplers[lc]) {
			atomic_store_explicit(
			    &entropy_samplers[lc]->sample_interval,
			    bconf->sample_interval,
			    memory_order_relaxed);
		}
	}
}

static void apply_pps_rate(void *bconf_)
{
	struct bless_conf *bconf = (struct bless_conf *)bconf_;
	runtime_control_publish_pps_rate(&bconf->runtime, bconf->pps_rate);
}

static void apply_entropy_target(void *bconf_)
{
	struct bless_conf *bconf = (struct bless_conf *)bconf_;
	runtime_control_publish_entropy_target(&bconf->runtime,
					       bconf->entropy_target);
}

static void apply_entropy_dim(void *bconf_)
{
	struct bless_conf *bconf = (struct bless_conf *)bconf_;
	runtime_control_publish_entropy_dim(&bconf->runtime,
					    bconf->entropy_dim);
}

static void apply_entropy_adapt_gain(void *bconf_)
{
	struct bless_conf *bconf = (struct bless_conf *)bconf_;
	runtime_control_publish_entropy_adapt_gain(
		&bconf->runtime, bconf->entropy_adapt_gain);
}

/* ──────────────────────────────────────────────────────────────────────
 * Validation helpers
 * ──────────────────────────────────────────────────────────────────── */

bool validate_uint(const cJSON *val, const struct runtime_field *f,
		   struct field_value *out)
{
	if (!cJSON_IsNumber(val)) {
		return false;
	}

	double d = cJSON_GetNumberValue(val);

	/* Reject NaN and infinity */
	if (!isfinite(d)) {
		return false;
	}

	/* Reject negative */
	if (d < 0.0) {
		return false;
	}

	/* Reject values at or beyond the integer range (avoid UB on cast).
	 * (double)UINT64_MAX rounds to 0x1p64 (2^64 exactly), so use the
	 * hex literal to reject the boundary value as well. */
	if (d >= 0x1p64) {
		return false;
	}

	/* Reject non-integer (fractional part) */
	uint64_t u = (uint64_t)d;
	if (d != (double)u) {
		return false;
	}

	/* Descriptor range check */
	if (u < f->min || u > f->max) {
		return false;
	}

	out->ok   = true;
	out->u64  = u;
	return true;
}

bool validate_sint(const cJSON *val, const struct runtime_field *f,
		   struct field_value *out)
{
	if (!cJSON_IsNumber(val)) {
		return false;
	}

	double d = cJSON_GetNumberValue(val);

	/* Reject NaN and infinity */
	if (!isfinite(d)) {
		return false;
	}

	/* Reject values beyond the signed 64-bit range (avoid UB on cast).
	 * (double)INT64_MAX rounds to 0x1p63 (2^63 = INT64_MAX + 1), so
	 * use d >= 0x1p63 for the upper bound.  The lower bound (double)INT64_MIN
	 * is exact (-0x1p63), so d < -0x1p63 is the semantically correct check
	 * but d < (double)INT64_MIN also works. */
	/* cppcheck-suppress incorrectLogicOperator ; hex-float 0x1p63 is
	 * ~9.22e18; cppcheck 2.13 misinterprets it and claims the condition
	 * is always true, but double can represent values well beyond this. */
	if (d < -0x1p63 || d >= 0x1p63) {
		return false;
	}

	/* Reject non-integer */
	int64_t s = (int64_t)d;
	if (d != (double)s) {
		return false;
	}

	/* Descriptor range check */
	if (s < (int64_t)f->min || s > (int64_t)f->max) {
		return false;
	}

	out->ok  = true;
	out->i64 = s;
	return true;
}

bool validate_double(const cJSON *val, const struct runtime_field *f,
		     struct field_value *out)
{
	if (!cJSON_IsNumber(val)) {
		return false;
	}

	double d = cJSON_GetNumberValue(val);

	/* Reject NaN and infinity */
	if (!isfinite(d)) {
		return false;
	}

	/* Range check (as double) */
	if (d < (double)f->min || d > (double)f->max) {
		return false;
	}

	out->ok  = true;
	out->f64 = d;
	return true;
}

/* ──────────────────────────────────────────────────────────────────────
 * Get helper — read a field from bless_conf into cJSON
 * ──────────────────────────────────────────────────────────────────── */

static double read_field_as_double(const struct runtime_field *f, void *bconf)
{
	switch (f->type) {
	case FIELD_U8:  return (double)*(uint8_t  *)((char *)bconf + f->offset);
	case FIELD_U16: return (double)*(uint16_t *)((char *)bconf + f->offset);
	case FIELD_U32: return (double)*(uint32_t *)((char *)bconf + f->offset);
	case FIELD_U64: return (double)*(uint64_t *)((char *)bconf + f->offset);
	case FIELD_I64: return (double)*(int64_t  *)((char *)bconf + f->offset);
	case FIELD_DOUBLE:
		/* cppcheck-suppress invalidPointerCast */
		return *(double *)((char *)bconf + f->offset);
	}
	return 0.0;
}

void runtime_field_get_json(const struct runtime_field *f, void *bconf,
			    cJSON *obj)
{
	double v = read_field_as_double(f, bconf);
	cJSON_AddNumberToObject(obj, f->name, v);
}
