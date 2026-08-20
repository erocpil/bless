# Fuzz Corpus Management

## Directory layout

```
fuzz/corpus/
  ws_frame/       seeds for fuzz_ws_frame
  (per-target directories as targets are added)
```

## Adding seeds

1. Create `fuzz/corpus/<target>/` directory.
2. Add minimal binary seeds — one file per interesting input class.
3. Keep seeds small (< 10 KB each; ideally 1–100 bytes).
4. Run the fuzz target once to confirm it loads the seeds:

   ```
   ./fuzz_ws_frame fuzz/corpus/ws_frame/ -max_total_time=5
   ```

## Seed design principles

Each seed should exercise a distinct code path:

| Category | Example seed | What it tests |
|----------|-------------|---------------|
| Valid input | `valid_text.bin` | Happy path, no rejection |
| Fragmented | `fragmented.bin` | FIN=0 → FRAGMENTED |
| Binary opcode | `binary_opcode.bin` | 0x82 → NON_TEXT |
| Continuation | `continuation.bin` | 0x80 → NON_TEXT |
| Embedded NUL | `embedded_nul.bin` | memchr → EMBEDDED_NUL |
| Oversized | `oversized.bin` | size > max_size → TOO_LARGE |
| Empty payload | `empty_payload.bin` | size=0 → EMPTY |
| One-byte | `one_byte.bin` | size=1 boundary |

## Crash triage

When CI or local fuzzing finds a crash:

1. **Save the crashing input** to `fuzz/corpus/<target>/crash-<date>-<hash>.bin`.
2. **Minimise** with `-minimize_crash=1`:

   ```
   ./fuzz_ws_frame crash-input.bin -minimize_crash=1 -runs=100000
   ```

3. **Record** the crash in this document under the target's section.
4. **Fix** the defect.
5. **Add the minimised input as a regression seed** so CI catches regressions.

## CI integration

CI runs each fuzz target for 30 seconds with `-max_total_time=28`.
This is a *smoke gate*, not a coverage campaign — the full OSS-Fuzz
infrastructure handles continuous fuzzing.

When CI encounters a crash (exit code ≠ 0, sanitizer failure), the
`pipefail` shell ensures the step fails and the PR is blocked.

## YAML parser termination regression

The malformed document `0a 0a 7d 0a` (`"\n\n}\n"`) previously left the
parser in an error-state loop.  It is now covered by
`test/unit/test_config_yaml.c`; the production parser returns failure
without reusing libyaml after a nested parse error.  `fuzz_yaml` calls
the same DPDK-independent production parser, so this regression cannot
drift between the fuzz harness and the application.

## Adding a new fuzz target

1. Place the harness in `fuzz/fuzz_<target>.c`.
2. Call production code directly (no local copies).
3. Add seeds in `fuzz/corpus/<target>/`.
4. Add build + run commands to `.github/workflows/ci.yml` (fuzz step).
5. Update the fuzz target count comment.
