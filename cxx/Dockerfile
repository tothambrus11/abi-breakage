# Build + run image for the abistudy C++26 tool.
# Debian sid: the only Debian suite with a C++26-capable GCC alongside
# libabigail-dev and libclang-dev. Runtime and build share one image; a
# multi-stage split is a production follow-up noted in METHODOLOGY.md.
FROM debian:sid-slim
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      g++ cmake ninja-build pkg-config ca-certificates clang-tidy clang-format \
      libabigail-dev abigail-tools \
      libclang-dev \
      libcurl4-openssl-dev libssl-dev nlohmann-json3-dev libarchive-dev \
      libc6-dev libdw-dev libelf-dev \
    && rm -rf /var/lib/apt/lists/*
RUN g++ --version | head -1 && cmake --version | head -1 \
 && dpkg-query -W -f='${Package} ${Version}\n' libabigail-dev libclang-dev nlohmann-json3-dev libarchive-dev libssl-dev \
 && ls -d /usr/lib/llvm-*/include/clang-c \
 && echo 'int main(){}' > /tmp/t.cpp && g++ -std=c++26 /tmp/t.cpp -o /tmp/t && echo "c++26 flag OK"
WORKDIR /work
