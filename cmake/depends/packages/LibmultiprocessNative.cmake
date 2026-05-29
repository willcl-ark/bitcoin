# Copyright (c) 2026-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

ExternalProject_Add(native_libmultiprocess
  SOURCE_DIR "${BITCOIN_REPO_SOURCE_DIR}/src/ipc/libmultiprocess"
  BINARY_DIR "${CMAKE_BINARY_DIR}/_deps/Build/native_libmultiprocess"
  DEPENDS native_capnp
  CMAKE_ARGS
    "-DCMAKE_INSTALL_PREFIX=${BITCOIN_DEPENDS_NATIVE_PREFIX}"
    "-DCMAKE_INSTALL_LIBDIR=lib"
    "-DCMAKE_PREFIX_PATH=${BITCOIN_DEPENDS_NATIVE_PREFIX}"
    "-DBUILD_TESTING=OFF"
  INSTALL_COMMAND "${CMAKE_COMMAND}" --build "<BINARY_DIR>" --target install-bin
)
