# Copyright (c) 2026-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

cmake_minimum_required(VERSION 3.22)

project(BitcoinCoreDepends LANGUAGES C CXX)

include(ExternalProject)
find_package(Patch REQUIRED)
set_property(DIRECTORY PROPERTY EP_BASE "${CMAKE_BINARY_DIR}/_deps")

option(BITCOIN_DEPENDS_WITH_USDT "Build USDT headers." ON)
option(BITCOIN_DEPENDS_WITH_IPC "Build Cap'n Proto and native libmultiprocess tools." ON)
option(WITH_CCACHE "Attempt to use ccache for compiling." ON)

set(BITCOIN_DEPENDS_ENABLE_USDT OFF)
if(BITCOIN_DEPENDS_WITH_USDT AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(BITCOIN_DEPENDS_ENABLE_USDT ON)
endif()

set(BITCOIN_DEPENDS_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
cmake_path(GET CMAKE_CURRENT_SOURCE_DIR PARENT_PATH bitcoin_depends_cmake_dir)
cmake_path(GET bitcoin_depends_cmake_dir PARENT_PATH BITCOIN_REPO_SOURCE_DIR)
include("${BITCOIN_REPO_SOURCE_DIR}/cmake/ccache.cmake")
set(BITCOIN_DEPENDS_PREFIX "${CMAKE_INSTALL_PREFIX}")
set(BITCOIN_DEPENDS_NATIVE_PREFIX "${BITCOIN_DEPENDS_PREFIX}/native")
set(BITCOIN_DEPENDS_TARGET_TOOLCHAIN_FILE "")
if(CMAKE_TOOLCHAIN_FILE)
  cmake_path(IS_ABSOLUTE CMAKE_TOOLCHAIN_FILE toolchain_is_absolute)
  if(toolchain_is_absolute)
    set(BITCOIN_DEPENDS_TARGET_TOOLCHAIN_FILE "${CMAKE_TOOLCHAIN_FILE}")
  elseif(EXISTS "${CMAKE_BINARY_DIR}/${CMAKE_TOOLCHAIN_FILE}")
    cmake_path(ABSOLUTE_PATH CMAKE_TOOLCHAIN_FILE
      BASE_DIRECTORY "${CMAKE_BINARY_DIR}"
      NORMALIZE
      OUTPUT_VARIABLE BITCOIN_DEPENDS_TARGET_TOOLCHAIN_FILE
    )
  else()
    cmake_path(ABSOLUTE_PATH CMAKE_TOOLCHAIN_FILE
      BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      NORMALIZE
      OUTPUT_VARIABLE BITCOIN_DEPENDS_TARGET_TOOLCHAIN_FILE
    )
  endif()
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}")
include(DependsHelpers)

add_custom_target(depends-download)

include(packages/Boost)
if(BITCOIN_DEPENDS_ENABLE_USDT)
  include(packages/SystemTap)
endif()
if(BITCOIN_DEPENDS_WITH_IPC)
  include(packages/CapnProto)
  include(packages/LibmultiprocessNative)
endif()

configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/toolchain.cmake.in"
  "${CMAKE_BINARY_DIR}/toolchain.cmake"
  @ONLY
)
install(FILES "${CMAKE_BINARY_DIR}/toolchain.cmake" DESTINATION ".")
