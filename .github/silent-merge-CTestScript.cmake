# Copyright (c) 2026-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit.

cmake_host_system_information(RESULT host_name QUERY HOSTNAME)
include(ProcessorCount)
ProcessorCount(nproc)
if(nproc EQUAL 0)
  set(nproc 1)
endif()

if(NOT CTEST_SITE)
  set(CTEST_SITE "${host_name}")
endif()
if(NOT CTEST_BUILD_NAME)
  set(CTEST_BUILD_NAME "silent-merge-check")
endif()
if(NOT CTEST_SOURCE_DIRECTORY)
  get_filename_component(CTEST_SOURCE_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()
if(NOT CTEST_BINARY_DIRECTORY)
  set(CTEST_BINARY_DIRECTORY "${CTEST_SOURCE_DIRECTORY}/ci_build")
endif()

set(CTEST_CMAKE_GENERATOR "Ninja")
set(CTEST_CONFIGURE_COMMAND "${CMAKE_COMMAND} -S \"${CTEST_SOURCE_DIRECTORY}\" --preset=ci-test-each-commit")
set(CTEST_NOTES_FILES "${CMAKE_CURRENT_LIST_FILE}")

function(submit_parts)
  ctest_submit(PARTS ${ARGV} RETURN_VALUE submit_result)
  if(submit_result)
    string(REPLACE ";" " " parts "${ARGV}")
    message(WARNING "CTest submit failed for ${parts}: ${submit_result}")
  endif()
endfunction()

function(fail_after_done failure_message)
  submit_parts("Done")
  message(FATAL_ERROR "${failure_message}")
endfunction()

ctest_start("Experimental")
submit_parts("Notes")
submit_parts("Update")

ctest_configure(
  BUILD "${CTEST_BINARY_DIRECTORY}"
  SOURCE "${CTEST_SOURCE_DIRECTORY}"
  RETURN_VALUE configure_result
)
submit_parts("Configure")
if(configure_result)
  fail_after_done("Configure failed with exit code ${configure_result}")
endif()

ctest_build(
  BUILD "${CTEST_BINARY_DIRECTORY}"
  PARALLEL_LEVEL "${nproc}"
  NUMBER_ERRORS build_errors
  RETURN_VALUE build_result
)
submit_parts("Build")
if(build_result OR build_errors)
  fail_after_done("Build failed with exit code ${build_result} and ${build_errors} error(s)")
endif()

ctest_test(
  BUILD "${CTEST_BINARY_DIRECTORY}"
  PARALLEL_LEVEL "${nproc}"
  EXCLUDE "interface_ipc"
  RETURN_VALUE test_result
)
submit_parts("Test")
submit_parts("Done")
if(test_result)
  message(FATAL_ERROR "Tests failed with exit code ${test_result}")
endif()
