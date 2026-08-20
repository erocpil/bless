# Building BLESS

## Quick Start

```bash
# 1. Install system dependencies
apt install build-essential cmake autoconf libtool pkg-config \
            git libdpdk-dev libyaml-dev libcjson-dev libssl-dev   \
            libpcap-dev libnl-3-dev libnuma-dev libelf-dev        \
            libsystemd-dev libdbus-1-dev

# 2. Clone with submodules
git clone --recurse-submodules <repo-url>
cd bless

# 3. Build
make -j$(nproc)

# 4. Verify
./build/release-static/bin/bless version
```

## System Requirements

### Operating System

- Debian 12 / Ubuntu 22.04+ (recommended)
- Kernel with hugepage support (CONFIG_HUGETLBFS)

### Packages (Debian 12 / Ubuntu 22.04)

| Package | Required | Purpose |
|---------|----------|---------|
| `gcc` / `clang` | yes | C11 compiler |
| `build-essential` | yes | `make`, `ld`, headers |
| `cmake` | yes | cJSON third-party build |
| `autoconf` / `libtool` | yes | libyaml third-party build |
| `pkg-config` | yes | DPDK and system lib discovery |
| `git` | yes | version info and third-party submodules |
| `libdpdk-dev` | yes | DPDK (Data Plane Development Kit) |
| `libyaml-dev` | no* | YAML config parsing (optional -- built from submodule) |
| `libcjson-dev` | no* | JSON serialisation (optional -- built from submodule) |
| `libssl-dev` | yes | TLS for civetweb (WebSocket) |
| `libpcap-dev` | yes | PCAP device support |
| `libnl-3-dev` | yes | Netlink PMD support |
| `libnuma-dev` | yes | NUMA memory allocation |
| `libelf-dev` | yes | ELF section parsing (buildinfo) |
| `libsystemd-dev` | yes | systemd journal integration |
| `libdbus-1-dev` | yes | D-Bus support |

`*` -- libyaml and libcjson are also available as third-party submodules if system packages are absent.

### DPDK

BLESS requires DPDK **23.11** (tested with 23.11.3). Two installation paths:

**Option A: System package (recommended)**

```bash
apt install libdpdk-dev
```

**Option B: Custom install at `/opt/dpdk`**

Used when distro packages are outdated or custom PMDs are needed.

```bash
wget https://fast.dpdk.org/rel/dpdk-23.11.tar.xz
tar xf dpdk-23.11.tar.xz && cd dpdk-23.11
meson setup build -Dprefix=/opt/dpdk --default-library=static
ninja -C build && ninja -C build install
```

### Hugepage Setup

DPDK requires hugepages (minimum 2 GiB recommended):

```bash
# 2 MiB hugepages
echo 1024 | tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages

# 1 GiB hugepages (optional, better for large mbuf pools)
echo 2 | tee /sys/kernel/mm/hugepages/hugepages-1048576kB/nr_hugepages

# Mount
mkdir -p /mnt/huge
mount -t hugetlbfs nodev /mnt/huge
```

For persistence, add to `/etc/fstab`:
```
nodev /mnt/huge hugetlbfs defaults 0 0
```

## Build

### Standard Build

```bash
make -j$(nproc)
```

Produces `build/release-static/bin/bless` (statically linked, ~36 MB).

Run `make` from the repository root. `src/Makefile` is an internal sub-make,
not a standalone build entry point. The top-level Makefile prepares the
third-party dependency flags, updates `include/version.h`, selects an absolute
output directory, and then invokes it.

Running a build target directly in `src/` stops with an error that points back
to the repository root. `make clean` remains available there for the top-level
`distclean` target.

### Build Variants

| Variable | Default | Options | Description |
|----------|---------|---------|-------------|
| `BUILD` | `release` | `release`, `debug` | Optimisation level |
| `STATIC` | `1` | `1`, `0` | Static vs shared linking |
| `V` | `0` | `0`, `1` | Verbose make output |
| `PREFIX` | `/opt/bless-1.0` | path | Install prefix |

Examples:

```bash
# Debug build with symbols
make BUILD=debug -j$(nproc)

# Shared library linking
make STATIC=0 -j$(nproc)
```

### Third-Party Libraries

Three libraries are handled as git submodules and built automatically:

| Library | Path | Build Type |
|---------|------|------------|
| libyaml | `third_party/libyaml/upstream` | autotools (bootstrap + configure + make) |
| cJSON | `third_party/cjson/upstream` | CMake |
| civetweb | `third_party/civetweb/upstream` | GNU make |

To fetch or update submodules before building:

```bash
git submodule update --init --recursive
make update-third-party
```

Output archives (`*.a`) land in `lib/`.

### Unit Tests

```bash
make test
```

Runs `test/unit/test_dist` (distribution algorithm). Requires no DPDK.

### Clean

```bash
make clean          # remove build artifacts
make distclean      # also clean third-party library builds
```

## Install

```bash
make install PREFIX=<path> DESTDIR=<path>
```

Installs:
- `<prefix>/bin/bless` -- binary
- `<prefix>/lib/*.a` -- third-party static archives (if shared build)
- `<prefix>/conf/config.yaml.template` -- configuration template
- `<prefix>/include/bless/*.h` -- headers

## Troubleshooting

### `pkg-config: not found`
```bash
apt install pkg-config
```

### `Cannot find libdpdk`
```
apt install libdpdk-dev
# or build DPDK from source and install to /opt/dpdk
export PKG_CONFIG_PATH=/opt/dpdk/lib/x86_64-linux-gnu/pkgconfig:$PKG_CONFIG_PATH
```

### `hugepages not available`
```
cat /proc/meminfo | grep HugePages
# If 0, follow the hugepage setup steps above.
# Running on a VM may require /etc/default/grub hugepagesz=1G hugepages=2
```

### gcc: `Warning: unterminated string; newline inserted` (304 warnings)

Building with `CC=gcc` produces ~300 assembler warnings from the
`elfnote.o` generation step (embedded `.note.buildinfo` ELF section).
clang's integrated assembler handles the same input silently; the GNU
assembler (gas) warns when `.ascii` receives literal newline bytes
(0x0a) inside a string literal.

**Impact**: cosmetic only.  The `readelf -n` output is correct, the
binary links and runs normally.  CI does not fail on warnings (only
on non-zero exit).  Both gcc and clang builds produce functionally
identical binaries.

**Root cause**: `BUILDINFO_DESC` is piped through `printf | cc -x
assembler` to produce `elfnote.o`.  The `\n` escape sequences are
deliberately left as literal newline bytes so gas can embed them in
the ELF note's description field.  gas warns on every occurrence.

**Related**: `buildinfo.c` (build-time timestamp, separate from
`elfnote.o`) is generated as plain C and does not trigger these
warnings.  See `src/Makefile` for the content-guarded generation rules.

### `function `rte_xxx' is not implemented` (shared build)
The binary is linked dynamically. Verify `LD_LIBRARY_PATH` includes DPDK library paths.

### `bless: error while loading shared libraries`
Use `STATIC=1` (default) to avoid runtime library search issues.

### `third_party/libyaml/upstream/.git: NOT FOUND`
```bash
git submodule update --init --recursive
```
