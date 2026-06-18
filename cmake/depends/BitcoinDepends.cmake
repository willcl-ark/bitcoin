# Copyright (c) 2026-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

include(ExternalProject)

set(BITCOIN_DEPENDS_PREFIX "${PROJECT_BINARY_DIR}/_depends/prefix" CACHE PATH "Install prefix for CMake-built bundled target dependencies.")
set(BITCOIN_DEPENDS_NATIVE_PREFIX "${PROJECT_BINARY_DIR}/_depends/native" CACHE PATH "Install prefix for CMake-built bundled native tools.")
set(BITCOIN_DEPENDS_DOWNLOAD_DIR "${PROJECT_BINARY_DIR}/_depends/src" CACHE PATH "Download directory for CMake-built bundled dependencies.")
set(BITCOIN_DEPENDS_BUILD_DIR "${PROJECT_BINARY_DIR}/_depends/build" CACHE PATH "Build directory for CMake-built bundled dependencies.")
set(BITCOIN_DEPENDS_C_FLAGS "" CACHE STRING "C compiler flags for CMake-built bundled target dependencies.")
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
  set(multi_value_args CMAKE_ARGS BUILD_BYPRODUCTS)
  cmake_parse_arguments(PKG "${options}" "${one_value_args}" "${multi_value_args}" ${ARGN})

  ExternalProject_Add(${name}
    URL "${PKG_URL}"
    URL_HASH "SHA256=${PKG_SHA256}"
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE
    DOWNLOAD_DIR "${BITCOIN_DEPENDS_DOWNLOAD_DIR}"
    PREFIX "${BITCOIN_DEPENDS_BUILD_DIR}/${name}"
    SOURCE_SUBDIR "${PKG_SOURCE_SUBDIR}"
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

include(cmake/depends/packages/Boost.cmake)
