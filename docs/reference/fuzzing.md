# Fuzzing and OSS-Fuzz Build Support

The repository provides seven DPDK-independent libFuzzer targets, a bounded CI
gate and an OSS-Fuzz-compatible build script.
These files establish local build support, not enrollment in the external
OSS-Fuzz service. Confirm external acceptance separately before claiming it.

## Coverage Overview

| Target | Linked production code | Coverage boundary |
|---|---|---|
| `fuzz_yaml` | `src/config_yaml.c`, pinned libyaml sources | YAML bytes to `Node` tree and cleanup, not all Bless semantic configuration |
| `fuzz_dist` | `src/dist.c` | Distribution arithmetic and power-of-two helper |
| `fuzz_ip_range` | `src/parse.c` | IP, port and IPv6 range parsing |
| `fuzz_ws_frame` | `src/ws_frame.c` | Frame flags, lengths and payload validation, not JSON command execution |
| `fuzz_http_payload` | None | Local HTTP payload/mutation helpers |
| `fuzz_dns_encode` | None | Local DNS encoding and query-type helpers |
| `fuzz_bless` | `src/dist.c` | Production power-of-two helper; checksum and range parsers are local copies |

A passing copied-helper test does not prove that the corresponding production
mbuf constructor is safe.
`fuzz/build.sh` is the authoritative source/link list; it compiles the pinned
libyaml sources with instrumentation as well as the production YAML module.
The YAML harness calls `config_yaml_parse_memory()` directly; it does not need
DPDK initialization, hugepages or a NIC.

## Local Fuzzing

Run from the repository root on a host with a Clang installation that includes
libFuzzer, ASan and UBSan, plus `zip`.
Initialize the repository submodules before building.
The script needs both C and C++ compiler drivers because the fuzz engine links
against the C++ runtime.

```bash
git submodule update --init --recursive
mkdir -p /tmp/bless-fuzz-out /tmp/bless-fuzz-work
CC=clang CXX=clang++ \
  CFLAGS="-fno-omit-frame-pointer" CXXFLAGS="-fno-omit-frame-pointer" \
  LIB_FUZZING_ENGINE="-fsanitize=fuzzer" \
  OUT=/tmp/bless-fuzz-out WORK=/tmp/bless-fuzz-work \
  ./fuzz/build.sh
```

For a quick bounded run of all targets:

```bash
set -e
for target in fuzz_yaml fuzz_dist fuzz_ip_range fuzz_http_payload \
              fuzz_dns_encode fuzz_bless fuzz_ws_frame; do
  /tmp/bless-fuzz-out/"$target" -max_total_time=10 -max_len=5001
done
```

Use the committed WS corpus in a writable temporary directory when testing the
4096-byte command boundary; the harness consumes one byte as flags:

```bash
mkdir -p /tmp/bless-ws-corpus
cp fuzz/corpus/ws_frame/* /tmp/bless-ws-corpus/
/tmp/bless-fuzz-out/fuzz_ws_frame /tmp/bless-ws-corpus \
  -max_len=5001 -max_total_time=30
```

Set `FUZZ_TARGETS=fuzz_yaml` on the build command to build only that target.
To replay a saved failure:

```bash
/tmp/bless-fuzz-out/fuzz_yaml /path/to/crash-input
```

There is no top-level `make fuzz` target at this revision.
Avoid globbing `fuzz_*` as executables in the source directory: that also matches
C source files and generated corpus archives.

## CI gate

The GCC matrix job in [CI](../../.github/workflows/ci.yml) builds and runs all
seven targets with libFuzzer, ASan and UBSan.
Each target has a 30-second process budget with `-max_total_time=28`.
The YAML target reuses `fuzz/build.sh`; other targets use explicit production
source lists in the workflow.
The shell uses `pipefail`, so a fuzzer failure is not hidden by the output tail.
A bounded run is a regression check, not exhaustive input coverage.

## OSS-Fuzz submission checklist

A submission still needs project metadata, maintainer contacts, a source-fetching
Dockerfile and a wrapper that enters the repository root before invoking
`fuzz/build.sh`.
The checkout must include `third_party/libyaml/upstream`.
The build script expects `CC`, `CXX`, `CFLAGS`, `CXXFLAGS`,
`LIB_FUZZING_ENGINE`, `OUT` and `WORK` from its build environment.
It also packages the WS seed corpus for the external build convention.

Validate the proposed project in the external infrastructure before submission;
record an accepted upstream change and actual service results before changing
these docs to say continuous external fuzzing is active.

See [Security](../guides/security.md) and
[Testing Coverage Boundaries](testing-coverage.md) for sanitizer and integration
coverage beyond these harnesses.
