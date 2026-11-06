#ifndef __PREFLIGHT_H__
#define __PREFLIGHT_H__

struct base;

enum preflight_mode {
	PREFLIGHT_OFF = 0,
	PREFLIGHT_WARN = 1,
	PREFLIGHT_STRICT = 2,
};

/* Inspect without changing the host. Returns -1 only when strict policy finds
 * a condition that makes benchmark placement materially unreliable. */
int preflight_run(const struct base *base, enum preflight_mode mode,
		  const char *snapshot_path);

#endif
