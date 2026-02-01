# BLESS
**An Entropy Injector for High-Performance Network Systems**

BLESS is an entropy injection and experimental framework designed for high-performance network systems (DPDK / gateway / data plane). It is used to generate controlled traffic, perturb the system, and observe its behavior and uncertainty.

---

## Project Layout
```
top/
├── Makefile              # top-level orchestration
├── third_party/
│   ├── Makefile          # third-party fetch / build
│   ├── third_party.mk    # exported flags for main build
│   └── xxx/upstream/     # third-party library sources (git submodules)
├── src/
└── Makefile              # bless build rules
```

- **Top-level Makefile**: Orchestrates third-party library fetching and the main bless build.
- **third_party/**: Handles the fetching, building, and exporting of third-party libraries.
- **src/**: The actual code for BLESS.

---

## Requirements

### Build tools

- git
- make
- gcc or clang
- pkg-config
- autoconf / automake / libtool (for libyaml)

### Runtime / Link Dependencies

- DPDK (via pkg-config)
- OpenSSL

---

## Install Dependencies

### DPDK

Ensure that DPDK can be found via `pkg-config`:

```bash
pkg-config --cflags libdpdk
pkg-config --libs libdpdk
# or (static)
pkg-config --libs --static libdpdk
```

### Ubuntu / Debian
```
sudo apt install \
    clang \
    pkg-config \
    libssl-dev \
    autoconf automake libtool
```

### Fedora / RHEL
```
sudo dnf install \
    clang \
    pkgconf-pkg-config \
    openssl-devel \
    autoconf automake libtool
```

## Clone the Source Code

BLESS uses git submodules to manage third-party libraries.

Recommended way (fetch everything at once):
```
git clone --recurse-submodules https://github.com/erocpil/bless.git
```

If you have already cloned the repository without submodules, run:
```
git submodule update --init --recursive
```

Third-Party Libraries Only

To fetch and build third-party libraries only:
```
make third_party
```

This command will:

Initialize and update all submodules

Build the required third-party libraries

Generate the third_party/third_party.mk file for use in the main project

## Build BLESS
### Default Build

To build the project:
```
make -j
```

### Common Build Options
```
make BUILD=release -j
make STATIC=1 -j
make BUILD=release STATIC=1 -j
```
| Variable        | Meaning                                          |
| --------------- | ------------------------------------------------ |
| `BUILD=release` | Enable optimizations (`-O2 -DNDEBUG`)            |
| `STATIC=1`      | Attempt to statically link third-party libraries |
| `V=1`           | Show full compilation commands                   |

Example:
```
make BUILD=release STATIC=1 V=1 -j
```

## Installation

To install BLESS:
```
make install PREFIX=/opt/bless
```

Optional variables:

| Variable  | Meaning                                     |
| --------- | ------------------------------------------- |
| `PREFIX`  | Installation prefix (default: `/usr/local`) |
| `DESTDIR` | Directory for staging or packaging          |


Example (for packaging):
```
make install DESTDIR=/tmp/pkg PREFIX=/usr
```

Installation will copy files to the following directories:

- bin/: The bless executable
- lib/: Required runtime libraries
- conf/: Example configuration files
- www/: Web-related resources
- include/bless/: Public headers

## Notes

Third-party library versions are fixed via git submodules to ensure reproducible builds.

All build parameters are recorded in the binary's build info for traceability.

It is recommended to use clang for better diagnostics.

## License

MIT License. See the LICENSE file for details.

---

### Key Changes / Enhancements:

1. **Clear Explanation of the Project Layout**:
   - I added a short explanation of the project layout for first-time users, so they know what each folder is used for.

2. **Dependencies Clarified**:
   - Split dependencies into **build tools** and **runtime/link dependencies** to avoid confusion about what’s needed to get started vs. what’s needed to run the project.

3. **Step-by-Step Instructions**:
   - Added clarity in each section about the build process and how third-party libraries are fetched and built.

4. **Installation and Configuration**:
   - Clarified the **DESTDIR/PREFIX** environment variables and gave clear examples of how to package the project (using `DESTDIR`).

5. **Simplified Notes**:
   - I made sure that the instructions are clear and friendly for anyone who wants to build the project from scratch, even if they're unfamiliar with it.

6. **General Formatting**:
   - Reformatted some sections to be clearer and more professional, especially the **build options** and **third-party library management**.

---
