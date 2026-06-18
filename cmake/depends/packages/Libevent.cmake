# Copyright (c) 2026-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

function(bitcoin_depends_add_libevent)
  if(TARGET libevent::core)
    return()
  endif()

  set(libevent_include_dir "${BITCOIN_DEPENDS_PREFIX}/include")
  set(libevent_lib_dir "${BITCOIN_DEPENDS_PREFIX}/lib")
  file(MAKE_DIRECTORY "${libevent_include_dir}" "${libevent_lib_dir}")

  set(libevent_core_library "${libevent_lib_dir}/${CMAKE_STATIC_LIBRARY_PREFIX}event_core${CMAKE_STATIC_LIBRARY_SUFFIX}")
  set(libevent_extra_library "${libevent_lib_dir}/${CMAKE_STATIC_LIBRARY_PREFIX}event_extra${CMAKE_STATIC_LIBRARY_SUFFIX}")
  set(libevent_byproducts
    "${libevent_core_library}"
    "${libevent_extra_library}"
  )
  if(NOT WIN32)
    set(libevent_pthreads_library "${libevent_lib_dir}/${CMAKE_STATIC_LIBRARY_PREFIX}event_pthreads${CMAKE_STATIC_LIBRARY_SUFFIX}")
    list(APPEND libevent_byproducts "${libevent_pthreads_library}")
  endif()

  set(libevent_patch_dir "${PROJECT_SOURCE_DIR}/depends/patches/libevent")
  bitcoin_depends_external_cmake_project(bitcoin_depends_libevent
    URL "https://github.com/libevent/libevent/releases/download/release-2.1.12-stable/libevent-2.1.12-stable.tar.gz"
    SHA256 "92e6de1be9ec176428fd2367677e61ceffc2ee1cb119035037a27d346b0403bb"
    PATCH_COMMAND
      patch -p1 -i "${libevent_patch_dir}/cmake_fixups.patch"
      COMMAND patch -p1 -i "${libevent_patch_dir}/netbsd_fixup.patch"
      COMMAND patch -p1 -i "${libevent_patch_dir}/winver_fixup.patch"
    CMAKE_ARGS
      "-DCMAKE_BUILD_TYPE=None"
      "-DEVENT__DISABLE_BENCHMARK=ON"
      "-DEVENT__DISABLE_OPENSSL=ON"
      "-DEVENT__DISABLE_SAMPLES=ON"
      "-DEVENT__DISABLE_REGRESS=ON"
      "-DEVENT__DISABLE_TESTS=ON"
      "-DEVENT__LIBRARY_TYPE=STATIC"
    BUILD_BYPRODUCTS ${libevent_byproducts}
  )

  add_library(bitcoin_depends_libevent_core INTERFACE)
  add_library(libevent::core ALIAS bitcoin_depends_libevent_core)
  target_include_directories(bitcoin_depends_libevent_core SYSTEM INTERFACE
    "${libevent_include_dir}"
  )
  target_link_libraries(bitcoin_depends_libevent_core INTERFACE
    "${libevent_core_library}"
  )
  add_dependencies(bitcoin_depends_libevent_core bitcoin_depends_libevent)

  add_library(bitcoin_depends_libevent_extra INTERFACE)
  add_library(libevent::extra ALIAS bitcoin_depends_libevent_extra)
  target_include_directories(bitcoin_depends_libevent_extra SYSTEM INTERFACE
    "${libevent_include_dir}"
  )
  target_link_libraries(bitcoin_depends_libevent_extra INTERFACE
    "${libevent_extra_library}"
    libevent::core
  )
  add_dependencies(bitcoin_depends_libevent_extra bitcoin_depends_libevent)

  if(NOT WIN32)
    add_library(bitcoin_depends_libevent_pthreads INTERFACE)
    add_library(libevent::pthreads ALIAS bitcoin_depends_libevent_pthreads)
    target_include_directories(bitcoin_depends_libevent_pthreads SYSTEM INTERFACE
      "${libevent_include_dir}"
    )
    target_link_libraries(bitcoin_depends_libevent_pthreads INTERFACE
      "${libevent_pthreads_library}"
      libevent::core
    )
    add_dependencies(bitcoin_depends_libevent_pthreads bitcoin_depends_libevent)
  endif()
endfunction()
