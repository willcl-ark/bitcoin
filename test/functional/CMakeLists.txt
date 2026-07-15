# Copyright (c) 2026-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

set(FUNCTIONAL_TEST_CONFIG_FILE "${PROJECT_BINARY_DIR}/test/config.ini")
set(FUNCTIONAL_TEST_CACHE_DIR "${PROJECT_BINARY_DIR}/test/functional/cache")
set(FUNCTIONAL_TEST_TMP_DIR "${PROJECT_BINARY_DIR}/test/functional/tmp")
set(FUNCTIONAL_TEST_RUNNER "${CMAKE_CURRENT_SOURCE_DIR}/test_runner.py")
if(BUILD_EXTENDED_FUNCTIONAL_TESTS)
  set(FUNCTIONAL_DISCOVERY_EXTENDED_ARG "--extended")
endif()

configure_file(functional_discovery.cmake.in functional_discovery.cmake @ONLY)
set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES
  "${CMAKE_CURRENT_BINARY_DIR}/functional_discovery.cmake"
)

add_test(NAME functional.cache_remove_before
  COMMAND ${CMAKE_COMMAND} -E rm -rf
    "${FUNCTIONAL_TEST_CACHE_DIR}"
    "${FUNCTIONAL_TEST_TMP_DIR}"
  WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)
set_tests_properties(functional.cache_remove_before PROPERTIES
  FIXTURES_SETUP functional_cache
  LABELS functional
)

add_test(NAME functional.create_cache
  COMMAND Python3::Interpreter
    "${CMAKE_CURRENT_SOURCE_DIR}/create_cache.py"
    "--configfile=${FUNCTIONAL_TEST_CONFIG_FILE}"
    "--cachedir=${FUNCTIONAL_TEST_CACHE_DIR}"
    "--portseed=1"
  WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)
set_tests_properties(functional.create_cache PROPERTIES
  DEPENDS functional.cache_remove_before
  ENVIRONMENT "PYTHON_GIL=1"
  FIXTURES_SETUP functional_cache
  LABELS functional
  SKIP_RETURN_CODE 77
)

add_test(NAME functional.cache_remove_after
  COMMAND ${CMAKE_COMMAND} -E rm -rf
    "${FUNCTIONAL_TEST_CACHE_DIR}"
  WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)
set_tests_properties(functional.cache_remove_after PROPERTIES
  FIXTURES_CLEANUP functional_cache
  LABELS functional
)
