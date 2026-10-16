# Linux toolchain matching CI (LLVM 22 from apt.llvm.org), for checks that need
# Linux: the host-offload plugin runs OpenMP target regions on the CPU with
# separate device memory, so a wrong map() section shows up as wrong output.
#
#   docker build -t p05-linux eval/docker
#   docker run --rm -v "$PWD":/p05:ro -v p05-build:/b p05-linux bash -c '
#     cmake -S /p05 -B /b -G Ninja -DCMAKE_PREFIX_PATH=/usr/lib/llvm-22 && ninja -C /b &&
#     cp -r /p05 /tmp/src && cd /tmp/src && P05_TOOL=/b/p05tool CC=clang OFFLOAD=1 \
#     scripts/check_parallel.sh tests/maps.c eval/runtime/maps_main.c'
FROM ubuntu:24.04
ARG LLVM_VERSION=22
RUN apt-get update -qq && apt-get install -y -qq wget gnupg lsb-release software-properties-common ninja-build cmake python3 pipx >/dev/null \
 && wget -q https://apt.llvm.org/llvm.sh && chmod +x llvm.sh && ./llvm.sh "$LLVM_VERSION" >/dev/null \
 && apt-get install -y -qq "libclang-$LLVM_VERSION-dev" "llvm-$LLVM_VERSION-dev" "llvm-$LLVM_VERSION-tools" \
      "clang-tools-$LLVM_VERSION" "libpolly-$LLVM_VERSION-dev" "libclang-cpp$LLVM_VERSION-dev" \
      "libomp-$LLVM_VERSION-dev" "liboffload-$LLVM_VERSION-dev" >/dev/null \
 && pipx install lit >/dev/null
ENV PATH="/root/.local/bin:/usr/lib/llvm-22/bin:$PATH"
