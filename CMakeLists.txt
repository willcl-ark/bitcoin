# Copyright (c) 2023-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

cmake_minimum_required(VERSION 3.22)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
  message(FATAL_ERROR "In-source builds are not allowed.")
endif()

if(POLICY CMP0171)
  cmake_policy(SET CMP0171 NEW)
endif()

set(CLIENT_NAME "Bitcoin Core")
set(CLIENT_VERSION_MAJOR 31)
set(CLIENT_VERSION_MINOR 99)
set(CLIENT_VERSION_BUILD 0)
set(CLIENT_VERSION_RC 0)
set(CLIENT_VERSION_IS_RELEASE "false")
set(COPYRIGHT_YEAR "2026")

project(BitcoinCore
  VERSION ${CLIENT_VERSION_MAJOR}.${CLIENT_VERSION_MINOR}.${CLIENT_VERSION_BUILD}
  DESCRIPTION "Bitcoin client software"
  HOMEPAGE_URL "https://bitcoincore.org/"
  LANGUAGES CXX
)

set(CLIENT_VERSION_STRING ${PROJECT_VERSION})
if(CLIENT_VERSION_RC GREATER 0)
  string(APPEND CLIENT_VERSION_STRING "rc${CLIENT_VERSION_RC}")
endif()

set(COPYRIGHT_HOLDERS "The %s developers")
set(COPYRIGHT_HOLDERS_FINAL "The ${CLIENT_NAME} developers")
set(CLIENT_BUGREPORT "https://github.com/bitcoin/bitcoin/issues")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake/module)

include(CMakeDependentOption)

option(BUILD_BITCOIN_BIN "Build bitcoin executable." ON)
option(BUILD_DAEMON "Build bitcoind executable." ON)
option(BUILD_GUI "Build bitcoin-qt executable." OFF)
option(BUILD_CLI "Build bitcoin-cli executable." ON)

option(BUILD_TESTS "Build test_bitcoin and other unit test executables." ON)
option(BUILD_TX "Build bitcoin-tx executable." ${BUILD_TESTS})
option(BUILD_UTIL "Build bitcoin-util executable." ${BUILD_TESTS})

option(BUILD_UTIL_CHAINSTATE "Build experimental bitcoin-chainstate executable." OFF)
option(BUILD_KERNEL_LIB "Build experimental bitcoinkernel library." ${BUILD_UTIL_CHAINSTATE})
cmake_dependent_option(BUILD_KERNEL_TEST "Build tests for the experimental bitcoinkernel library." ON "BUILD_KERNEL_LIB" OFF)

option(ENABLE_WALLET "Enable wallet." ON)
if(ENABLE_WALLET)
  if(VCPKG_TARGET_TRIPLET)
    find_package(unofficial-sqlite3 CONFIG REQUIRED)
  else()
    find_package(SQLite3 3.7.17 REQUIRED)
  endif()
endif()
cmake_dependent_option(BUILD_WALLET_TOOL "Build bitcoin-wallet tool." ${BUILD_TESTS} "ENABLE_WALLET" OFF)

option(WITH_ZMQ "Enable ZMQ notifications." OFF)
if(WITH_ZMQ)
  find_package(ZeroMQ 4.0.0 MODULE REQUIRED)
endif()

option(WITH_EMBEDDED_ASMAP "Embed default ASMap data." ON)

option(WITH_USDT "Enable tracepoints for Userspace, Statically Defined Tracing." OFF)
if(WITH_USDT)
  find_package(USDT MODULE REQUIRED)
endif()

option(ENABLE_EXTERNAL_SIGNER "Enable external signer support." ON)

cmake_dependent_option(WITH_QRENCODE "Enable QR code support." ON "BUILD_GUI" OFF)
if(WITH_QRENCODE)
  find_package(QRencode MODULE REQUIRED)
  set(USE_QRCODE TRUE)
endif()

cmake_dependent_option(WITH_DBUS "Enable DBus support." ON "NOT CMAKE_SYSTEM_NAME MATCHES \"(Windows|Darwin)\" AND BUILD_GUI" OFF)

cmake_dependent_option(ENABLE_IPC "Build multiprocess bitcoin-node and bitcoin-gui executables in addition to monolithic bitcoind and bitcoin-qt executables." ON "NOT WIN32" OFF)
cmake_dependent_option(WITH_EXTERNAL_LIBMULTIPROCESS "Build with external libmultiprocess library instead of with local git subtree when ENABLE_IPC is enabled. This is not normally recommended, but can be useful for developing libmultiprocess itself." OFF "ENABLE_IPC" OFF)
if(ENABLE_IPC AND WITH_EXTERNAL_LIBMULTIPROCESS)
  find_package(Libmultiprocess REQUIRED COMPONENTS Lib)
  find_package(LibmultiprocessNative REQUIRED COMPONENTS Bin
    NAMES Libmultiprocess
  )
endif()

cmake_dependent_option(BUILD_GUI_TESTS "Build test_bitcoin-qt executable." ON "BUILD_GUI;BUILD_TESTS" OFF)
if(BUILD_GUI)
  set(qt_components Core Gui Widgets LinguistTools)
  if(ENABLE_WALLET)
    list(APPEND qt_components Network)
  endif()
  if(WITH_DBUS)
    list(APPEND qt_components DBus)
    set(USE_DBUS TRUE)
  endif()
  if(BUILD_GUI_TESTS)
    list(APPEND qt_components Test)
  endif()
  find_package(Qt 6.2 MODULE REQUIRED
    COMPONENTS ${qt_components}
  )
  unset(qt_components)
endif()

option(BUILD_BENCH "Build bench_bitcoin executable." OFF)
option(BUILD_FUZZ_BINARY "Build fuzz binary." OFF)
option(INSTALL_MAN "Install man pages." ON)

add_library(core_interface INTERFACE)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
target_link_libraries(core_interface INTERFACE Threads::Threads)

target_compile_definitions(core_interface INTERFACE
  ${DEPENDS_COMPILE_DEFINITIONS}
  $<$<CONFIG:RelWithDebInfo>:${DEPENDS_COMPILE_DEFINITIONS_RELWITHDEBINFO}>
  $<$<CONFIG:Debug>:${DEPENDS_COMPILE_DEFINITIONS_DEBUG}>
)

add_library(sanitize_interface INTERFACE)
target_link_libraries(core_interface INTERFACE sanitize_interface)

add_library(fuzzer_interface INTERFACE)
set(FUZZ_BINARY_LINKS_WITHOUT_MAIN_FUNCTION OFF)

include(AddBoostIfNeeded)
add_boost_if_needed()

if(BUILD_DAEMON OR BUILD_GUI OR BUILD_CLI OR BUILD_TESTS OR BUILD_BENCH OR BUILD_FUZZ_BINARY)
  find_package(Libevent 2.1.8 MODULE REQUIRED)
endif()

include(cmake/introspection.cmake)

if(BUILD_TESTS)
  enable_testing()
endif()

add_subdirectory(test)
add_subdirectory(doc)
add_subdirectory(src)
