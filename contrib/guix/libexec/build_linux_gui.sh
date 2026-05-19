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
        x86_64-linux-gnu)      echo /lib64/ld-linux-x86-64.so.2 ;;
        arm-linux-gnueabihf)   echo /lib/ld-linux-armhf.so.3 ;;
        aarch64-linux-gnu)     echo /lib/ld-linux-aarch64.so.1 ;;
        riscv64-linux-gnu)     echo /lib/ld-linux-riscv64-lp64d.so.1 ;;
        powerpc64-linux-gnu)   echo /lib64/ld64.so.1;;
        powerpc64le-linux-gnu) echo /lib64/ld64.so.2;;
        *)                     exit 1 ;;
    esac
)

####################
# Depends Building #
####################

# shellcheck disable=SC2119
build_linux_depends

###########################
# Source Tarball Building #
###########################

prepare_linux_source_archive

###########################
# Binary Tarball Building #
###########################

# CONFIGFLAGS
CONFIGFLAGS="-DREDUCE_EXPORTS=ON -DBUILD_BENCH=OFF -DBUILD_GUI_TESTS=OFF -DBUILD_FUZZ_BINARY=OFF -DCMAKE_SKIP_RPATH=TRUE"

set_common_linux_flags

# EXE FLAGS
case "$HOST" in
    *linux*)  CMAKE_EXE_LINKER_FLAGS="-DCMAKE_EXE_LINKER_FLAGS=${HOST_LDFLAGS} -static-libstdc++ -static-libgcc" ;;
esac

mkdir -p "$DISTSRC"
(
    cd "$DISTSRC"

    # Extract the source tarball
    tar --strip-components=1 -xf "${GIT_ARCHIVE}"

    configure_linux_cmake -Werror=dev

    # Build Bitcoin Core
    cmake --build build -j "$JOBS" ${V:+--verbose} --target bitcoin-gui bitcoin-qt

    mkdir -p "$OUTDIR"

    # Setup the directory where our Bitcoin Core build for HOST will be
    # installed. This directory will also later serve as the input for our
    # binary tarballs.
    mkdir -p "${INSTALLPATH}"
    # Install built Bitcoin Core to $INSTALLPATH
    cmake --install build --prefix "${INSTALLPATH}" ${V:+--verbose} --component bitcoin-gui
    cmake --install build --prefix "${INSTALLPATH}" ${V:+--verbose} --component bitcoin-qt

    run_linux_binary_checks
)  # $DISTSRC

. contrib/guix/libexec/package.sh
