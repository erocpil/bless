#include "ws_frame.h"

#include <string.h>

#define WS_FIN_BIT 0x80
#define WS_OPCODE_MASK 0x0f
#define WS_OPCODE_TEXT 0x01

enum bless_ws_frame_result bless_ws_frame_validate(
	int bits, const char *data, size_t size, size_t max_size)
{
	if (!data || size == 0) {
		return BLESS_WS_FRAME_EMPTY;
	}
	if ((bits & WS_OPCODE_MASK) != WS_OPCODE_TEXT) {
		return BLESS_WS_FRAME_NON_TEXT;
	}
	if ((bits & WS_FIN_BIT) == 0) {
		return BLESS_WS_FRAME_FRAGMENTED;
	}
	if (size > max_size) {
		return BLESS_WS_FRAME_TOO_LARGE;
	}
	if (memchr(data, '\0', size)) {
		return BLESS_WS_FRAME_EMBEDDED_NUL;
	}
	return BLESS_WS_FRAME_OK;
}

const char *bless_ws_frame_error(enum bless_ws_frame_result result)
{
	switch (result) {
	case BLESS_WS_FRAME_EMPTY:
		return "empty WebSocket frame";
	case BLESS_WS_FRAME_NON_TEXT:
		return "text WebSocket frame required";
	case BLESS_WS_FRAME_FRAGMENTED:
		return "fragmented WebSocket messages are not supported";
	case BLESS_WS_FRAME_TOO_LARGE:
		return "WebSocket command exceeds the size limit";
	case BLESS_WS_FRAME_EMBEDDED_NUL:
		return "WebSocket command contains an embedded NUL";
	case BLESS_WS_FRAME_OK:
		break;
	}
	return "invalid WebSocket frame";
}
