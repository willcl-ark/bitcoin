set shell := ["bash", "-eu", "-o", "pipefail", "-c"]

[private]
default:
    just --list

# Remove ./build directory
clean:
    rm -Rf ./build

# Remove ./build and ./build-deps directories
clean-all:
    rm -Rf ./build ./build-deps

# Fetch depends sources, and build depends
build-depends:
    # Download sources
    cmake -S cmake/depends -B build-deps -DCMAKE_INSTALL_PREFIX="$PWD/build-deps/prefix"
    cmake --build build-deps --target depends-download

    # Build and install
    cmake --build build-deps --target install

# Build bitcoin using depends
build: build-depends
    cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE="$PWD/build-deps/prefix/toolchain.cmake"
    cmake --build build
