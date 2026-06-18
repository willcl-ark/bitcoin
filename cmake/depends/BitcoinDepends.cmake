# Copyright (c) 2026-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

include(ExternalProject)

find_program(BITCOIN_DEPENDS_MAKE_PROGRAM NAMES gmake make REQUIRED)

set(BITCOIN_DEPENDS_BUILD_CC "${CMAKE_C_COMPILER}" CACHE STRING "Native C compiler for CMake-built bundled dependency build tools.")
set(BITCOIN_DEPENDS_CACHE_DIR "${PROJECT_SOURCE_DIR}/build-deps" CACHE PATH "Cache directory for CMake-built bundled dependencies.")
set(BITCOIN_DEPENDS_CACHE_KEY "" CACHE STRING "Cache key for CMake-built bundled dependencies. If empty, a key is generated from the configured target.")
set(BITCOIN_DEPENDS_CACHE_VERSION "1" CACHE STRING "Cache layout version for CMake-built bundled dependencies.")
if(BITCOIN_DEPENDS_CACHE_KEY STREQUAL "")
  set(bitcoin_depends_cache_data "CACHE_VERSION=${BITCOIN_DEPENDS_CACHE_VERSION}\n")
  foreach(var
      CMAKE_GENERATOR
      CMAKE_TOOLCHAIN_FILE
      CMAKE_C_COMPILER
      CMAKE_C_COMPILER_ID
      CMAKE_C_COMPILER_VERSION
      CMAKE_C_COMPILER_TARGET
      CMAKE_CXX_COMPILER
      CMAKE_CXX_COMPILER_ID
      CMAKE_CXX_COMPILER_VERSION
      CMAKE_CXX_COMPILER_TARGET
      CMAKE_SYSROOT
      CMAKE_FIND_ROOT_PATH
      CMAKE_FIND_ROOT_PATH_MODE_PROGRAM
      CMAKE_FIND_ROOT_PATH_MODE_LIBRARY
      CMAKE_FIND_ROOT_PATH_MODE_INCLUDE
      CMAKE_FIND_ROOT_PATH_MODE_PACKAGE
      CMAKE_OSX_ARCHITECTURES
      CMAKE_OSX_DEPLOYMENT_TARGET
      CMAKE_OSX_SYSROOT
      CMAKE_SYSTEM_NAME
      CMAKE_SYSTEM_PROCESSOR
      CMAKE_BUILD_TYPE
      CMAKE_POSITION_INDEPENDENT_CODE
      BITCOIN_DEPENDS_C_FLAGS
      BITCOIN_DEPENDS_CPP_FLAGS
      BITCOIN_DEPENDS_CXX_FLAGS
      BITCOIN_DEPENDS_EXE_LINKER_FLAGS
      BITCOIN_DEPENDS_SHARED_LINKER_FLAGS
      BITCOIN_DEPENDS_MODULE_LINKER_FLAGS
      BITCOIN_DEPENDS_STATIC_LINKER_FLAGS
      BITCOIN_DEPENDS_CMAKE_ARGS
      BITCOIN_DEPENDS_BUILD_CC
      BITCOIN_DEPENDS_MAKE_PROGRAM
  )
    if(DEFINED ${var})
      string(APPEND bitcoin_depends_cache_data "${var}=${${var}}\n")
    endif()
  endforeach()
  string(SHA256 bitcoin_depends_cache_hash "${bitcoin_depends_cache_data}")
  string(SUBSTRING "${bitcoin_depends_cache_hash}" 0 16 bitcoin_depends_cache_hash)
  set(bitcoin_depends_effective_cache_key "${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}-${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}-${bitcoin_depends_cache_hash}")
else()
  set(bitcoin_depends_effective_cache_key "${BITCOIN_DEPENDS_CACHE_KEY}")
endif()
string(REGEX REPLACE "[^A-Za-z0-9_.+-]" "_" bitcoin_depends_effective_cache_key "${bitcoin_depends_effective_cache_key}")
set(BITCOIN_DEPENDS_EFFECTIVE_CACHE_KEY "${bitcoin_depends_effective_cache_key}" CACHE INTERNAL "Effective cache key for CMake-built bundled dependencies.")

if(NOT DEFINED CACHE{BITCOIN_DEPENDS_PREFIX})
  set(BITCOIN_DEPENDS_PREFIX "${BITCOIN_DEPENDS_CACHE_DIR}/${bitcoin_depends_effective_cache_key}/prefix" CACHE PATH "Install prefix for CMake-built bundled target dependencies.")
endif()
if(NOT DEFINED CACHE{BITCOIN_DEPENDS_NATIVE_PREFIX})
  set(BITCOIN_DEPENDS_NATIVE_PREFIX "${BITCOIN_DEPENDS_PREFIX}/native" CACHE PATH "Install prefix for CMake-built bundled native tools.")
endif()
set(BITCOIN_DEPENDS_DOWNLOAD_DIR "${BITCOIN_DEPENDS_CACHE_DIR}/Download" CACHE PATH "Download directory for CMake-built bundled dependencies.")
if(NOT DEFINED CACHE{BITCOIN_DEPENDS_BUILD_DIR})
  set(BITCOIN_DEPENDS_BUILD_DIR "${BITCOIN_DEPENDS_CACHE_DIR}/${bitcoin_depends_effective_cache_key}/Build" CACHE PATH "Build directory for CMake-built bundled dependencies.")
endif()
set(BITCOIN_DEPENDS_C_FLAGS "" CACHE STRING "C compiler flags for CMake-built bundled target dependencies.")
set(BITCOIN_DEPENDS_CPP_FLAGS "" CACHE STRING "C/C++ preprocessor flags for CMake-built bundled target dependencies.")
set(BITCOIN_DEPENDS_CXX_FLAGS "" CACHE STRING "C++ compiler flags for CMake-built bundled target dependencies.")
set(BITCOIN_DEPENDS_EXE_LINKER_FLAGS "" CACHE STRING "Executable linker flags for CMake-built bundled target dependencies.")
set(BITCOIN_DEPENDS_SHARED_LINKER_FLAGS "" CACHE STRING "Shared library linker flags for CMake-built bundled target dependencies.")
set(BITCOIN_DEPENDS_MODULE_LINKER_FLAGS "" CACHE STRING "Module library linker flags for CMake-built bundled target dependencies.")
set(BITCOIN_DEPENDS_STATIC_LINKER_FLAGS "" CACHE STRING "Static library linker flags for CMake-built bundled target dependencies.")
set(BITCOIN_DEPENDS_CMAKE_ARGS "" CACHE STRING "Additional CMake arguments for CMake-built bundled target dependencies.")

file(MAKE_DIRECTORY
  "${BITCOIN_DEPENDS_PREFIX}"
  "${BITCOIN_DEPENDS_NATIVE_PREFIX}"
  "${BITCOIN_DEPENDS_DOWNLOAD_DIR}"
  "${BITCOIN_DEPENDS_BUILD_DIR}"
)

set(BITCOIN_DEPENDS_TARGET_CMAKE_ARGS)
foreach(var
    CMAKE_TOOLCHAIN_FILE
    CMAKE_C_COMPILER
    CMAKE_CXX_COMPILER
    CMAKE_C_COMPILER_TARGET
    CMAKE_CXX_COMPILER_TARGET
    CMAKE_SYSROOT
    CMAKE_FIND_ROOT_PATH
    CMAKE_FIND_ROOT_PATH_MODE_PROGRAM
    CMAKE_FIND_ROOT_PATH_MODE_LIBRARY
    CMAKE_FIND_ROOT_PATH_MODE_INCLUDE
    CMAKE_FIND_ROOT_PATH_MODE_PACKAGE
    CMAKE_OSX_ARCHITECTURES
    CMAKE_OSX_DEPLOYMENT_TARGET
    CMAKE_OSX_SYSROOT
    CMAKE_SYSTEM_NAME
    CMAKE_SYSTEM_PROCESSOR
    CMAKE_BUILD_TYPE
)
  if(DEFINED ${var} AND NOT "${${var}}" STREQUAL "")
    list(APPEND BITCOIN_DEPENDS_TARGET_CMAKE_ARGS "-D${var}=${${var}}")
  endif()
endforeach()

foreach(var
    C_FLAGS
    CXX_FLAGS
    EXE_LINKER_FLAGS
    SHARED_LINKER_FLAGS
    MODULE_LINKER_FLAGS
    STATIC_LINKER_FLAGS
)
  if(NOT "${BITCOIN_DEPENDS_${var}}" STREQUAL "")
    list(APPEND BITCOIN_DEPENDS_TARGET_CMAKE_ARGS "-DCMAKE_${var}=${BITCOIN_DEPENDS_${var}}")
  endif()
endforeach()
list(APPEND BITCOIN_DEPENDS_TARGET_CMAKE_ARGS ${BITCOIN_DEPENDS_CMAKE_ARGS})

add_custom_target(bitcoin-depends)
add_custom_target(bitcoin-depends-download)

function(bitcoin_depends_external_cmake_project name)
  set(options)
  set(one_value_args URL SHA256 SOURCE_SUBDIR)
  set(multi_value_args PATCH_COMMAND CMAKE_ARGS BUILD_BYPRODUCTS)
  cmake_parse_arguments(PKG "${options}" "${one_value_args}" "${multi_value_args}" ${ARGN})

  ExternalProject_Add(${name}
    URL "${PKG_URL}"
    URL_HASH "SHA256=${PKG_SHA256}"
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE
    DOWNLOAD_DIR "${BITCOIN_DEPENDS_DOWNLOAD_DIR}"
    PREFIX "${BITCOIN_DEPENDS_BUILD_DIR}/${name}"
    SOURCE_SUBDIR "${PKG_SOURCE_SUBDIR}"
    PATCH_COMMAND ${PKG_PATCH_COMMAND}
    CMAKE_ARGS
      "-DCMAKE_INSTALL_PREFIX=${BITCOIN_DEPENDS_PREFIX}"
      "-DCMAKE_INSTALL_LIBDIR=lib"
      "-DCMAKE_POSITION_INDEPENDENT_CODE=${CMAKE_POSITION_INDEPENDENT_CODE}"
      ${BITCOIN_DEPENDS_TARGET_CMAKE_ARGS}
      ${PKG_CMAKE_ARGS}
    BUILD_BYPRODUCTS ${PKG_BUILD_BYPRODUCTS}
  )
  ExternalProject_Add_StepTargets(${name} download)

  add_dependencies(bitcoin-depends ${name})
  add_dependencies(bitcoin-depends-download ${name}-download)
endfunction()

function(bitcoin_depends_external_autoconf_project name)
  set(options)
  set(one_value_args URL SHA256)
  set(multi_value_args PATCH_COMMAND CONFIGURE_ENV CONFIGURE_ARGS BUILD_COMMAND INSTALL_COMMAND BUILD_BYPRODUCTS)
  cmake_parse_arguments(PKG "${options}" "${one_value_args}" "${multi_value_args}" ${ARGN})

  set(depends_c_flags "${BITCOIN_DEPENDS_C_FLAGS}")
  set(depends_cxx_flags "${BITCOIN_DEPENDS_CXX_FLAGS}")
  set(depends_linker_flags "${BITCOIN_DEPENDS_EXE_LINKER_FLAGS}")
  set(depends_configure_args)
  if(DEFINED CMAKE_C_COMPILER_TARGET AND NOT "${CMAKE_C_COMPILER_TARGET}" STREQUAL "")
    string(PREPEND depends_c_flags "--target=${CMAKE_C_COMPILER_TARGET} ")
    string(PREPEND depends_cxx_flags "--target=${CMAKE_C_COMPILER_TARGET} ")
    string(PREPEND depends_linker_flags "--target=${CMAKE_C_COMPILER_TARGET} ")
    list(APPEND depends_configure_args "--host=${CMAKE_C_COMPILER_TARGET}")
  endif()
  if(DEFINED CMAKE_SYSROOT AND NOT "${CMAKE_SYSROOT}" STREQUAL "")
    string(PREPEND depends_c_flags "--sysroot=${CMAKE_SYSROOT} ")
    string(PREPEND depends_cxx_flags "--sysroot=${CMAKE_SYSROOT} ")
    string(PREPEND depends_linker_flags "--sysroot=${CMAKE_SYSROOT} ")
  endif()

  set(depends_configure_env
    "CC=${CMAKE_C_COMPILER}"
    "CXX=${CMAKE_CXX_COMPILER}"
    "CC_FOR_BUILD=${BITCOIN_DEPENDS_BUILD_CC}"
    "CFLAGS=${depends_c_flags}"
    "CXXFLAGS=${depends_cxx_flags}"
    "CPPFLAGS=${BITCOIN_DEPENDS_CPP_FLAGS}"
    "LDFLAGS=${depends_linker_flags}"
  )
  foreach(var AR NM RANLIB)
    if(DEFINED CMAKE_${var} AND NOT "${CMAKE_${var}}" STREQUAL "")
      list(APPEND depends_configure_env "${var}=${CMAKE_${var}}")
    endif()
  endforeach()
  list(APPEND depends_configure_env ${PKG_CONFIGURE_ENV})

  ExternalProject_Add(${name}
    URL "${PKG_URL}"
    URL_HASH "SHA256=${PKG_SHA256}"
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE
    DOWNLOAD_DIR "${BITCOIN_DEPENDS_DOWNLOAD_DIR}"
    PREFIX "${BITCOIN_DEPENDS_BUILD_DIR}/${name}"
    PATCH_COMMAND ${PKG_PATCH_COMMAND}
    BUILD_IN_SOURCE TRUE
    CONFIGURE_COMMAND
      "${CMAKE_COMMAND}" -E env ${depends_configure_env}
      <SOURCE_DIR>/configure
      "--prefix=${BITCOIN_DEPENDS_PREFIX}"
      ${depends_configure_args}
      ${PKG_CONFIGURE_ARGS}
    BUILD_COMMAND ${PKG_BUILD_COMMAND}
    INSTALL_COMMAND ${PKG_INSTALL_COMMAND}
    BUILD_BYPRODUCTS ${PKG_BUILD_BYPRODUCTS}
  )
  ExternalProject_Add_StepTargets(${name} download)

  add_dependencies(bitcoin-depends ${name})
  add_dependencies(bitcoin-depends-download ${name}-download)
endfunction()

include(cmake/depends/packages/Boost.cmake)
include(cmake/depends/packages/Libevent.cmake)
include(cmake/depends/packages/SQLite.cmake)
include(cmake/depends/packages/SystemTap.cmake)
include(cmake/depends/packages/ZeroMQ.cmake)
