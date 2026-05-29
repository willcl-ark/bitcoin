# CMake Depends

This directory contains CMake package declarations for Bitcoin Core's depends
build.

Configure the dependency project directly to fetch sources only:

```sh
cmake -S cmake/depends -B build-deps
cmake --build build-deps --target depends-download
```

That mode downloads and verifies upstream source archives without configuring,
building, or installing the packages. It is the intended entry point for
hermetic builders that want to pre-populate source inputs.

To fetch, build, and install the dependency prefix:

```sh
cmake -S cmake/depends -B build-deps -DCMAKE_INSTALL_PREFIX="$PWD/build-deps/prefix"
cmake --build build-deps --target install
```

To use an external target toolchain:

```sh
cmake -S cmake/depends -B build-deps \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/toolchain.cmake \
  -DCMAKE_INSTALL_PREFIX="$PWD/build-deps/prefix"
cmake --build build-deps --target install
```

The installed prefix contains `toolchain.cmake`, which can be passed to the main
Bitcoin Core configure step:

```sh
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE="$PWD/build-deps/prefix/toolchain.cmake"
```
