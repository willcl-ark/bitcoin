#!/usr/bin/env bash
# Copyright (c) The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit.

# Sourced by a package's generated build script inside its private /bitcoin.
set -eo pipefail
export LC_ALL=C.UTF-8 TZ=UTC
umask 0022

source "${GUIX_ENVIRONMENT}/etc/profile"
unset LIBRARY_PATH CPATH C_INCLUDE_PATH CPLUS_INCLUDE_PATH OBJC_INCLUDE_PATH OBJCPLUS_INCLUDE_PATH
export HOST="$target"
source "$toolchain_script"

case "$target" in
    *linux*) system_name=Linux ;;
    *mingw32) system_name=Windows ;;
    *darwin) system_name=Darwin ;;
    *) echo "Unsupported depends target: $target" >&2; exit 1 ;;
esac

host_prefix="/bitcoin/depends/$target"
native_prefix="$host_prefix/native"
prefix="$host_prefix"
build_AR="${build_AR:-ar}"
build_NM="${build_NM:-nm}"
build_RANLIB="${build_RANLIB:-ranlib}"
build_STRIP="${build_STRIP:-strip}"

cc="$target-gcc"
cxx="$target-g++"
ar="$target-ar"
nm="$target-nm"
ranlib="$target-ranlib"
cflags="-O2 -pipe -std=c11"
cxxflags="-O2 -pipe -std=c++20"
ldflags=""
export CFLAGS="" CXXFLAGS="" CFLAGS_RELEASE="-O2" CXXFLAGS_RELEASE="-O2"
export CFLAGS_DEBUG="" CXXFLAGS_DEBUG=""
if [ "$target" = "$build_triplet" ]; then
    cc=gcc; cxx=g++; ar=ar; nm=nm; ranlib=ranlib
fi
case "$target" in
    *mingw32)
        if command -v "$target-gcc-posix" >/dev/null; then cc="$target-gcc-posix"; fi
        if command -v "$target-g++-posix" >/dev/null; then cxx="$target-g++-posix"; fi
        ;;
    *darwin)
        sdk=/bitcoin/depends/SDKs/Xcode-26.1.1-17B100-extracted-SDK-with-libcxx-headers
        export OSX_SDK="$sdk" OSX_SDK_VERSION=14.0 XCODE_VERSION=26.1.1
        cc="$(command -v clang) --target=$target -isysroot$sdk -nostdlibinc -iwithsysroot/usr/include -iframeworkwithsysroot/System/Library/Frameworks"
        cxx="$(command -v clang++) --target=$target -isysroot$sdk -nostdlibinc -iwithsysroot/usr/include/c++/v1 -iwithsysroot/usr/include -iframeworkwithsysroot/System/Library/Frameworks"
        ar="$(command -v llvm-ar)"
        nm="$(command -v llvm-nm)"
        ranlib="$(command -v llvm-ranlib)"
        cflags="-mmacos-version-min=14.0 -mlinker-version=711 -O2 -pipe -std=c11"
        cxxflags="-mmacos-version-min=14.0 -Xclang -fno-cxx-modules -mlinker-version=711 -O2 -pipe -std=c++20"
        ldflags="-Wl,-platform_version,macos,14.0,14.0 -Wl,-no_adhoc_codesign -fuse-ld=lld"
        CFLAGS="-mmacos-version-min=14.0 -mlinker-version=711"
        CXXFLAGS="-mmacos-version-min=14.0 -Xclang -fno-cxx-modules -mlinker-version=711"
        ;;
esac

if [ "$native" = 1 ]; then
    prefix="$native_prefix"
    cc="$build_CC"; cxx="$build_CXX"
    ar="$build_AR"; nm="$build_NM"; ranlib="$build_RANLIB"
    cflags="-pipe -std=c11"
    cxxflags="-pipe -std=c++20"
    ldflags="${build_LDFLAGS:-}"
fi
export CPPFLAGS="-I$prefix/include" LDFLAGS="$ldflags"
cppflags="-I$prefix/include"
ldflags+=" -L$prefix/lib"
export PATH="$native_prefix/bin:$PATH"
export PKG_CONFIG_LIBDIR="$prefix/lib/pkgconfig"
export PKG_CONFIG_PATH="$prefix/share/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR=/
export CMAKE_MODULE_PATH="$prefix/lib/cmake"

configure_cmake() {
    local args=(
        -G "Unix Makefiles"
        "-DCMAKE_INSTALL_PREFIX:PATH=$prefix"
        "-DCMAKE_AR=$(command -v "$ar")"
        "-DCMAKE_NM=$(command -v "$nm")"
        "-DCMAKE_RANLIB=$(command -v "$ranlib")"
        -DCMAKE_INSTALL_LIBDIR=lib
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON
        -DCMAKE_VERBOSE_MAKEFILE:BOOL=
        -DCMAKE_EXPORT_NO_PACKAGE_REGISTRY:BOOL=TRUE
    )
    if [ "$native" = 1 ]; then
        args+=("-DCMAKE_INSTALL_RPATH:PATH=$prefix/lib")
    elif [ "$target" != "$build_triplet" ]; then
        args+=("-DCMAKE_SYSTEM_NAME=$system_name"
               "-DCMAKE_C_COMPILER_TARGET=$target"
               "-DCMAKE_CXX_COMPILER_TARGET=$target")
    fi
    env CC="$cc" CFLAGS="$cppflags $cflags" \
        CXX="$cxx" CXXFLAGS="$cppflags $cxxflags" LDFLAGS="$ldflags" \
        cmake "${args[@]}" "$@"
}

configure_autoconf() {
    local host="$target"
    local pic=()
    if [ "$native" = 1 ]; then host="$build_triplet"; fi
    if [ "$autoconf_with_pic" = 1 ]; then pic=(--with-pic); fi
    ./configure "--build=$build_triplet" "--host=$host" "--prefix=$prefix" \
        "${pic[@]}" "$@" CC="$cc" CXX="$cxx" NM="$nm" RANLIB="$ranlib" \
        AR="$ar" CFLAGS="$cflags" CXXFLAGS="$cxxflags" CPPFLAGS="$cppflags" \
        LDFLAGS="$ldflags"
}
