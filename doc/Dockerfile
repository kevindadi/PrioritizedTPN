# PTPN — Linux build image with system Boost (apt).
#
# Run this command from the repository root: the build context stays the repo
# root, so all COPY paths below are relative to it.
#
# Build (native arch — arm64 on Apple Silicon, amd64 on Intel):
#   docker build -f doc/Dockerfile -t ptpn .
#
# Force x86_64 Linux:
#   docker build -f doc/Dockerfile --platform linux/amd64 -t ptpn .
#
# Run (TDG example; example/ is mounted at /data, the default WORKDIR):
#   docker run --rm -v "$PWD/example:/data" ptpn tdg \
#     -f /data/l-bench/pipeline-40t-8c-5l.json

# syntax=docker/dockerfile:1

ARG UBUNTU_VERSION=24.04

FROM ubuntu:${UBUNTU_VERSION} AS base

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates \
        curl \
        git \
        g++ \
        ninja-build \
        cmake \
        libboost-all-dev \
        zip \
        unzip \
        tar \
    && rm -rf /var/lib/apt/lists/*


FROM base AS builder

ARG CMAKE_BUILD_TYPE=Release

WORKDIR /src

COPY . .

RUN cmake -S . -B build -G Ninja \
        -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE}" \
    && cmake --build build \
    && ctest --test-dir build --output-on-failure


FROM ubuntu:${UBUNTU_VERSION} AS runtime

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates \
        libstdc++6 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /src/build/ptpn /usr/local/bin/ptpn
COPY --from=builder /src/build/test/ptpn_test /usr/local/bin/ptpn_test

WORKDIR /data
ENTRYPOINT ["ptpn"]
CMD ["--help"]
