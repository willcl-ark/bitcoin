# Copyright (c) 2026-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

set(CAPNPROTO_URL "https://capnproto.org/capnproto-c++-1.4.0.tar.gz")
set(CAPNPROTO_SHA256 "fa02378ad522b318916b9ad928d1372fc9abd43dd1f4f0392e50450f5c87828f")

bitcoin_depends_cmake_package(native_capnp
  NATIVE
  URL "${CAPNPROTO_URL}"
  SHA256 "${CAPNPROTO_SHA256}"
  PREFIX "${BITCOIN_DEPENDS_NATIVE_PREFIX}"
  CMAKE_ARGS
    "-DBUILD_TESTING=OFF"
    "-DWITH_OPENSSL=OFF"
    "-DWITH_ZLIB=OFF"
)

bitcoin_depends_cmake_package(capnp
  URL "${CAPNPROTO_URL}"
  SHA256 "${CAPNPROTO_SHA256}"
  DEPENDS native_capnp
  CMAKE_ARGS
    "-DCMAKE_PREFIX_PATH=${BITCOIN_DEPENDS_NATIVE_PREFIX}"
    "-DBUILD_TESTING=OFF"
    "-DWITH_OPENSSL=OFF"
    "-DWITH_ZLIB=OFF"
)
