# Copyright (c) 2026-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

function(bitcoin_depends_add_zeromq)
  if(TARGET zeromq)
    return()
  endif()

  set(zeromq_include_dir "${BITCOIN_DEPENDS_PREFIX}/include")
  set(zeromq_lib_dir "${BITCOIN_DEPENDS_PREFIX}/lib")
  file(MAKE_DIRECTORY "${zeromq_include_dir}" "${zeromq_lib_dir}")

  set(zeromq_library "${zeromq_lib_dir}/${CMAKE_STATIC_LIBRARY_PREFIX}zmq${CMAKE_STATIC_LIBRARY_SUFFIX}")
  set(zeromq_patch_dir "${PROJECT_SOURCE_DIR}/depends/patches/zeromq")
  bitcoin_depends_external_cmake_project(bitcoin_depends_zeromq
    URL "https://github.com/zeromq/libzmq/releases/download/v4.3.5/zeromq-4.3.5.tar.gz"
    SHA256 "6653ef5910f17954861fe72332e68b03ca6e4d9c7160eb3a8de5a5a913bfab43"
    PATCH_COMMAND
      patch -p1 -i "${zeromq_patch_dir}/macos_mktemp_check.patch"
      COMMAND patch -p1 -i "${zeromq_patch_dir}/builtin_sha1.patch"
      COMMAND patch -p1 -i "${zeromq_patch_dir}/cacheline_undefined.patch"
      COMMAND patch -p1 -i "${zeromq_patch_dir}/fix_have_windows.patch"
      COMMAND patch -p1 -i "${zeromq_patch_dir}/openbsd_kqueue_headers.patch"
      COMMAND patch -p1 -i "${zeromq_patch_dir}/cmake_minimum.patch"
      COMMAND patch -p1 -i "${zeromq_patch_dir}/no_librt.patch"
    CMAKE_ARGS
      "-DCMAKE_BUILD_TYPE=None"
      "-DWITH_DOCS=OFF"
      "-DWITH_LIBSODIUM=OFF"
      "-DWITH_LIBBSD=OFF"
      "-DENABLE_CURVE=OFF"
      "-DENABLE_CPACK=OFF"
      "-DBUILD_SHARED=OFF"
      "-DBUILD_TESTS=OFF"
      "-DZMQ_BUILD_TESTS=OFF"
      "-DENABLE_DRAFTS=OFF"
    BUILD_BYPRODUCTS "${zeromq_library}"
  )

  add_library(bitcoin_depends_zeromq_target INTERFACE)
  add_library(zeromq ALIAS bitcoin_depends_zeromq_target)
  target_include_directories(bitcoin_depends_zeromq_target INTERFACE
    "${zeromq_include_dir}"
  )
  target_link_libraries(bitcoin_depends_zeromq_target INTERFACE
    "${zeromq_library}"
  )
  add_dependencies(bitcoin_depends_zeromq_target bitcoin_depends_zeromq)
endfunction()
