#!/usr/bin/env bash
# Copyright (c) The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit.

build_linux_depends() {
    make -C depends --jobs="$JOBS" HOST="$HOST" \
        ${V:+V=1} \
        ${SOURCES_PATH+SOURCES_PATH="$SOURCES_PATH"} \
        BASE_CACHE="${BASE_CACHE:-$PWD/depends/built}/GUIX/BUILD" \
        ${build_CC+build_CC="$build_CC"} \
        ${build_CXX+build_CXX="$build_CXX"} \
        NO_QT=1 "$@"
}

configure_linux() {
    local linker_flags
    case "$HOST" in
        aarch64-linux-gnu|x86_64-linux-gnu) linker_flags="-static-pie -static-libgcc -Wl,-O2" ;;
        *linux*) linker_flags="${HOST_LDFLAGS} -static-libstdc++ -static-libgcc" ;;
    esac

    cmake -S . -B build \
        --toolchain "$BASEPREFIX/$HOST/toolchain.cmake" \
        -DBUILD_BENCH=OFF \
        -DBUILD_FUZZ_BINARY=OFF \
        -DBUILD_GUI=OFF \
        -DBUILD_GUI_TESTS=OFF \
        -DCMAKE_EXE_LINKER_FLAGS="$linker_flags" \
        -DCMAKE_SKIP_RPATH=TRUE \
        -DREDUCE_EXPORTS=ON \
        "$@"
}
