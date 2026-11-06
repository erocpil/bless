# ---- Build stage ----
FROM ubuntu:24.04 AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential pkg-config libdpdk-dev libssl-dev \
    libsystemd-dev autoconf automake libtool git ca-certificates python3 \
    cmake xxd \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY . .

RUN if [ ! -f third_party/third_party.mk ]; then make third_party; fi
RUN make -j$(nproc) STATIC=1 BUILD=release

# ---- Runtime stage ----
FROM ubuntu:24.04

# Only runtime deps of the "static" binary.  DPDK static libs on
# ubuntu:24.04 pull in libisal (ISA-L accelerated CRC) and libssl
# (HMAC mutations) at runtime even when linked with -static.
RUN apt-get update && apt-get install -y --no-install-recommends \
    libssl3t64 libisal2 libsystemd0 ca-certificates curl \
    && rm -rf /var/lib/apt/lists/*

RUN mkdir -p /opt/bless/bin /opt/bless/conf /opt/bless/www

COPY --from=builder /build/build/release-static/bin/bless /opt/bless/bin/
COPY --from=builder /build/conf/config.yaml.template    /opt/bless/conf/
COPY --from=builder /build/conf/config-ci.yaml          /opt/bless/conf/
COPY --from=builder /build/tools/www/                   /opt/bless/www/

WORKDIR /opt/bless
# Default: no-huge smoke test with net_null PMD.
# Override CMD with a config file for production use.
ENTRYPOINT ["/opt/bless/bin/bless"]
CMD ["conf/config-ci.yaml"]
