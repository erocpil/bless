#ifndef BLESS_WS_FRAME_H
#define BLESS_WS_FRAME_H

#include <stddef.h>

enum bless_ws_frame_result {
	BLESS_WS_FRAME_OK = 0,
	BLESS_WS_FRAME_EMPTY,
	BLESS_WS_FRAME_NON_TEXT,
	BLESS_WS_FRAME_FRAGMENTED,
	BLESS_WS_FRAME_TOO_LARGE,
	BLESS_WS_FRAME_EMBEDDED_NUL,
};

enum bless_ws_frame_result bless_ws_frame_validate(
	int bits, const char *data, size_t size, size_t max_size);
const char *bless_ws_frame_error(enum bless_ws_frame_result result);

#endif
