#!/usr/bin/env bash
# Copyright (c) The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit.
export LC_ALL=C.UTF-8
set -o errexit -o pipefail

# Environment variables for determinism
export TAR_OPTIONS="--no-same-owner --owner=0 --group=0 --numeric-owner --mtime='@${SOURCE_DATE_EPOCH}' --sort=name"
export TZ=UTC

# Although Guix _does_ set umask when building its own packages (in our case,
# this is all packages in manifest.scm), it does not set it for `guix
# shell`. It does make sense for at least `guix shell --container`
# to set umask, so if that change gets merged upstream and we bump the
# time-machine to a commit which includes the aforementioned change, we can
# remove this line.
#
# This line should be placed before any commands which creates files.
umask 0022

if [ -n "$V" ]; then
    # Print both unexpanded (-v) and expanded (-x) forms of commands as they are
    # read from this file.
    set -vx
    # Set VERBOSE for CMake-based builds
    export VERBOSE="$V"
fi

# Check that required environment variables are set
cat << EOF
Required environment variables as seen inside the container:
    DIST_ARCHIVE_BASE: ${DIST_ARCHIVE_BASE:?not set}
    DISTNAME: ${DISTNAME:?not set}
    HOST: ${HOST:?not set}
    SOURCE_DATE_EPOCH: ${SOURCE_DATE_EPOCH:?not set}
    JOBS: ${JOBS:?not set}
    DISTSRC: ${DISTSRC:?not set}
    OUTDIR: ${OUTDIR:?not set}
EOF

export ACTUAL_OUTDIR="${OUTDIR}"
export OUTDIR="${DISTSRC}/output"
export INSTALLPATH="${DISTSRC}/installed/${DISTNAME}"

#####################
# Environment Setup #
#####################

# The depends folder also serves as a base-prefix for depends packages for
# $HOSTs after successfully building.
export BASEPREFIX="${PWD}/depends"

# shellcheck source=toolchain.sh
source "$(dirname "${BASH_SOURCE[0]}")/toolchain.sh"

llvm_toolchain() {
    local CLANG_TOOLCHAIN LIB_CXX

    CLANG_TOOLCHAIN="$(store_path clang-toolchain)"
    LIB_CXX="$(store_path libcxx)"

    export build_CC="${CLANG_TOOLCHAIN}/bin/clang -isystem ${CLANG_TOOLCHAIN}/include"
    export build_CXX="${CLANG_TOOLCHAIN}/bin/clang++ -stdlib=libc++ -isystem ${LIB_CXX}/include/c++/v1 -isystem ${CLANG_TOOLCHAIN}/include"
    export build_LDFLAGS="-fuse-ld=lld -rtlib=compiler-rt -unwindlib=libunwind -L${LIB_CXX}/lib -Wl,-rpath,${LIB_CXX}/lib"
    export build_AR="${CLANG_TOOLCHAIN}/bin/llvm-ar"
    export build_RANLIB="${CLANG_TOOLCHAIN}/bin/llvm-ranlib"
    export build_OBJDUMP="${CLANG_TOOLCHAIN}/bin/llvm-objdump"
    export build_NM="${CLANG_TOOLCHAIN}/bin/llvm-nm"
    export build_STRIP="${CLANG_TOOLCHAIN}/bin/llvm-strip"
}

mingw_w64_toolchain() {
    # Set environment variables to point the NATIVE toolchain to the right
    # includes/libs
    local NATIVE_GCC CROSS_GLIBC CROSS_GCC CROSS_GCC_LIB_STORE CROSS_GCC_LIBS CROSS_GCC_LIB

    NATIVE_GCC="$(store_path gcc-toolchain)"

    # Set native toolchain
    export build_CC="${NATIVE_GCC}/bin/gcc -isystem ${NATIVE_GCC}/include"
    export build_CXX="${NATIVE_GCC}/bin/g++ -isystem ${NATIVE_GCC}/include/c++ -isystem ${NATIVE_GCC}/include"

    # Set environment variables to point the CROSS toolchain to the right
    # includes/libs for $HOST
    # Determine output paths to use in CROSS_* environment variables
    CROSS_GLIBC="$(store_path "mingw-w64-x86_64-winpthreads")"
    CROSS_GCC="$(store_path "gcc-cross-${HOST}")"
    CROSS_GCC_LIB_STORE="$(store_path "gcc-cross-${HOST}" lib)"
    CROSS_GCC_LIBS=( "${CROSS_GCC_LIB_STORE}/lib/gcc/${HOST}"/* ) # This expands to an array of directories...
    CROSS_GCC_LIB="${CROSS_GCC_LIBS[0]}" # ...we just want the first one (there should only be one)

    # The search path ordering is generally:
    #    1. gcc-related search paths
    #    2. libc-related search paths
    #    2. kernel-header-related search paths (not applicable to mingw-w64 hosts)
    export CROSS_C_INCLUDE_PATH="${CROSS_GCC_LIB}/include:${CROSS_GCC_LIB}/include-fixed:${CROSS_GLIBC}/include"
    export CROSS_CPLUS_INCLUDE_PATH="${CROSS_GCC}/include/c++:${CROSS_GCC}/include/c++/${HOST}:${CROSS_GCC}/include/c++/backward:${CROSS_C_INCLUDE_PATH}"
    export CROSS_LIBRARY_PATH="${CROSS_GCC_LIB_STORE}/lib:${CROSS_GCC_LIB}:${CROSS_GLIBC}/lib"

    check_cross_paths "${CROSS_C_INCLUDE_PATH}:${CROSS_CPLUS_INCLUDE_PATH}:${CROSS_LIBRARY_PATH}"
}

# shellcheck source=environment.sh
source "$(dirname "${BASH_SOURCE[0]}")/environment.sh"

###########################
# Source Tarball Building #
###########################

GIT_ARCHIVE="${DIST_ARCHIVE_BASE}/${DISTNAME}.tar.gz"

# Create the source tarball if not already there
if [ ! -e "$GIT_ARCHIVE" ]; then
    mkdir -p "$(dirname "$GIT_ARCHIVE")"
    git archive --prefix="${DISTNAME}/" --output="$GIT_ARCHIVE" HEAD
fi

mkdir -p "$OUTDIR"

clear_compiler_paths
