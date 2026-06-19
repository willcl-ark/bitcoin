default:
    just --list

# Configure using depends, without GUI
configure:
    cmake -B build -DBITCOIN_BUILD_DEPENDS=ON -DBUILD_GUI=OFF

download-sources:
    cmake -S cmake/depends/sources -B build-deps/source-driver
    cmake --build build-deps/source-driver --target bitcoin-depends-download

make:
    cmake --build build --parallel

# Configure and make using depends, without GUI
build: configure make
    cmake --build build --parallel

build-all-depends:
    make -C depends -j{{num_cpus()}}
    cmake -B build -DBITCOIN_BUILD_DEPENDS=ON --toolchain=depends/x86_64-pc-linux-gnu/toolchain.cmake
    cmake --build build --parallel

# Remove ./build
clean:
    rm -Rf ./build

# Remove ./build and ./build-deps
clean-all: clean
    [ ! -d ./build-deps ] || find ./build-deps -mindepth 1 -maxdepth 1 | grep -vx './build-deps/Download' | xargs -r rm -Rf
