# Copyright (c) 2026-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

macro(find_capnp)
  find_package(CapnProto 0.7 QUIET NO_MODULE)
  if(NOT CapnProto_FOUND)
    message(FATAL_ERROR
      "Cap'n Proto is required but was not found.\n"
      "To resolve, choose one of the following:\n"
      "  - Install Cap'n Proto (version 1.0+ recommended)\n"
      "  - Build with -DENABLE_IPC=OFF to disable multiprocess support\n"
    )
  endif()

  set(CAPNPROTO_ISSUES "")
  set(CAPNPROTO_CVE_AFFECTED FALSE)
  set(CAPNPROTO_CLANG_INCOMPATIBLE FALSE)

  # Check for list-of-pointers memory access bug from Nov 2022:
  # https://github.com/capnproto/capnproto/security/advisories/GHSA-qqff-4vw4-f6hx
  if(CapnProto_VERSION STREQUAL "0.7.0"
     OR CapnProto_VERSION STREQUAL "0.8.0"
     OR CapnProto_VERSION STREQUAL "0.9.0"
     OR CapnProto_VERSION STREQUAL "0.9.1"
     OR CapnProto_VERSION STREQUAL "0.10.0"
     OR CapnProto_VERSION STREQUAL "0.10.1"
     OR CapnProto_VERSION STREQUAL "0.10.2")
    set(CAPNPROTO_CVE_AFFECTED TRUE)
    string(APPEND CAPNPROTO_ISSUES "- CVE-2022-46149 security vulnerability\n")
  endif()

  # Cap'n Proto 0.9.x and 0.10.x are incompatible with Clang 16+ when
  # compiling as C++20 due to P2468R2 implementation.
  if((CapnProto_VERSION VERSION_GREATER_EQUAL "0.9.0") AND
     (CapnProto_VERSION VERSION_LESS "1.0.0") AND
     (CMAKE_CXX_COMPILER_ID STREQUAL "Clang") AND
     (CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "16") AND
     (CMAKE_CXX_STANDARD EQUAL 20))
    set(CAPNPROTO_CLANG_INCOMPATIBLE TRUE)
    string(APPEND CAPNPROTO_ISSUES "- Incompatible with Clang ${CMAKE_CXX_COMPILER_VERSION} when using C++20\n")
  endif()

  if(CAPNPROTO_CVE_AFFECTED OR CAPNPROTO_CLANG_INCOMPATIBLE)
    set(CAPNPROTO_RESOLUTION_OPTIONS
      "  - Upgrade to Cap'n Proto version 1.0 or newer (recommended)\n"
    )

    if(CAPNPROTO_CVE_AFFECTED AND NOT CAPNPROTO_CLANG_INCOMPATIBLE)
      string(APPEND CAPNPROTO_RESOLUTION_OPTIONS "  - Upgrade to a patched minor version (0.7.1, 0.8.1, 0.9.2, 0.10.3, or later)\n")
    elseif(CAPNPROTO_CLANG_INCOMPATIBLE AND NOT CAPNPROTO_CVE_AFFECTED)
      string(APPEND CAPNPROTO_RESOLUTION_OPTIONS "  - Use GCC instead of Clang\n")
    endif()
    string(APPEND CAPNPROTO_RESOLUTION_OPTIONS "  - Build with -DENABLE_IPC=OFF to disable multiprocess support\n")

    message(FATAL_ERROR
      "The version of Cap'n Proto detected: ${CapnProto_VERSION} has known compatibility issues:\n"
      "${CAPNPROTO_ISSUES}"
      "To resolve, choose one of the following:\n"
      "${CAPNPROTO_RESOLUTION_OPTIONS}"
    )
  endif()

  mark_as_advanced(CapnProto_DIR)
  mark_as_advanced(CapnProto_capnpc_IMPORTED_LOCATION)
  mark_as_advanced(CapnProto_capnp_IMPORTED_LOCATION)
  mark_as_advanced(CapnProto_capnp-json_IMPORTED_LOCATION)
  mark_as_advanced(CapnProto_capnp-rpc_IMPORTED_LOCATION)
  mark_as_advanced(CapnProto_capnp-websocket_IMPORTED_LOCATION)
  mark_as_advanced(CapnProto_kj-async_IMPORTED_LOCATION)
  mark_as_advanced(CapnProto_kj-gzip_IMPORTED_LOCATION)
  mark_as_advanced(CapnProto_kj-http_IMPORTED_LOCATION)
  mark_as_advanced(CapnProto_kj_IMPORTED_LOCATION)
  mark_as_advanced(CapnProto_kj-test_IMPORTED_LOCATION)
  mark_as_advanced(CapnProto_kj-tls_IMPORTED_LOCATION)
endmacro()
