# QEMU Development Environment

This guide creates the QEMU/KVM environment used to develop Bless.
Its second network path supports an independent Virtio and vhost-net
experiment alongside hugepage-backed NUMA memory and VFIO devices.

The VM uses two independent Virtio network paths:

```text
                              Guest
                    +-----------+-----------+
                    |                       |
             virtio-net NIC 1        virtio-net NIC 2
                    |                       |
             QEMU user network          virtqueue
                    |                       |
               SLIRP / NAT              vhost-net
                    |                       |
                Internet                  tap0
                                            |
                                           Host
```

The first NIC retains the existing Internet and port-forwarding path.
The second NIC provides a dedicated Host to Guest path for Virtio and
vhost-net experiments.
Do not add a default route through the second NIC.

## 1. Environment assumptions

The commands below use the following values:

| Item | Value |
|------|-------|
| VM name | `dpdk-vm1` |
| Disk image | `1.img` in the launch directory |
| QEMU binary | `/usr/libexec/qemu-kvm` |
| Host TAP interface | `tap0` |
| Host experiment address | `192.168.100.1/24` |
| Guest experiment address | `192.168.100.2/24` |
| SSH forwarding | Host TCP 10022 to Guest TCP 22 |
| Bless UI forwarding | Host TCP 8000 to Guest TCP 8000 |
| Hugepage mount | `/mnt/huge-1G` |
| Guest resources | 40 vCPUs and 40 GiB RAM |

Run the Host commands as root or through `sudo`.
The launch account needs access to KVM, VFIO, `/dev/vhost-net`, the TAP
interface, the disk image, and the hugepage mount.

Check the required files and devices before starting QEMU:

```bash
test -x /usr/libexec/qemu-kvm
test -f 1.img
test -c /dev/kvm
```

The QEMU binary path is distribution-specific.
Use `command -v qemu-system-x86_64` or `command -v qemu-kvm` when
`/usr/libexec/qemu-kvm` is not present.

## 2. Prepare Host hugepages and VFIO

The two memory backends allocate 20 GiB each from a 1 GiB hugepage mount.
The Host therefore needs at least 40 free 1 GiB hugepages at VM startup.

The Host startup script in section 4 loads `tun`, `vhost_net`, `vfio`, and
`vfio_pci`, creates the mount when needed, and verifies the free-page count.
Inspect the current allocation and mount independently when diagnosing a
startup failure:

```bash
cat /sys/kernel/mm/hugepages/hugepages-1048576kB/nr_hugepages
cat /sys/kernel/mm/hugepages/hugepages-1048576kB/free_hugepages
findmnt /mnt/huge-1G
```

A typical hugetlbfs mount uses:

```bash
mkdir -p /mnt/huge-1G
mount -t hugetlbfs -o pagesize=1G nodev /mnt/huge-1G
```

Hugepage reservation is platform-specific and may require kernel command-line
configuration when contiguous 1 GiB pages cannot be allocated at runtime.
Do not start the VM until the required pages are free.

The launch command passes these Host PCI functions through to the Guest:

```text
af:00.0
af:04.2
af:04.3
af:04.4
af:04.5
af:04.6
af:04.7
```

Confirm that each function exists, belongs to an isolated IOMMU group, and is
bound to `vfio-pci` before launch:

```bash
lspci -nn -s af:00.0
lspci -nnk -s af:00.0
readlink /sys/bus/pci/devices/0000:af:00.0/iommu_group
```

Repeat those checks for every listed function.
Binding a production interface to `vfio-pci` disconnects it from its Host
driver, so perform that operation only on devices reserved for the VM.

VFIO gives the Guest direct device access while the IOMMU restricts DMA to the
Host pages mapped for that Guest:

```text
Guest DMA address
        |
        v
IOMMU translation
        |
        v
permitted Host physical pages
```

## 3. Install the Host TAP scripts

QEMU can create `tap0` and invoke scripts to configure and release it.
This replaces the manual `ip tuntap add`, address assignment, and link setup
used by the initial procedure.

Install `/usr/local/libexec/bless-qemu-ifup`:

```bash
#!/usr/bin/env bash
set -euo pipefail

tap_name=${1:?QEMU did not provide a TAP interface name}

if [[ ${tap_name} != tap0 ]]; then
  echo "refusing to configure unexpected TAP interface: ${tap_name}" >&2
  exit 1
fi

ip -4 addr flush dev "${tap_name}"
ip addr add 192.168.100.1/24 dev "${tap_name}"
ip link set "${tap_name}" up
```

Install `/usr/local/libexec/bless-qemu-ifdown`:

```bash
#!/usr/bin/env bash
set -u

tap_name=${1:?QEMU did not provide a TAP interface name}

if [[ ${tap_name} != tap0 ]]; then
  echo "refusing to release unexpected TAP interface: ${tap_name}" >&2
  exit 1
fi

ip -4 addr flush dev "${tap_name}" 2>/dev/null || true
ip link set "${tap_name}" down 2>/dev/null || true
```

Make both scripts executable:

```bash
chmod 0755 /usr/local/libexec/bless-qemu-ifup
chmod 0755 /usr/local/libexec/bless-qemu-ifdown
```

QEMU passes the TAP interface name as the first script argument.
The scripts reject any name other than `tap0` so that a configuration error
cannot flush an unrelated Host interface.

## 4. Install and run the Host startup script

Save the following as `/usr/local/sbin/start-dpdk-vm1`:

```bash
#!/usr/bin/env bash
set -euo pipefail

QEMU_BIN=${QEMU_BIN:-/usr/libexec/qemu-kvm}
DISK_IMAGE=${DISK_IMAGE:-${PWD}/1.img}
HUGEPAGE_DIR=${HUGEPAGE_DIR:-/mnt/huge-1G}
LOG_FILE=${LOG_FILE:-${PWD}/qemu-dpdk-vm1.log}
PID_FILE=${PID_FILE:-${PWD}/qemu-dpdk-vm1.pid}
IFUP_SCRIPT=/usr/local/libexec/bless-qemu-ifup
IFDOWN_SCRIPT=/usr/local/libexec/bless-qemu-ifdown
VFIO_STATE_FILE=/run/dpdk-vm1-vfio-drivers
REQUIRED_1G_PAGES=40
EXPERIMENT_MAC=52:54:00:64:00:02

VFIO_DEVICES=(
  0000:af:00.0
  0000:af:04.2
  0000:af:04.3
  0000:af:04.4
  0000:af:04.5
  0000:af:04.6
  0000:af:04.7
)

bind_vfio=0
if [[ ${1:-} == --bind-vfio ]]; then
  bind_vfio=1
elif [[ $# -ne 0 ]]; then
  echo "usage: $0 [--bind-vfio]" >&2
  exit 2
fi

if [[ ${EUID} -ne 0 ]]; then
  echo "run this script as root" >&2
  exit 1
fi

for command in basename findmnt grep modprobe mountpoint mount readlink pgrep; do
  command -v "${command}" >/dev/null || {
    echo "missing required command: ${command}" >&2
    exit 1
  }
done

[[ -x ${QEMU_BIN} ]] || {
  echo "QEMU executable not found: ${QEMU_BIN}" >&2
  exit 1
}
[[ -f ${DISK_IMAGE} ]] || {
  echo "disk image not found: ${DISK_IMAGE}" >&2
  exit 1
}
[[ -x ${IFUP_SCRIPT} && -x ${IFDOWN_SCRIPT} ]] || {
  echo "install the QEMU TAP scripts before starting the VM" >&2
  exit 1
}
[[ ! -e ${PID_FILE} ]] || {
  old_pid=$(cat "${PID_FILE}" 2>/dev/null || true)
  if [[ -n ${old_pid} ]] && kill -0 "${old_pid}" 2>/dev/null; then
    echo "dpdk-vm1 is already running with PID ${old_pid}" >&2
    exit 1
  fi
  rm -f "${PID_FILE}"
}

modprobe tun
modprobe vhost_net
modprobe vfio
modprobe vfio_pci

for device_node in /dev/kvm /dev/net/tun /dev/vhost-net; do
  [[ -e ${device_node} ]] || {
    echo "required device node is unavailable: ${device_node}" >&2
    exit 1
  }
done

mkdir -p "${HUGEPAGE_DIR}"
if ! mountpoint -q "${HUGEPAGE_DIR}"; then
  mount -t hugetlbfs -o pagesize=1G nodev "${HUGEPAGE_DIR}"
fi
if [[ $(findmnt -n -o FSTYPE --target "${HUGEPAGE_DIR}") != hugetlbfs ]]; then
  echo "${HUGEPAGE_DIR} is mounted, but it is not hugetlbfs" >&2
  exit 1
fi
if ! findmnt -n -o OPTIONS --target "${HUGEPAGE_DIR}" | \
     grep -Eq '(^|,)pagesize=(1G|1024M)(,|$)'; then
  echo "${HUGEPAGE_DIR} is not configured for 1 GiB hugepages" >&2
  exit 1
fi

hugepage_free_file=/sys/kernel/mm/hugepages/hugepages-1048576kB/free_hugepages
[[ -r ${hugepage_free_file} ]] || {
  echo "the Host kernel does not expose a 1 GiB hugepage pool" >&2
  exit 1
}
free_1g_pages=$(<"${hugepage_free_file}")
if (( free_1g_pages < REQUIRED_1G_PAGES )); then
  echo "need ${REQUIRED_1G_PAGES} free 1 GiB hugepages; found ${free_1g_pages}" >&2
  exit 1
fi

if (( bind_vfio )); then
  : > "${VFIO_STATE_FILE}"
fi

for bdf in "${VFIO_DEVICES[@]}"; do
  device_path=/sys/bus/pci/devices/${bdf}
  [[ -e ${device_path} ]] || {
    echo "PCI function does not exist: ${bdf}" >&2
    exit 1
  }
  [[ -L ${device_path}/iommu_group ]] || {
    echo "PCI function has no IOMMU group: ${bdf}" >&2
    exit 1
  }

  current_driver=none
  if [[ -L ${device_path}/driver ]]; then
    current_driver=$(basename "$(readlink -f "${device_path}/driver")")
  fi

  if [[ ${current_driver} == vfio-pci ]]; then
    continue
  fi
  if (( ! bind_vfio )); then
    echo "${bdf} uses ${current_driver}; rerun with --bind-vfio after confirming the device is reserved for this VM" >&2
    exit 1
  fi

  printf '%s %s\n' "${bdf}" "${current_driver}" >> "${VFIO_STATE_FILE}"
  printf '%s\n' vfio-pci > "${device_path}/driver_override"
  if [[ -L ${device_path}/driver ]]; then
    printf '%s\n' "${bdf}" > "${device_path}/driver/unbind"
  fi
  printf '%s\n' "${bdf}" > /sys/bus/pci/drivers_probe

  bound_driver=$(basename "$(readlink -f "${device_path}/driver")")
  [[ ${bound_driver} == vfio-pci ]] || {
    echo "failed to bind ${bdf} to vfio-pci" >&2
    exit 1
  }
  printf '\n' > "${device_path}/driver_override"
done

for bdf in "${VFIO_DEVICES[@]}"; do
  group_path=/sys/bus/pci/devices/${bdf}/iommu_group/devices
  for group_member_path in "${group_path}"/*; do
    [[ -e ${group_member_path} ]] || continue
    group_member=$(basename "${group_member_path}")
    group_driver=none
    if [[ -L ${group_member_path}/driver ]]; then
      group_driver=$(basename "$(readlink -f "${group_member_path}/driver")")
    fi
    if [[ ${group_driver} != vfio-pci && ${group_driver} != none ]]; then
      echo "IOMMU group member ${group_member} still uses ${group_driver}" >&2
      exit 1
    fi
  done
done

nohup "${QEMU_BIN}" \
  -enable-kvm \
  -name dpdk-vm1 \
  -machine type=pc,accel=kvm \
  -cpu host,kvm=on \
  -smp 40,sockets=2,cores=20,threads=1 \
  -m 40G \
  -object memory-backend-file,id=mem0,size=20G,mem-path="${HUGEPAGE_DIR}",share=on,prealloc=yes \
  -object memory-backend-file,id=mem1,size=20G,mem-path="${HUGEPAGE_DIR}",share=on,prealloc=yes \
  -numa node,nodeid=0,cpus=0-19,memdev=mem0 \
  -numa node,nodeid=1,cpus=20-39,memdev=mem1 \
  -mem-prealloc \
  -device vfio-pci,host=af:00.0 \
  -device vfio-pci,host=af:04.2 \
  -device vfio-pci,host=af:04.3 \
  -device vfio-pci,host=af:04.4 \
  -device vfio-pci,host=af:04.5 \
  -device vfio-pci,host=af:04.6 \
  -device vfio-pci,host=af:04.7 \
  -drive file="${DISK_IMAGE}",format=raw,if=virtio \
  -boot c \
  -net nic,model=virtio \
  -net user,hostfwd=tcp::10022-:22,hostfwd=tcp::8000-:8000 \
  -netdev tap,id=vhostnet0,ifname=tap0,script="${IFUP_SCRIPT}",downscript="${IFDOWN_SCRIPT}",vhost=on \
  -device virtio-net-pci,netdev=vhostnet0,mac="${EXPERIMENT_MAC}" \
  > "${LOG_FILE}" 2>&1 &

qemu_pid=$!
printf '%s\n' "${qemu_pid}" > "${PID_FILE}"
sleep 2
if ! kill -0 "${qemu_pid}" 2>/dev/null; then
  echo "QEMU exited during startup; inspect ${LOG_FILE}" >&2
  tail -n 100 "${LOG_FILE}" >&2 || true
  rm -f "${PID_FILE}"
  exit 1
fi

echo "dpdk-vm1 started with PID ${qemu_pid}"
echo "log: ${LOG_FILE}"
```

Make it executable:

```bash
chmod 0755 /usr/local/sbin/start-dpdk-vm1
```

The default mode verifies that every passthrough function is already bound to
`vfio-pci` and stops if any binding differs:

```bash
cd /path/to/directory-containing-1.img
/usr/local/sbin/start-dpdk-vm1
```

Use the binding mode only after confirming that all seven functions are
reserved for this VM:

```bash
cd /path/to/directory-containing-1.img
/usr/local/sbin/start-dpdk-vm1 --bind-vfio
```

The binding mode records changed drivers in
`/run/dpdk-vm1-vfio-drivers` before detaching the devices.
It does not automatically return them to their original Host drivers when the
VM stops.
If startup fails after binding begins, use the restoration procedure in
section 10 before returning the devices to Host workloads.

The command retains the legacy `-net nic` and `-net user` syntax for the
existing management NIC.
The equivalent explicit form is:

```bash
-netdev user,id=mgmt0,hostfwd=tcp::10022-:22,hostfwd=tcp::8000-:8000 \
-device virtio-net-pci,netdev=mgmt0
```

Use one form or the other, not both.
For a Host shared with untrusted users or networks, bind forwarding to the
loopback address explicitly:

```bash
hostfwd=tcp:127.0.0.1:10022-:22,hostfwd=tcp:127.0.0.1:8000-:8000
```

## 5. Verify QEMU and vhost-net on the Host

Confirm the kernel modules, passthrough bindings, and TAP address:

```bash
test -c /dev/net/tun
test -e /dev/vhost-net
lsmod | grep -E '^(tun|vhost_net|vhost|vhost_iotlb|vfio_pci|vfio)' || true
lspci -nnk -s af:00.0
ip -br addr show tap0
```

Some kernels build these components in, so absence from `lsmod` is not by
itself a failure when the required device nodes exist.
Repeat the `lspci -nnk` check for the remaining passthrough functions.
Each one must report `Kernel driver in use: vfio-pci` while the VM owns it.

Confirm that QEMU remains running and inspect its startup log:

```bash
pgrep -af 'dpdk-vm1'
tail -n 100 qemu-dpdk-vm1.log
```

Confirm that the command line contains `vhost=on`:

```bash
ps -ef | grep '[q]emu.*dpdk-vm1'
```

Then find the QEMU PID and inspect its open file descriptors:

```bash
qemu_pid=$(pgrep -f 'name dpdk-vm1' | head -n 1)
ls -l "/proc/${qemu_pid}/fd" | grep vhost
```

If `lsof` is installed, the equivalent check is:

```bash
lsof -p "${qemu_pid}" | grep '/dev/vhost-net'
```

An open `/dev/vhost-net` descriptor verifies that QEMU activated the kernel
backend rather than relying only on the command-line request.

Verify that Host forwarding still reaches the first NIC:

```bash
ssh -p 10022 <guest-user>@127.0.0.1
curl http://127.0.0.1:8000/
```

## 6. Configure the experiment NIC in the Guest

The startup script assigns `52:54:00:64:00:02` to the experiment NIC so the
Guest does not need to rely on an interface name or PCI enumeration order.

List Guest links, routes, and Virtio PCI devices:

```bash
ip -br link
ip route
lspci -nnk | grep -A 3 -i ethernet
```

The existing user-network NIC normally owns the default route and receives an
address such as `10.0.2.15/24`.
The new TAP-backed NIC initially has no address.

Resolve the experiment interface from its fixed MAC address:

```bash
experiment_mac=52:54:00:64:00:02
experiment_if=$(
  ip -o link | awk -v mac="${experiment_mac}" \
    'tolower($0) ~ mac { name=$2; sub(/:$/, "", name); print name }'
)
test -n "${experiment_if}"
printf 'experiment interface: %s\n' "${experiment_if}"
ethtool -i "${experiment_if}"
```

Configure only the connected subnet and remove any default route that a Guest
network manager may have installed on this interface:

```bash
ip link set "${experiment_if}" up
ip -4 addr flush dev "${experiment_if}"
ip addr add 192.168.100.2/24 dev "${experiment_if}"
ip route del default dev "${experiment_if}" 2>/dev/null || true
ip -br addr show "${experiment_if}"
ip route
```

The resulting routes should resemble:

```text
default via 10.0.2.2 dev ens3
192.168.100.0/24 dev <experiment-if> proto kernel scope link src 192.168.100.2
```

Do not add a default route through `192.168.100.1`.
Confirm explicitly that the experiment interface owns no default route:

```bash
if ip route show default dev "${experiment_if}" | grep -q .; then
  echo "unexpected default route on ${experiment_if}" >&2
  exit 1
fi
```

These commands are temporary and must be repeated after a Guest reboot.
For persistent configuration, create a Guest network profile matched by the
fixed MAC address and omit its gateway and default-route settings.
The intended split is:

```text
Internet traffic            Host experiment traffic
        |                              |
        v                              v
       ens3                           ens11
        |                              |
QEMU user networking             vhost-net and tap0
```

## 7. Validate Host to Guest traffic

Test both directions before running a throughput workload.

On the Host:

```bash
ping 192.168.100.2
```

In the Guest:

```bash
ping 192.168.100.1
```

For a TCP throughput baseline, run the server on the Host:

```bash
iperf3 -s
```

Run these clients in the Guest:

```bash
iperf3 -c 192.168.100.1
iperf3 -c 192.168.100.1 -R
iperf3 -c 192.168.100.1 -P 4
```

These cases measure Guest-to-Host, Host-to-Guest, and four-stream throughput
over the Virtio plus vhost-net path.

## 8. Scope boundary: QEMU vhost-net and Bless vdev

The `vhost=on` option in this guide belongs entirely to the QEMU network
backend:

```text
Guest virtio-net PCI device -> QEMU configuration -> Host vhost-net -> tap0
```

It does not create or configure a Bless vdev.
It also does not exercise the `net_virtio_user` support implemented in Bless.

Bless vdev configuration is a separate DPDK process-level mechanism with its
own frontend creation and backend requirements.
No Bless `--vdev` option is used by this QEMU startup procedure.
Treat the two environments as independent until an explicit integration
procedure is documented and validated.

## 9. Parameter reference

### Virtualization and machine model

| Parameter | Purpose |
|-----------|---------|
| `-enable-kvm` | Enables KVM hardware virtualization instead of software CPU emulation. |
| `-name dpdk-vm1` | Assigns the process and VM display name used for discovery. |
| `-machine type=pc,accel=kvm` | Selects the PC machine model and explicitly selects KVM acceleration. |
| `-cpu host,kvm=on` | Exposes Host CPU features to the Guest and reports the KVM environment. |

`-enable-kvm` and `accel=kvm` express the same acceleration requirement in two
forms.
Keeping both is valid and makes the requirement explicit.

### CPU and Guest NUMA topology

| Parameter | Purpose |
|-----------|---------|
| `-smp 40,sockets=2,cores=20,threads=1` | Creates 40 vCPUs as two sockets with 20 cores per socket and one thread per core. |
| `-numa node,nodeid=0,cpus=0-19,memdev=mem0` | Assigns Guest CPUs 0 through 19 and memory backend `mem0` to Guest NUMA node 0. |
| `-numa node,nodeid=1,cpus=20-39,memdev=mem1` | Assigns Guest CPUs 20 through 39 and memory backend `mem1` to Guest NUMA node 1. |

These parameters define Guest-visible NUMA topology.
They do not by themselves pin QEMU vCPU threads or memory allocations to Host
NUMA nodes.
Use Host CPU affinity and memory policy separately when the experiment needs
strict Host NUMA placement.

### Memory

| Parameter | Purpose |
|-----------|---------|
| `-m 40G` | Sets total Guest RAM to 40 GiB. |
| `memory-backend-file,id=mem0,size=20G,...` | Creates the 20 GiB backend for Guest NUMA node 0. |
| `memory-backend-file,id=mem1,size=20G,...` | Creates the 20 GiB backend for Guest NUMA node 1. |
| `mem-path=/mnt/huge-1G` | Allocates the backend from the 1 GiB hugetlbfs mount. |
| `share=on` | Creates a shared mapping suitable for vhost and other shared-memory I/O paths. |
| `prealloc=yes` | Allocates the backend during startup rather than faulting pages in later. |
| `-mem-prealloc` | Requests global Guest RAM preallocation. |

The backend-specific `prealloc=yes` options already express the allocation
policy for both NUMA backends.
`-mem-prealloc` is redundant in this command but harmless.

### PCI passthrough

Each option of this form adds one Host PCI function to the Guest:

```bash
-device vfio-pci,host=af:00.0
```

If the listed functions are SR-IOV virtual functions, the resulting path is:

```text
Guest PCI driver -> VFIO -> IOMMU -> physical SR-IOV VF
```

QEMU startup fails when a function is absent, still owned by an incompatible
Host driver, or part of a non-viable IOMMU group.

### Storage and boot

| Parameter | Purpose |
|-----------|---------|
| `-drive file=1.img,format=raw,if=virtio` | Uses `1.img` as a raw Virtio block disk. |
| `-boot c` | Boots from the first hard disk using the traditional BIOS boot notation. |

The disk data path is:

```text
Guest virtio-blk -> QEMU block backend -> 1.img
```

### Management network

| Parameter | Purpose |
|-----------|---------|
| `-net nic,model=virtio` | Creates the first Guest Virtio NIC using legacy QEMU syntax. |
| `-net user` | Creates a QEMU user-mode network backend with DHCP, NAT, DNS forwarding, and socket translation. |
| `hostfwd=tcp::10022-:22` | Forwards Host TCP port 10022 to Guest TCP port 22. |
| `hostfwd=tcp::8000-:8000` | Forwards Host TCP port 8000 to Guest TCP port 8000. |

The management data path is:

```text
Guest TCP/IP -> virtio-net -> QEMU userspace -> SLIRP/NAT -> Host socket
```

### Experiment network

The backend and frontend options are a required pair:

```bash
-netdev tap,id=vhostnet0,ifname=tap0,script=/usr/local/libexec/bless-qemu-ifup,downscript=/usr/local/libexec/bless-qemu-ifdown,vhost=on
-device virtio-net-pci,netdev=vhostnet0,mac=52:54:00:64:00:02
```

| Parameter | Purpose |
|-----------|---------|
| `-netdev tap` | Defines a TAP network backend. |
| `id=vhostnet0` | Assigns the QEMU-internal backend identifier referenced by the frontend. |
| `ifname=tap0` | Requests the Host TAP interface name `tap0`. |
| `script=...bless-qemu-ifup` | Configures the Host address and brings `tap0` up after QEMU creates it. |
| `downscript=...bless-qemu-ifdown` | Flushes the Host address and brings `tap0` down during normal QEMU cleanup. |
| `vhost=on` | Requests kernel vhost-net processing for the Virtio data path. |
| `-device virtio-net-pci,netdev=vhostnet0,...` | Creates the second Guest PCI Virtio NIC, assigns its fixed MAC, and connects it to the TAP backend. |

Without vhost-net, QEMU userspace processes the Virtio queues:

```text
Guest -> virtio-net -> virtqueue -> QEMU process -> TAP
```

With vhost-net enabled, the kernel handles the primary data path:

```text
Guest -> virtio-net -> virtqueue -> vhost-net -> TAP
```

vhost-net is an accelerator, not a complete network device.
The TAP interface remains the Host packet endpoint.

### Process and logging

| Shell element | Purpose |
|---------------|---------|
| `nohup` | Keeps QEMU running after the launching terminal exits. |
| `> qemu-dpdk-vm1.log 2>&1` | Records standard output and standard error in one log file. |
| `&` | Runs QEMU as a background process. |

## 10. Stop and clean up

Shut down the Guest cleanly when possible.
To stop QEMU from the Host, use the PID recorded by the startup script:

```bash
qemu_pid=$(cat qemu-dpdk-vm1.pid)
kill "${qemu_pid}"

for attempt in $(seq 1 30); do
  kill -0 "${qemu_pid}" 2>/dev/null || break
  sleep 1
done

if kill -0 "${qemu_pid}" 2>/dev/null; then
  echo "QEMU did not stop within 30 seconds" >&2
else
  rm -f qemu-dpdk-vm1.pid
fi
```

During a normal shutdown, QEMU invokes `bless-qemu-ifdown` and closes its TAP
file descriptor.
A non-persistent TAP created by QEMU then disappears automatically.
Check for a stale interface after an abnormal termination:

```bash
ip -br link show tap0 2>/dev/null || true
```

If `--bind-vfio` changed Host drivers, restore them only after QEMU has
released every passthrough device.
The following command uses the state recorded by the startup script:

```bash
while read -r bdf original_driver; do
  device_path=/sys/bus/pci/devices/${bdf}

  if [[ -L ${device_path}/driver ]]; then
    printf '%s\n' "${bdf}" > "${device_path}/driver/unbind"
  fi

  if [[ ${original_driver} != none ]]; then
    printf '%s\n' "${original_driver}" > "${device_path}/driver_override"
    printf '%s\n' "${bdf}" > /sys/bus/pci/drivers_probe
    printf '\n' > "${device_path}/driver_override"
  fi
done < /run/dpdk-vm1-vfio-drivers

rm -f /run/dpdk-vm1-vfio-drivers
```

Verify the restored drivers with `lspci -nnk` before returning the devices to
Host workloads.
Leave `/mnt/huge-1G` mounted when other VMs or DPDK processes use it.
Unmount it only after confirming that no process owns files or mappings from
that hugetlbfs instance.

## 11. Troubleshooting

| Symptom | Check |
|---------|-------|
| QEMU cannot create `tap0` | Check `/dev/net/tun`, root privileges, and the executable permissions of both TAP scripts. |
| QEMU cannot open `/dev/vhost-net` | Load `vhost_net` and check device permissions for the launch account. |
| QEMU fails to allocate memory | Check free 1 GiB hugepages, the hugetlbfs mount, and the combined 40 GiB backend size. |
| QEMU rejects a VFIO device | Check its PCI address, `vfio-pci` binding, IOMMU group, and whether another process owns it. |
| Guest loses Internet access | Confirm that the default route still uses the first, user-network NIC. |
| Host and Guest cannot ping | Check both link states, the `/24` addresses, Guest firewall rules, and Host firewall rules. |
| No `/dev/vhost-net` descriptor appears | Inspect the QEMU log and confirm `vhost=on` is on the TAP backend. |

For reproducible performance results, record the QEMU command, QEMU version,
Host kernel, Guest kernel, DPDK version, CPU affinity, Host NUMA placement,
hugepage allocation, NIC firmware, and the exact Bless commit with each run.
