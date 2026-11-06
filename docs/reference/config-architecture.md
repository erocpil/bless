# Configuration code layout

Configuration remains an internal Bless subsystem. It is not a public library:
the parsed `Cnode` layout, DPDK arguments, server options, protocol extension
registry and mutation table are all application-specific.

The implementation is being split behind the existing `config.h` API:

| File | Responsibility |
|---|---|
| `config_file.c` | Open, validate and map the YAML file; own the mapping and fd |
| `config_yaml.c` | Convert YAML input into the repository's `Node` tree |
| `config_node.c` | Look up nodes by dotted paths and sequence indexes |
| `config_value.c` | Strict scalar integer conversion |
| `config_array.c` | Bounded scalar/sequence, address, port and VNI conversion |
| `config_debug.c` | Print the file state and parsed tree |
| `config_bless_ether.c` | Inner Ethernet, ARP, IPv4, IPv6 and transport parsing |
| `config_bless_vxlan.c` | VXLAN outer Ethernet, IPv4/IPv6 and UDP parsing |
| `config_bless_extra.c` | Erroneous-traffic and protocol-extension parsing |
| `config.c` | System/DPDK parsing, Bless orchestration and Cnode cloning |

`config.c` and the files split from it do not terminate the process. They
return an error to their caller. Startup policy remains in `main.c`, which can
still treat an invalid configuration as fatal. This boundary is required for
future validation commands or configuration reload support.

`config_clone_cnode()` builds a temporary clone and publishes it only after all
allocations succeed. Extension `clone_cfg` callbacks return a status so a
partially cloned worker configuration cannot be accepted.

Fast-path selectors such as `RANDOM_IP_SRC` and `IMIX_PAYLOAD_LEN` live in
`cnode_runtime.h`; they are consumers of an already parsed `Cnode` and are not
part of configuration parsing.

## Remaining cleanup

The protocol-domain split is complete. DPDK/server argument construction and
`Cnode` cloning can be moved later if those sections grow, but they no longer
block isolated testing of packet-field parsing.

`mutation.h` still defines functions and therefore must be included by only one
translation unit. `config_bless_extra.c` is currently that owner through
`erroneous.h`; moving mutation implementations into `.c` files is a separate
prerequisite for removing this restriction.
