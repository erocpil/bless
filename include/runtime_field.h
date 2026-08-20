#ifndef __BLESS_RUNTIME_FIELD_H__
#define __BLESS_RUNTIME_FIELD_H__

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Value types for runtime fields. */
enum field_type {
	FIELD_U32,       /* uint32_t */
	FIELD_U64,       /* uint64_t */
	FIELD_I64,       /* int64_t */
	FIELD_U16,       /* uint16_t */
	FIELD_U8,        /* uint8_t */
	FIELD_DOUBLE,    /* double (floating-point, allows fractional) */
};

/** Mutability classification. */
enum field_mutable {
	FIELD_RUNTIME  = 0,   /* settable at any time */
	FIELD_STARTUP  = 1,   /* only settable before first start */
};

/** Maximum field name length (including NUL). */
#define FIELD_NAME_MAX 32

/** Checked parse result from a JSON value. */
struct field_value {
	bool     ok;
	union {
		uint64_t u64;
		int64_t  i64;
		double   f64;
	};
};

/**
 * Runtime field descriptor.
 *
 * One entry per field exposed through the WebSocket ``set`` / ``get`` API.
 * The descriptor table is the single source of truth for field names,
 * types, ranges, and mutability.
 */
struct runtime_field {
	const char         *name;          /* JSON key */
	enum field_type     type;          /* C type of the field */
	size_t              offset;        /* offsetof(struct bless_conf, field) */
	uint64_t            min;           /* inclusive minimum (interpreted per type) */
	uint64_t            max;           /* inclusive maximum */
	enum field_mutable  mutability;    /* runtime vs startup-only */
	const char         *apply_desc;    /* human-readable apply note (or NULL) */

	/** Validate a JSON value before applying.
	 *
	 *  Returns true if the value is valid for this field.
	 *  On false, the caller should NOT write to the field. */
	bool (*validate)(const cJSON *val, const struct runtime_field *f,
			 struct field_value *out);

	/** Post-apply callback.
	 *
	 *  Called AFTER the field value has been written to the struct.
	 *  May be NULL.  Receives a pointer to the bless_conf. */
	void (*apply)(void *bconf);
};

/* ── validation helpers ─────────────────────────────────────────────── */

/** Extract an unsigned integer from a JSON number, rejecting NaN, Inf,
 *  negative values, and non-integer representations. */
bool validate_uint(const cJSON *val, const struct runtime_field *f,
		   struct field_value *out);

/** Extract a signed integer from a JSON number, rejecting NaN, Inf,
 *  and non-integer representations. */
bool validate_sint(const cJSON *val, const struct runtime_field *f,
		   struct field_value *out);

/** Extract a double from a JSON number (allows fractional, rejects NaN/Inf). */
bool validate_double(const cJSON *val, const struct runtime_field *f,
		     struct field_value *out);

/* ── descriptor table ───────────────────────────────────────────────── */

/** Number of entries in the field descriptor table. */
extern const unsigned int runtime_field_count;

/** The field descriptor table (sorted alphabetically by name). */
extern const struct runtime_field runtime_fields[];

/** Find a field descriptor by name.  Returns NULL if not found. */
const struct runtime_field *runtime_field_lookup(const char *name);

/** Read a field's value from bless_conf and add it to a cJSON object. */
void runtime_field_get_json(const struct runtime_field *f, void *bconf,
			    cJSON *obj);

#ifdef __cplusplus
}
#endif

#endif /* __BLESS_RUNTIME_FIELD_H__ */
