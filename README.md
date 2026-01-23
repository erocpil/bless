## BLESS - An Entropy Injector for High Performance Network System

##
```
top/
├── Makefile              # orchestration
├── third_party/
│   ├── third_party.mk    # third party libraries management
│   └── xxx/upstream
└── src/
    └── Makefile          # how to build bless
```


## dependency
```
pkg-config --cflags libdpdk
pkg-config --libs --static libdpdk
# or
pkg-config --libs libdpdk

sudo apt install clang pkg-config libssl-dev
# or
sudo dnf install clang pkgconf-pkg-config openssl-devel
```


## source code
```
git clone https://github.com/xxx/bless.git
cd bless
```


## third party
```
make third_party
```


## build src
```
make
# or
make BUILD=release STATIC=1 -j
```

## Installation
```
make install PREFIX=/opt/bless
```
