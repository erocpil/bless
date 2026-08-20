#include "stats_payload.h"

#include <string.h>

size_t stats_payload_copy(char *dst, size_t dst_size,
	const char *src, size_t src_len)
{
	if (!dst || dst_size == 0) {
		return 0;
	}
	if (!src) {
		src_len = 0;
	}
	if (src_len >= dst_size) {
		src_len = dst_size - 1;
	}
	if (src_len) {
		memcpy(dst, src, src_len);
	}
	dst[src_len] = '\0';
	return src_len;
}
