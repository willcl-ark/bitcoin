#!/usr/bin/env bash
set -euxo pipefail

FREEBSD_VERSION="${FREEBSD_VERSION:-14.2}"
FREEBSD_ARCH="${FREEBSD_ARCH:-amd64}"
HOST_ARCH="${HOST_ARCH:-x86_64}"
HOST=${HOST_ARCH}-unknown-freebsd

SYSROOTS_DIR=/opt/sysroots
mkdir -p "$SYSROOTS_DIR"
FREEBSD_SYSROOT="$SYSROOTS_DIR"/freebsd-${FREEBSD_VERSION}

SYSROOT_CFLAGS="--sysroot=$FREEBSD_SYSROOT"
SYSROOT_CXXFLAGS="--sysroot=$FREEBSD_SYSROOT"
SYSROOT_LDFLAGS="--sysroot=$FREEBSD_SYSROOT"

# Get sysroot
if [ ! -f "$FREEBSD_SYSROOT/base.txz" ]; then
    mkdir -p "$FREEBSD_SYSROOT"
    pushd "$FREEBSD_SYSROOT" > /dev/null
    echo "Downloading FreeBSD $FREEBSD_VERSION base system for $FREEBSD_ARCH..."
    wget "http://ftp.plusline.de/FreeBSD/releases/$FREEBSD_ARCH/$FREEBSD_VERSION-RELEASE/base.txz"
    tar -xf base.txz
    popd > /dev/null
fi

# Get sysroot clang version
SYSROOT_CLANG_DIR=$(find "$FREEBSD_SYSROOT/usr/lib/clang" -maxdepth 1 -type d -name "[0-9]*" 2>/dev/null | head -1)
if [ -n "$SYSROOT_CLANG_DIR" ]; then
    SYSROOT_CLANG_VERSION=$(basename "$SYSROOT_CLANG_DIR")
else
    echo "Error: Could not detect Clang version in sysroot"
    exit 1
fi

# Check clang compatibility with sysroot
if [ -n "$SYSROOT_CLANG_VERSION" ]; then
    ENV_CLANG_VERSION=$(clang --version | head -n1 | sed 's/.*clang version \([0-9]\+\).*/\1/')
    echo "Environment Clang: $ENV_CLANG_VERSION"
    echo "Sysroot Clang: $SYSROOT_CLANG_VERSION"
    if [ "$ENV_CLANG_VERSION" != "$SYSROOT_CLANG_VERSION" ]; then
        echo "Warning: Clang version mismatch vs sysroot, may cause compilation errors!"
    fi
fi

# Build depends
make -C depends -j"$(nproc)" \
    HOST="$HOST" \
    NO_QT=1 \
    CFLAGS="$SYSROOT_CFLAGS" \
    CXXFLAGS="$SYSROOT_CXXFLAGS" \
    LDFLAGS="$SYSROOT_LDFLAGS"


# Generate build system
cmake -B build \
    --toolchain "depends/$HOST/toolchain.cmake" \
    -DCMAKE_SYSROOT="$FREEBSD_SYSROOT" \
    -DCMAKE_CXX_FLAGS="-isystem$FREEBSD_SYSROOT/usr/lib/clang/$SYSROOT_CLANG_VERSION/include"

# Main build
cmake --build build --parallel --target bitcoind --target bitcoin-cli
