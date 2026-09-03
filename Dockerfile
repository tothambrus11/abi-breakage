# Hot path: the ABI diff stage, which fans out to many parallel workers.
# Kept minimal on purpose -- no clang, no build toolchain.
FROM debian:trixie-slim
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      abigail-tools binutils file xz-utils zstd curl ca-certificates python3 \
    && rm -rf /var/lib/apt/lists/*
RUN abidiff --version && abipkgdiff --version
WORKDIR /work
