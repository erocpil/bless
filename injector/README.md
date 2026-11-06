# Injector

Injector is a standalone TX-only application and the first consumer of the
repository's minimal DPDK TX runtime. It remains an independent build target;
Bless continues to use its existing runtime path in this increment.

The runtime API and lifecycle/mbuf ownership boundaries are documented in
[`docs/design/dpdk-tx-runtime.md`](../docs/design/dpdk-tx-runtime.md). The
implementation is a separate static archive under `runtime/dpdk/`.

This directory was imported from the available `../injector` source snapshot.
That source directory had no Git metadata, so the original commit history could
not be preserved. The import includes its current source changes and excludes
build outputs and generated PCAP files.

Its packet description uses the same top-level `bless:` section as the main
Bless configuration schema. The application remains TX-only and consumes only
the options it implements.

Build from the repository root:

```sh
make injector
```

The standalone PCAP smoke-test configuration is `conf/config-pcap.yaml`.
It writes `tx.pcap` in the process working directory; run it from a scratch
directory if you do not want to overwrite an existing capture:

```sh
cd /tmp
/path/to/bless/injector/build/release-static/bin/injector \
  /path/to/bless/injector/conf/config-pcap.yaml
```
