# Build + run image for the abistudy C++26 tool.
# Debian sid: the only Debian suite with a C++26-capable GCC alongside
# libabigail-dev. The LLVM tools are pinned to the 23 series from
# apt.llvm.org so that clang-format, clang-tidy and libclang are the same
# versions the gate is validated against locally (scripts/check.sh and
# CMakeLists.txt prefer the -23 binaries): a formatter or checker of a
# different version disagrees with them and turns the gate red for style
# reasons alone. Runtime and build share one image; a multi-stage split is
# a production follow-up noted in METHODOLOGY.md.
FROM debian:sid-slim
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      ca-certificates curl gnupg \
    && curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key \
       | gpg --dearmor -o /usr/share/keyrings/llvm.gpg \
    && echo "deb [signed-by=/usr/share/keyrings/llvm.gpg] http://apt.llvm.org/unstable/ llvm-toolchain-23 main" \
       > /etc/apt/sources.list.d/llvm-23.list \
    && apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      g++ cmake ninja-build pkg-config \
      clang-23 clang-tidy-23 clang-format-23 libclang-23-dev \
      libabigail-dev abigail-tools \
      libcurl4-openssl-dev libssl-dev nlohmann-json3-dev libarchive-dev \
      libc6-dev libdw-dev libelf-dev \
    && rm -rf /var/lib/apt/lists/*
RUN g++ --version | head -1 && clang++-23 --version | head -1 && clang-format-23 --version \
 && clang-tidy-23 --version | head -2 && cmake --version | head -1 \
 && dpkg-query -W -f='${Package} ${Version}\n' libabigail-dev libclang-23-dev nlohmann-json3-dev libarchive-dev libssl-dev \
 && ls -d /usr/lib/llvm-23/include/clang-c \
 && echo 'int main(){}' > /tmp/t.cpp && g++ -std=c++26 /tmp/t.cpp -o /tmp/t && echo "c++26 flag OK"
WORKDIR /work
