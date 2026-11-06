# Legacy documentation reconciliation

This reconciliation used the 59 Markdown exports in `~/Downloads/bless` and the
repository at `4d5d538` as its implementation baseline.
It is a documentation review, not a new benchmark or a claim that remote CI passed.
The exports contain root-level and categorized revisions plus Chinese translations;
file count is not a count of distinct topics.
No maintained document depends on the Downloads directory being available.

## Material absorbed

| Legacy topic | Maintained destination | Treatment |
|---|---|---|
| Chinese architecture, controllable entropy injection, directory layout | [Chinese overview](../zh-CN/overview.md) | Consolidated three-axis model, data flow and navigation |
| Design Decisions and Tradeoffs / 设计决策与权衡 | [Architecture](../overview.md), [Chinese design rationale](../zh-CN/design/decisions.md) | Retained rationale for off-path statistics, PRNG state, feedback, interleave and layered lengths; corrected unsupported guarantees |
| Chinese configuration, runtime, Docker, error handling and observability | [Chinese runtime guide](../zh-CN/guides/runtime.md) | Preserved workflow; current references own exact parameters and commands |
| Chinese entropy theory, min-entropy and detailed MI guide | [Scenario guide](../zh-CN/guides/scenarios.md), [Min-Entropy](../concepts/min-entropy.md) | Preserved examples for conntrack, DPI, VXLAN and timing; removed universal bit thresholds |
| Industry Showcase Roadmap | [Chinese evidence guidance](../zh-CN/overview.md#目录与证据) | Retained reproducibility/deployment/validation intent; did not import stale completion percentages |
| API, concurrency, security, fuzzing and optimization status | Current guides/reference | Checked against descriptors, source lists and workflow commands |
| Protocol, build, parsing and benchmark exports | Existing topic documents | Already represented; no duplicate copies or new historical performance claims |

## Corrections established by local inspection

- `runtime_field.c` owns runtime mutability and WS numeric ranges; these are not
  automatically the CLI/YAML validation rules.
- `worker_func_tx_only()` uses per-worker packet/byte budgets; `bps_rate` counts
  DPDK bytes, and `bless_alloc_mbufs()` returns `n`, not zero, on success.
- `adapt_pps_rate()` increments PPS when entropy is below target and truncates
  the increment; zero PPS means unlimited. Old slow-down/stability claims were removed.
- `random_delay_jitter()` uses signed-in-intent offsets in unsigned arithmetic;
  the runtime guide describes its actual constraint rather than a clamped result.
- `control_policy.h` defines a 16–255-byte key range; `server.c` separates WS
  control from HTTP observation routes.
- `fuzz/build.sh` and CI cover seven targets with different production/copy
  boundaries; neither local build support nor an old checklist proves OSS-Fuzz enrollment.
- The sanitized production CI command runs `--version`, not packet I/O.
- Entropy math, timing, PSD, sampling and config helper tests exist; historical
  assertion counts and helper copies do not establish complete production coverage.
- Hash-based sampling replaces an exact every-Nth-packet description.
- The entropy/MI guides now distinguish population assumptions, finite windows,
  inactive populations and configured versus observed-support baselines.

## Boundaries retained

No measured benchmark numbers were regenerated or edited in this reconciliation.
Historical assessments remain revision-specific records.
Hardware validation, full live-process failure injection and external service
status require their own evidence.
The separate root and `.github/` security policies still need an ownership-level
reconciliation; the user guide links to `.github/SECURITY.md` rather than inventing
new response deadlines or disclosure rules.
