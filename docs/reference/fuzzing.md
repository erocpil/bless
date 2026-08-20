# OSS-Fuzz Integration for BLESS

BLESS participates in [OSS-Fuzz](https://google.github.io/oss-fuzz/),
Google's continuous fuzzing service for open-source projects.
This document describes the integration architecture, fuzz targets,
and submission process.

## Coverage Overview

Six fuzz targets exercise the core attack surfaces of BLESS:

| Target | Source | What It Tests | Value |
|--------|--------|---------------|-------|
| `fuzz_yaml` | YAML config parser | libyaml streaming parse + tree construction | **Highest** -- YAML is untrusted external input |
| `fuzz_dist` | `distribute()` + `make_power_of_2()` | Weighted proportional allocation | Invariant verification (sum == total) |
| `fuzz_ip_range` | `bless_parse_ip_range()` / `bless_parse_port_range()` | `base+N` string parsers | Every config load path |
| `fuzz_http_payload` | `build_http_payload()` + 4 mutations | HTTP/1.1 header construction | Buffer overflow risk (snprintf) |
| `fuzz_dns_encode` | DNS name encoder + `parse_qtype()` | Dot-to-wire format encoding | Label length validation (<=63) |
| `fuzz_bless` | IP/port parsers + checksum + power-of-2 | Legacy combined harness | Broader exercise path |

## Architecture

All fuzz targets are **self-contained C files** that do not depend on DPDK.
They use local implementations of the parsing/encoding logic with identical
semantics to the production code.  This design choice avoids the complexity
of stubbing DPDK headers and runtime (EAL init, hugepages, NIC binding)
for fuzzing purposes.

The YAML target (`fuzz_yaml`) links against `libyaml` -- the same library
BLESS uses in production for config file parsing.  All other targets are
pure C with no external dependencies beyond libc.

## File Layout

```
bless/
  fuzz/
    fuzz_yaml.c          YAML parser harness
    fuzz_dist.c          Distribution engine harness
    fuzz_ip_range.c      IP/port range parser harness
    fuzz_http_payload.c  HTTP payload builder harness
    fuzz_dns_encode.c    DNS name encoder harness
    fuzz_bless.c         Legacy combined harness
    build.sh             OSS-Fuzz build script
    corpus/              (optional) seed corpora
  docs/
    oss-fuzz-integration.md   This file
```

## OSS-Fuzz Submission

### Prerequisites

The BLESS OSS-Fuzz integration requires these files in the
[google/oss-fuzz](https://github.com/google/oss-fuzz) repository:

```
projects/bless/
  Dockerfile       Build environment
  build.sh         Fuzz target compilation
  project.yaml     Project metadata
```

### Dockerfile

```dockerfile
FROM gcr.io/oss-fuzz-base/base-builder
RUN apt-get update && apt-get install -y libyaml-dev
COPY . $SRC/bless
COPY build.sh $SRC/
WORKDIR $SRC/bless
```

### project.yaml

```yaml
homepage: "https://github.com/<org>/bless"
language: c
primary_contact: "<email>"
auto_ccs:
  - "<email>"
fuzzing_engines:
  - libfuzzer
  - afl
  - honggfuzz
sanitizers:
  - address
  - undefined
architectures:
  - x86_64
```

### Submission Process

1. Fork [google/oss-fuzz](https://github.com/google/oss-fuzz)
2. Create `projects/bless/` with the three files above
3. Verify locally:
   ```bash
   python infra/helper.py build_fuzzers bless
   python infra/helper.py check_build bless
   ```
4. Submit a PR to google/oss-fuzz

After merge, OSS-Fuzz will automatically:
- Build all fuzz targets on each commit to the BLESS repository
- Run continuous fuzzing with multiple engines (libFuzzer, AFL, Honggfuzz)
- Report crashes to the listed contacts
- Publish coverage reports

## Local Fuzzing

### Prerequisites

```bash
# Debian/Ubuntu
apt install clang libyaml-dev
```

### Build All Targets

```bash
cd fuzz
for t in fuzz_yaml fuzz_dist fuzz_ip_range fuzz_http_payload fuzz_dns_encode fuzz_bless; do
    case $t in
        fuzz_yaml)
            clang -fsanitize=fuzzer,address -g -O1 \
                -I ../include -o $t ${t}.c -lyaml
            ;;
        fuzz_dist)
            clang -fsanitize=fuzzer,address -g -O1 \
                -I ../src -I ../include -o $t ${t}.c ../src/dist.c
            ;;
        fuzz_bless)
            clang -fsanitize=fuzzer,address -g -O1 \
                -I ../src -I ../include -o $t ${t}.c ../src/dist.c -lm
            ;;
        *)
            clang -fsanitize=fuzzer,address -g -O1 \
                -o $t ${t}.c
            ;;
    esac
    echo "Built: $t"
done
```

### Quick Smoke Test

```bash
for t in fuzz_*; do
    echo "=== $t ==="
    ./$t -max_len=64 -runs=1000 2>&1 | tail -1
done
```

Expected: all targets exit with status 0 (no crashes, no sanitizer violations).

### Run Single Target

```bash
# YAML fuzzer (highest value)
./fuzz_yaml -max_len=4096 -runs=50000

# With a specific corpus
mkdir -p corpus_yaml
echo "key: value" > corpus_yaml/seed1.yaml
echo "- item1\n- item2" > corpus_yaml/seed2.yaml
./fuzz_yaml corpus_yaml/ -max_len=4096
```

### Reproduce a Crash

```bash
./fuzz_yaml crash-xxxxx
```

The fuzzer will replay the exact input that triggered the crash,
with full ASan/UBSan diagnostics.

## Fuzz Target Details

### fuzz_yaml -- YAML Config Parser

The highest-value target.  Feeds arbitrary bytes through the production
libyaml-backed tree parser used by BLESS in
`config_init() -> config_yaml_parse_memory() -> parse_mapping()` in the
production DPDK-independent YAML module.

Attack surface:
- Malformed YAML (truncated, invalid UTF-8, deeply nested)
- Tree construction (exhaustion, deep recursion)
- Memory leaks in error paths

The target and its production parser are built from the repository-pinned
libyaml submodule by both CI and the OSS-Fuzz build script.

The local implementation mirrors `config.c`'s logic: mapping keys
-> value pairs, sequence items, nested structures, multi-document
streams.  All allocations are tracked and freed on exit.

### fuzz_dist -- Distribution Engine

Links against the real `src/dist.c` (no DPDK, pure C).  Exercises:

- `distribute()` -- weighted proportional allocation
- `make_power_of_2()` -- ceiling to next power of two

Invariant check: after `distribute()`, `sum(result) == total`
and `result[i] >= 1` for all categories.  These assertions will
abort on violation, triggering a fuzzer crash.

### fuzz_ip_range -- IP/Port Parsers

Exercises `bless_parse_ip_range()` and `bless_parse_port_range()`
with arbitrary byte sequences.  These parsers are the first code
path hit when loading a YAML config file -- every IP address and
port number passes through them.

Edge cases explicitly tested:
- Empty string
- Bare `+` (no base or range)
- Integer overflow on range
- Non-numeric garbage

### fuzz_http_payload -- HTTP Request Builder

Exercises `build_http_payload()` from `proto_http.c` with random
method, path, and host strings.  The `snprintf` format string is
a classic buffer overflow risk -- the harness verifies the output
never exceeds the buffer.

Also exercises all four HTTP mutation functions:
1. `malformed` -- HTTP/9.9 version replacement
2. `method_invalid` -- garbage method injection
3. `host_overflow` -- (placeholder; active on real mbufs only)
4. `crlf_missing` -- LF-only line endings

### fuzz_dns_encode -- DNS Wire Encoder

Exercises the DNS dot-string to wire-format encoder from
`proto_dns.c`.  The encoder must validate:

- Label length <= 63 bytes (per RFC 1035)
- Total name length <= 255 bytes
- Null-terminated output

Also exercises `parse_qtype()` with both name-based (A, NS, MX, ...)
and numeric (1-65535) inputs.

### fuzz_bless -- Legacy Combined Harness

The original fuzz target, kept for backward compatibility.  Tests
IP/port parsers, one's complement checksum, and `make_power_of_2()`
in a single harness.  Links against `src/dist.c` for the real
implementation.

## Security Considerations

- All fuzz targets run with AddressSanitizer and UndefinedBehaviorSanitizer
- OSS-Fuzz uses multiple engines (libFuzzer, AFL, Honggfuzz) for diversity
- Crashes are reported within 24 hours to project contacts
- Coverage reports are published at https://oss-fuzz.com

## References

- [OSS-Fuzz Documentation](https://google.github.io/oss-fuzz/)
- [OSS-Fuzz New Project Guide](https://google.github.io/oss-fuzz/getting-started/new-project-guide/)
- [libFuzzer Tutorial](https://github.com/google/fuzzing/blob/master/tutorial/libFuzzerTutorial.md)
- [AddressSanitizer](https://clang.llvm.org/docs/AddressSanitizer.html)
