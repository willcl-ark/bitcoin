#!/usr/bin/env bash
# Copyright (c) The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit.
export LC_ALL=C.UTF-8
set -o errexit -o pipefail

# shellcheck source=environment.sh
source contrib/guix/libexec/environment.sh
# shellcheck source=toolchain.sh
source contrib/guix/libexec/toolchain.sh
setup_container_environment
clear_compiler_paths
gcc_toolchain
export BASEPREFIX="$PWD/depends"

environment="$(realpath "$GUIX_ENVIRONMENT")"
if [ -e build/CMakeCache.txt ] && [ "$(cat build/.guix/environment)" != "$environment" ]; then
    echo "The Guix environment changed. Use a fresh ./build directory."
    exit 1
fi
printf '%s\n' "$environment" > build/.guix/environment

action="$1"
shift
case "$action" in
    configure|build)
        # Native tools embed the depends prefix, so cache them per checkout path.
        make -C depends --jobs="$JOBS" HOST="$HOST" \
            SOURCES_PATH="$SOURCES_PATH" \
            BASE_CACHE="$BASE_CACHE/GUIX/BUILD" \
            build_CC="$build_CC" build_CXX="$build_CXX" \
            NO_QT=1 BUILD_ID_SALT="$PWD"
        if [ "$action" = configure ] || [ ! -e build/CMakeCache.txt ]; then
            cmake -S . -B build \
                --toolchain "$BASEPREFIX/$HOST/toolchain.cmake" \
                -DBUILD_BENCH=OFF \
                -DBUILD_FUZZ_BINARY=OFF \
                -DBUILD_GUI=OFF \
                -DBUILD_GUI_TESTS=OFF \
                -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc -Wl,-O2" \
                -DCMAKE_SKIP_RPATH=TRUE \
                -DREDUCE_EXPORTS=ON \
                -DWITH_CCACHE=ON "$@"
        fi
        if [ "$action" = build ]; then
            cmake --build build -j "$JOBS"
        fi
        ;;
    test)
        ctest --test-dir build --output-on-failure -j "$JOBS"
        python3 build/test/functional/test_runner.py --jobs="$JOBS"
        ;;
    shell)
        exec bash --norc
        ;;
esac
