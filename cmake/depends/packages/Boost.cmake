# Copyright (c) 2026-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

function(bitcoin_depends_add_boost)
  if(TARGET Boost::headers)
    return()
  endif()

  set(boost_include_dir "${BITCOIN_DEPENDS_PREFIX}/boost/include")
  file(MAKE_DIRECTORY "${boost_include_dir}")

  set(boost_initial_cache "${PROJECT_BINARY_DIR}/_depends/boost-cache.cmake")
  file(WRITE "${boost_initial_cache}" "set(BOOST_INCLUDE_LIBRARIES \"multi_index;test\" CACHE STRING \"\")\n")

  bitcoin_depends_external_cmake_project(bitcoin_depends_boost
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
    BUILD_BYPRODUCTS "${boost_include_dir}/boost/version.hpp"
  )

  add_library(bitcoin_depends_boost_headers INTERFACE)
  add_library(Boost::headers ALIAS bitcoin_depends_boost_headers)
  target_include_directories(bitcoin_depends_boost_headers SYSTEM INTERFACE
    "${boost_include_dir}"
  )
  add_dependencies(bitcoin_depends_boost_headers bitcoin_depends_boost)

  if(BUILD_TESTS AND NOT TARGET boost_included_unit_test_framework)
    add_library(boost_included_unit_test_framework INTERFACE)
    target_link_libraries(boost_included_unit_test_framework INTERFACE
      Boost::headers
    )
    add_dependencies(boost_included_unit_test_framework bitcoin_depends_boost)
  endif()
endfunction()
