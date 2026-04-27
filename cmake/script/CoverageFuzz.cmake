# Copyright (c) 2024-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

include(${CMAKE_CURRENT_LIST_DIR}/CoverageInclude.cmake)

if(NOT DEFINED FUZZ_CORPORA_DIR)
  set(FUZZ_CORPORA_DIR ${CMAKE_CURRENT_SOURCE_DIR}/qa-assets/fuzz_corpora)
endif()

set(fuzz_test_runner test/fuzz/test_runner.py ${FUZZ_CORPORA_DIR})
if(DEFINED JOBS)
  list(APPEND fuzz_test_runner -j ${JOBS})
endif()

set(raw_profile_dir ${CMAKE_CURRENT_LIST_DIR}/raw_profile_data)
file(REMOVE_RECURSE ${raw_profile_dir})
file(MAKE_DIRECTORY ${raw_profile_dir})
set(ENV{LLVM_PROFILE_FILE} "${raw_profile_dir}/%m_%p.profraw")

execute_process(
  COMMAND ${fuzz_test_runner} --loglevel DEBUG
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
if(NOT EXISTS ${CMAKE_CURRENT_LIST_DIR}/bin/fuzz)
  message(FATAL_ERROR "The fuzz binary was not found.")
endif()
set(demangler_args)
if(LLVM_CXXFILT_EXECUTABLE)
  list(APPEND demangler_args -Xdemangler ${LLVM_CXXFILT_EXECUTABLE})
endif()
execute_process(
  COMMAND ${LLVM_COV_EXECUTABLE} report
    --object ${CMAKE_CURRENT_LIST_DIR}/bin/fuzz
    ${demangler_args}
    --instr-profile=coverage.profdata
    --ignore-filename-regex=${COVERAGE_EXCLUDE_REGEX}
  OUTPUT_FILE coverage_percent.txt
  WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
  COMMAND_ERROR_IS_FATAL ANY
)
execute_process(
  COMMAND ${LLVM_COV_EXECUTABLE} show
    --object ${CMAKE_CURRENT_LIST_DIR}/bin/fuzz
    ${demangler_args}
    --instr-profile=coverage.profdata
    --ignore-filename-regex=${COVERAGE_EXCLUDE_REGEX}
    --format=html
    --show-instantiation-summary
    --show-line-counts-or-regions
    --show-expansions
    --output-dir=fuzz.coverage
    "--project-title=Bitcoin Core Fuzz Coverage Report"
  WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
  COMMAND_ERROR_IS_FATAL ANY
)
