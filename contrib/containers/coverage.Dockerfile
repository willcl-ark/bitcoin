# syntax=docker/dockerfile:1
#
# Build and run LLVM source-based coverage in a pinned Ubuntu environment:
#
#   docker build -t bitcoin-core-coverage -f contrib/containers/coverage.Dockerfile .
#   docker network create --ipv6 --subnet 1111:1111::/112 bitcoin-coverage-ip6net
#   docker run --rm --network bitcoin-coverage-ip6net --user "$(id -u):$(id -g)" --env HOME=/tmp -v "$PWD:/src" -w /src bitcoin-core-coverage
#
# For a fully pinned base image, override UBUNTU_IMAGE with an image digest.
ARG UBUNTU_IMAGE=ubuntu:26.04
FROM ${UBUNTU_IMAGE}

ARG DEBIAN_FRONTEND=noninteractive
ARG APT_SNAPSHOT=20260427T000000Z

RUN apt-get update \
 && apt-get install --yes --no-install-recommends ca-certificates \
 && rm -rf /var/lib/apt/lists/*

RUN echo "APT::Snapshot \"${APT_SNAPSHOT}\";" > /etc/apt/apt.conf.d/50snapshot \
 && apt-get update \
 && apt-get install --yes --no-install-recommends \
      build-essential \
      capnproto \
      clang \
      cmake \
      git \
      libboost-dev \
      libcapnp-dev \
      libclang-rt-dev \
      libevent-dev \
      libsqlite3-dev \
      llvm \
      ninja-build \
      pkgconf \
      python3 \
 && rm -rf /var/lib/apt/lists/*

ENV CC=clang
ENV CXX=clang++

WORKDIR /src

CMD ["bash", "-c", "cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Coverage -DCMAKE_C_COMPILER=${CC} -DCMAKE_CXX_COMPILER=${CXX} -DBUILD_GUI=OFF -DWITH_ZMQ=OFF -DWITH_USDT=OFF && cmake --build build --parallel && cmake -DJOBS=$(nproc) -P build/Coverage.cmake"]
