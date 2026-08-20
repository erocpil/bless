# Docker Usage Guide

Bless provides a pre-built Docker image (`ghcr.io/erocpil/bless:latest`) and
a `Dockerfile` for custom builds.

---

## Smoke test (no physical NIC)

For a source checkout, the shortest local run is:

```bash
make demo
# Open http://127.0.0.1:8000/entropy
```

This builds Bless and runs `conf/config-ci.yaml` with DPDK's `net_null`
device. It needs no huge pages, PCI device binding, or physical NIC. Packets
are constructed and measured, then discarded by the virtual device.

The equivalent container run is:

```bash
docker pull ghcr.io/erocpil/bless:latest
docker run --rm ghcr.io/erocpil/bless:latest version
docker run --rm -p 8000:8000 ghcr.io/erocpil/bless:latest
# Open http://localhost:8000/
```

Both commands use `net_null`; neither emits traffic on a host interface.

---

## Build from source

```bash
git clone --recursive https://github.com/erocpil/bless.git
cd bless
docker build -t bless .
docker run --rm -p 8000:8000 bless
```

---

## Physical NICs

### Prerequisite

```bash
echo 4096 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
```

### Mellanox ConnectX-4/5/6 (mlx5 driver)

No driver unbind needed.  DPDK communicates with `mlx5_core` via bifurcated
driver mode.

```bash
docker run --rm --privileged \
  -v /dev/hugepages:/dev/hugepages \
  -v $(pwd)/config-mlx5.yaml:/opt/bless/conf/config-mlx5.yaml \
  ghcr.io/erocpil/bless:latest conf/config-mlx5.yaml
```

Sample config (`config-mlx5.yaml`):

```yaml
dpdk:
  a: "0000:81:00.0"               # PCI address (lspci | grep Mellanox)
  main-lcore: 0; l: 0-3; n: 4

injector:
  p: 0x1; auto-start: true; batch: 64
  mode: tx-only; num: -1

bless:
  hw-offload: [ ipv4, udp, tcp ]
  ether:
    dst: 02:c0:ff:ee:00:01
    type:
      ipv4:
        src: 10.0.0.1+1024
        dst: 10.0.0.2+8
        tcp: { src: 10000+100, dst: 80+8 }
        udp: { src: 20000+100, dst: [ 53, 123, 443 ] }
```

### Intel X710 / E810 (i40e / ice driver)

Requires binding the NIC to vfio-pci:

```bash
modprobe vfio-pci
dpdk-devbind.py -b vfio-pci 0000:81:00.0

docker run --rm --privileged \
  -v /dev/hugepages:/dev/hugepages \
  -v /dev/vfio:/dev/vfio \
  -v $(pwd)/config-intel.yaml:/opt/bless/conf/config-intel.yaml \
  ghcr.io/erocpil/bless:latest conf/config-intel.yaml
```

---

## Dual-instance handshake test

`docker-compose.yml` runs TCP handshake stress between two containers:

```bash
docker compose up --build
docker compose logs -f bless-left
docker compose down
```

Left sends TCP SYN, right responds.  Requires `--cap-add=NET_ADMIN` (injected
by compose).

### Verification

```bash
# Single container (with port mapping)
curl -s http://localhost:8000/metrics | grep bless_entropy

# docker-compose (no port mapping -- check from inside)
docker compose exec bless-left /opt/bless/bin/bless version
docker compose logs bless-left | grep "PRNG seed"
```
