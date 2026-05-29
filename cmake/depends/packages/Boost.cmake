# Copyright (c) 2026-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

set(boost_initial_cache "${CMAKE_BINARY_DIR}/boost-cache.cmake")
file(WRITE "${boost_initial_cache}" "set(BOOST_INCLUDE_LIBRARIES \"multi_index;test\" CACHE STRING \"\")\n")

bitcoin_depends_cmake_package(boost
  URL "https://github.com/boostorg/boost/releases/download/boost-1.90.0/boost-1.90.0-cmake.tar.gz"
  SHA256 "913ca43d49e93d1b158c9862009add1518a4c665e7853b349a6492d158b036d4"
  CMAKE_ARGS
    "-C${boost_initial_cache}"
    "-DBOOST_TEST_HEADERS_ONLY=ON"
    "-DBOOST_ENABLE_MPI=OFF"
    "-DBOOST_ENABLE_PYTHON=OFF"
    "-DBOOST_INSTALL_LAYOUT=system"
    "-DBUILD_TESTING=OFF"
    "-DCMAKE_DISABLE_FIND_PACKAGE_ICU=ON"
    "-DCMAKE_INSTALL_INCLUDEDIR=boost/include"
)
