# Copyright (c) 2023-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

function(create_test_config)
  set(abs_top_srcdir ${PROJECT_SOURCE_DIR})
  set(abs_top_builddir ${PROJECT_BINARY_DIR})
  set(EXEEXT ${CMAKE_EXECUTABLE_SUFFIX})

  macro(set_configure_variable var conf_var)
    if(${var})
      set(${conf_var}_TRUE "")
    else()
      set(${conf_var}_TRUE "#")
    endif()
  endmacro()

  set_configure_variable(ENABLE_WALLET ENABLE_WALLET)
  set_configure_variable(BUILD_CLI BUILD_BITCOIN_CLI)
  set_configure_variable(BUILD_TX BUILD_BITCOIN_TX)
  set_configure_variable(BUILD_UTIL BUILD_BITCOIN_UTIL)
  set_configure_variable(BUILD_UTIL_CHAINSTATE BUILD_BITCOIN_CHAINSTATE)
  set_configure_variable(BUILD_WALLET_TOOL BUILD_BITCOIN_WALLET)
  set_configure_variable(BUILD_DAEMON BUILD_BITCOIND)
  set_configure_variable(BUILD_FUZZ_BINARY ENABLE_FUZZ_BINARY)
  set_configure_variable(WITH_ZMQ ENABLE_ZMQ)
  set_configure_variable(ENABLE_EXTERNAL_SIGNER ENABLE_EXTERNAL_SIGNER)
  set_configure_variable(WITH_USDT ENABLE_USDT_TRACEPOINTS)
  set_configure_variable(ENABLE_IPC ENABLE_IPC)

  configure_file(config.ini.in config.ini USE_SOURCE_PERMISSIONS @ONLY)
endfunction()

create_test_config()

file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/functional)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/fuzz)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/util)

file(GLOB_RECURSE functional_tests RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} functional/*)
foreach(script ${functional_tests} fuzz/test_runner.py)
  if(CMAKE_HOST_WIN32)
    set(symlink)
  else()
    set(symlink SYMBOLIC)
  endif()
  file(CREATE_LINK ${CMAKE_CURRENT_SOURCE_DIR}/${script} ${CMAKE_CURRENT_BINARY_DIR}/${script} COPY_ON_ERROR ${symlink})
endforeach()
unset(functional_tests)

# Functional test discovery
# Generate a CMake script that will be included by CTest to discover functional tests
set(functional_include_file "${CMAKE_CURRENT_BINARY_DIR}/functional_tests_include.cmake")

string(CONCAT functional_include_content
  "# Generated functional test discovery for CTest\n"
  "# This file is included by CTest to dynamically discover and register functional tests\n"
  "\n"
  "execute_process(\n"
  "  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_BINARY_DIR}/functional/list_tests.py\n"
  "  OUTPUT_VARIABLE functional_test_output\n"
  "  OUTPUT_STRIP_TRAILING_WHITESPACE\n"
  "  ERROR_VARIABLE functional_test_error\n"
  "  ERROR_STRIP_TRAILING_WHITESPACE\n"
  "  RESULT_VARIABLE functional_test_result\n"
  "  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}\n"
  ")\n"
  "\n"
  "if(NOT functional_test_result EQUAL 0)\n"
  "  add_test(bitcoin.functional.DISCOVERY_FAILURE ${Python3_EXECUTABLE} ${CMAKE_CURRENT_BINARY_DIR}/functional/list_tests.py)\n"
  "else()\n"
  "  string(REPLACE \"\\n\" \";\" functional_test_lines \"\${functional_test_output}\")\n"
  "  foreach(test_script IN LISTS functional_test_lines)\n"
  "    if(test_script MATCHES \"^(.+\\\\.py)$\")\n"
  "      string(REGEX REPLACE \"\\\\.py$\" \"\" test_name \"\${test_script}\")\n"
  "      add_test(\"bitcoin.functional.\${test_name}\" ${Python3_EXECUTABLE} ${CMAKE_CURRENT_BINARY_DIR}/functional/\${test_script})\n"
  "      set_tests_properties(\"bitcoin.functional.\${test_name}\" PROPERTIES\n"
  "        LABELS \"functional\"\n"
  "        WORKING_DIRECTORY \"${CMAKE_CURRENT_BINARY_DIR}\"\n"
  "      )\n"
  "    endif()\n"
  "  endforeach()\n"
  "endif()\n"
)

file(GENERATE
  OUTPUT "${functional_include_file}"
  CONTENT "${functional_include_content}"
)

set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES "${functional_include_file}")
