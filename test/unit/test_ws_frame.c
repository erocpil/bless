#include "ws_frame.h"

#include <stdio.h>
#include <string.h>

#define CHECK(expr) do {						\
	if (!(expr)) {							\
		fprintf(stderr, "FAIL:%d: %s\n", __LINE__, #expr);	\
		return 1;						\
	}								\
} while (0)

int main(void)
{
	const char valid[] = "{\"cmd\":\"start\"}";
	const char with_nul[] = {'{', '}', '\0', 'x'};

	CHECK(bless_ws_frame_validate(0x81, valid, strlen(valid), 4096)
	      == BLESS_WS_FRAME_OK);
	CHECK(bless_ws_frame_validate(0x01, valid, strlen(valid), 4096)
	      == BLESS_WS_FRAME_FRAGMENTED);
	CHECK(bless_ws_frame_validate(0x80, valid, strlen(valid), 4096)
	      == BLESS_WS_FRAME_NON_TEXT);
	CHECK(bless_ws_frame_validate(0x82, valid, strlen(valid), 4096)
	      == BLESS_WS_FRAME_NON_TEXT);
	CHECK(bless_ws_frame_validate(0x81, NULL, 1, 4096)
	      == BLESS_WS_FRAME_EMPTY);
	CHECK(bless_ws_frame_validate(0x81, valid, 0, 4096)
	      == BLESS_WS_FRAME_EMPTY);
	CHECK(bless_ws_frame_validate(0x81, valid, strlen(valid), 4)
	      == BLESS_WS_FRAME_TOO_LARGE);
	CHECK(bless_ws_frame_validate(0x81, with_nul, sizeof(with_nul), 4096)
	      == BLESS_WS_FRAME_EMBEDDED_NUL);

	puts("WebSocket frame validation: 8 passed, 0 failed");
	return 0;
}
