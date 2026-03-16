# Copyright (c) 2023-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

enable_language(C)

function(add_secp256k1 subdir)
  message("")
  message("Configuring secp256k1 subtree...")
  set(BUILD_SHARED_LIBS OFF)
  set(CMAKE_EXPORT_COMPILE_COMMANDS OFF)

  # Unconditionally prevent secp's symbols from being exported by our libs
  set(CMAKE_C_VISIBILITY_PRESET hidden)
  set(SECP256K1_ENABLE_API_VISIBILITY_ATTRIBUTES OFF CACHE BOOL "" FORCE)

  set(SECP256K1_ENABLE_MODULE_ECDH OFF CACHE BOOL "" FORCE)
  set(SECP256K1_ENABLE_MODULE_RECOVERY ON CACHE BOOL "" FORCE)
  set(SECP256K1_ENABLE_MODULE_MUSIG ON CACHE BOOL "" FORCE)
  set(SECP256K1_BUILD_BENCHMARK OFF CACHE BOOL "" FORCE)
  set(SECP256K1_BUILD_TESTS ${BUILD_TESTS} CACHE BOOL "" FORCE)
  set(SECP256K1_BUILD_EXHAUSTIVE_TESTS ${BUILD_TESTS} CACHE BOOL "" FORCE)
  if(NOT BUILD_TESTS)
    # Always skip the ctime tests, if we are building no other tests.
    # Otherwise, they are built if Valgrind is available. See SECP256K1_VALGRIND.
    set(SECP256K1_BUILD_CTIME_TESTS ${BUILD_TESTS} CACHE BOOL "" FORCE)
  endif()
  set(SECP256K1_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  # We want to build libsecp256k1 with the most tested RelWithDebInfo configuration.
  foreach(config IN LISTS CMAKE_BUILD_TYPE CMAKE_CONFIGURATION_TYPES)
    if(config STREQUAL "")
      continue()
    endif()
    string(TOUPPER "${config}" config)
    set(CMAKE_C_FLAGS_${config} "${CMAKE_C_FLAGS_RELWITHDEBINFO}")
  endforeach()

  add_subdirectory(${subdir})
  set_target_properties(secp256k1 PROPERTIES
    EXCLUDE_FROM_ALL TRUE
  )
endfunction()
