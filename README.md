## BLESS - An Entropy Injector for High Performance Network System


## top level view
```
top/
├── Makefile              # orchestration
├── third_party/
│   ├── third_party.mk    # third party libraries management
│   └── xxx/upstream      # third party library
└── src/
    └── Makefile          # how to build bless
```


Requirements:
- git
- make
- gcc or clang
- pkg-config
- autoconf automake libtool (for libyaml)


## install dependency
```
# install dpdk
pkg-config --cflags libdpdk
pkg-config --libs --static libdpdk
# or
pkg-config --libs libdpdk

sudo apt install clang pkg-config libssl-dev
# or
sudo dnf install clang pkgconf-pkg-config openssl-devel
```


## clone source code
```
```


## third party only
```
make third_party
```


## Build

```bash
git clone https://github.com/erocpil/bless.git
cd bless
make -j
# or
make BUILD=release STATIC=1 -j
```


## Installation
```
make install PREFIX=/opt/bless
```
