#ifndef BLESS_STATS_PAYLOAD_H
#define BLESS_STATS_PAYLOAD_H

#include <stddef.h>

/*
 * Copy a published payload to a private, NUL-terminated buffer.
 * The returned length is always less than dst_size.
 */
size_t stats_payload_copy(char *dst, size_t dst_size,
	const char *src, size_t src_len);

#endif
