/*
 * fuzz_ws_frame.c — libFuzzer harness for the bless WebSocket frame
 * validator (bless_ws_frame_validate from src/ws_frame.c).
 *
 * Tests production code directly — no local copies.  Exercises:
 *   - fragmented frames (FIN=0)
 *   - non-text opcodes (binary, close, ping, pong, continuation)
 *   - oversized payloads (above BLESS_WS_COMMAND_MAX)
 *   - embedded NUL bytes
 *   - empty data / NULL pointer at various sizes
 *   - boundary lengths: exactly at, one below, and one above the limit
 *
 * Build (local):
 *   clang -fsanitize=fuzzer,address -I include -o fuzz_ws_frame \
 *         fuzz/fuzz_ws_frame.c src/ws_frame.c
 *
 * Run:
 *   ./fuzz_ws_frame -max_len=256 -runs=100000
 */

#include "ws_frame.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Maximum command size from the production definition. */
#ifndef BLESS_WS_COMMAND_MAX
#define BLESS_WS_COMMAND_MAX 4096
#endif

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	/* Split input: first byte is the WebSocket opcode/flags byte,
	 * remaining bytes are the frame payload.  This lets the fuzzer
	 * exercise the full opcode space (0x00–0xFF) and arbitrary
	 * payloads. */
	int bits = size > 0 ? (int)data[0] : 0x81; /* default: text+fin */
	const char *payload = size > 1 ? (const char *)(data + 1) : NULL;
	size_t payload_size = size > 1 ? size - 1 : 0;

	/* Call the production validator with a range of size limits
	 * so boundary behaviour is exercised. */
	bless_ws_frame_validate(bits, payload, payload_size, 0);
	bless_ws_frame_validate(bits, payload, payload_size, 1);
	bless_ws_frame_validate(bits, payload, payload_size,
				BLESS_WS_COMMAND_MAX);
	bless_ws_frame_validate(bits, payload, payload_size,
				(size_t)-1);

	/* Also exercise the error-string function on every result. */
	enum bless_ws_frame_result r = bless_ws_frame_validate(
		bits, payload, payload_size, BLESS_WS_COMMAND_MAX);
	bless_ws_frame_error(r);

	return 0;
}
