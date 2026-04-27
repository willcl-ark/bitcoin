# Copyright (c) 2024-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

include(${CMAKE_CURRENT_LIST_DIR}/CoverageInclude.cmake)

set(functional_test_runner test/functional/test_runner.py)
if(EXTENDED_FUNCTIONAL_TESTS)
  list(APPEND functional_test_runner --extended)
endif()
if(DEFINED JOBS)
  list(APPEND CMAKE_CTEST_COMMAND -j ${JOBS})
  list(APPEND functional_test_runner -j ${JOBS})
endif()

set(raw_profile_dir ${CMAKE_CURRENT_LIST_DIR}/raw_profile_data)
file(REMOVE_RECURSE ${raw_profile_dir})
file(MAKE_DIRECTORY ${raw_profile_dir})
set(ENV{LLVM_PROFILE_FILE} "${raw_profile_dir}/%m_%p.profraw")

execute_process(
  COMMAND ${CMAKE_CTEST_COMMAND} --build-config Coverage
  WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
  COMMAND_ERROR_IS_FATAL ANY
)
execute_process(
  COMMAND ${functional_test_runner}
  WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
  COMMAND_ERROR_IS_FATAL ANY
)

file(GLOB_RECURSE raw_profiles ${raw_profile_dir}/*.profraw)
if(NOT raw_profiles)
  message(FATAL_ERROR "No LLVM coverage profiles were generated.")
endif()
set(raw_profile_list ${raw_profile_dir}/profiles.txt)
file(WRITE ${raw_profile_list} "")
foreach(raw_profile IN LISTS raw_profiles)
  file(APPEND ${raw_profile_list} "${raw_profile}\n")
endforeach()

execute_process(
  COMMAND ${LLVM_PROFDATA_EXECUTABLE} merge -sparse -f ${raw_profile_list} -o coverage.profdata
  WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
  COMMAND_ERROR_IS_FATAL ANY
)

set(coverage_objects)
foreach(coverage_object IN ITEMS
  bin/test_bitcoin
  bin/test_kernel
  bin/test_bitcoin-qt
  bin/bitcoind
  bin/bitcoin
  bin/bitcoin-node
  bin/bitcoin-cli
  bin/bitcoin-tx
  bin/bitcoin-util
  bin/bitcoin-wallet
  bin/bitcoin-chainstate
)
  if(EXISTS ${CMAKE_CURRENT_LIST_DIR}/${coverage_object})
    list(APPEND coverage_objects --object ${CMAKE_CURRENT_LIST_DIR}/${coverage_object})
  endif()
endforeach()
if(NOT coverage_objects)
  message(FATAL_ERROR "No coverage objects were found.")
endif()

set(demangler_args)
if(LLVM_CXXFILT_EXECUTABLE)
  list(APPEND demangler_args -Xdemangler ${LLVM_CXXFILT_EXECUTABLE})
endif()

execute_process(
  COMMAND ${LLVM_COV_EXECUTABLE} report
    ${coverage_objects}
    ${demangler_args}
    --instr-profile=coverage.profdata
    --ignore-filename-regex=${COVERAGE_EXCLUDE_REGEX}
  OUTPUT_FILE coverage_percent.txt
  WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
  COMMAND_ERROR_IS_FATAL ANY
)
execute_process(
  COMMAND ${LLVM_COV_EXECUTABLE} show
    ${coverage_objects}
    ${demangler_args}
    --instr-profile=coverage.profdata
    --ignore-filename-regex=${COVERAGE_EXCLUDE_REGEX}
    --format=html
    --show-instantiation-summary
    --show-line-counts-or-regions
    --show-expansions
    --output-dir=coverage_report
    "--project-title=Bitcoin Core Coverage Report"
  WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
  COMMAND_ERROR_IS_FATAL ANY
)
