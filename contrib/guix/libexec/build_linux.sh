#!/usr/bin/env bash
# Copyright (c) The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

. contrib/guix/libexec/setup.sh
# shellcheck source=build_linux_common.sh
. contrib/guix/libexec/build_linux_common.sh

setup_linux_toolchains

# Determine the correct value for -Wl,--dynamic-linker for the current $HOST

glibc_dynamic_linker=$(
    case "$HOST" in
        aarch64-linux-gnu|riscv64-linux-gnu|x86_64-linux-gnu)      ;;
        arm-linux-gnueabihf)   echo /lib/ld-linux-armhf.so.3 ;;
        powerpc64-linux-gnu)   echo /lib64/ld64.so.1;;
        powerpc64le-linux-gnu) echo /lib64/ld64.so.2;;
        *)                     exit 1 ;;
    esac
)

####################
# Depends Building #
####################

build_linux_depends NO_QT=1

###########################
# Source Tarball Building #
###########################

prepare_linux_source_archive

###########################
# Binary Tarball Building #
###########################

# CONFIGFLAGS
CONFIGFLAGS="-DREDUCE_EXPORTS=ON -DBUILD_BENCH=OFF -DBUILD_FUZZ_BINARY=OFF -DCMAKE_SKIP_RPATH=TRUE"

set_common_linux_flags

# EXE FLAGS
case "$HOST" in
    aarch64-linux-gnu|riscv64-linux-gnu|x86_64-linux-gnu) CMAKE_EXE_LINKER_FLAGS="-DCMAKE_EXE_LINKER_FLAGS=-static-pie -static-libgcc -Wl,-O2" ;;
    *linux*)  CMAKE_EXE_LINKER_FLAGS="-DCMAKE_EXE_LINKER_FLAGS=${HOST_LDFLAGS} -static-libstdc++ -static-libgcc" ;;
esac

mkdir -p "$DISTSRC"
(
    cd "$DISTSRC"

    # Extract the source tarball
    tar --strip-components=1 -xf "${GIT_ARCHIVE}"

    # shellcheck disable=SC2119
    configure_linux_cmake

    # Build Bitcoin Core
    cmake --build build -j "$JOBS" ${V:+--verbose}

    # Setup the directory where our Bitcoin Core build for HOST will be
    # installed. This directory will also later serve as the input for our
    # binary tarballs.
    mkdir -p "${INSTALLPATH}"
    # Install built Bitcoin Core to $INSTALLPATH
    cmake --install build --prefix "${INSTALLPATH}" ${V:+--verbose}
)

rm -rf "$DISTSRC"/build

exit 0
