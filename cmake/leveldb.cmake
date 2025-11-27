# Copyright (c) 2023-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

# This file is part of the transition from Autotools to CMake. Once CMake
# support has been merged we should switch to using the upstream CMake
# buildsystem.

# Apply UB fix patch only in CI builds (not tidy jobs)
# compact->outputs[i].file_size is uninitialized memory, so reading it is UB.
# The statistic bytes_written is only used for logging, which is disabled in CI.
# See https://github.com/bitcoin/bitcoin/pull/28359#issuecomment-1698694748
if(CI_BUILD AND NOT ENABLE_CLANG_TIDY)
  set(LEVELDB_PATCH_FILE "${CMAKE_CURRENT_LIST_DIR}/patches/leveldb-ub-fix.patch")

  # Check if patch is already applied (reverse dry-run succeeds if applied)
  execute_process(
    COMMAND patch -p1 --dry-run --reverse --input=${LEVELDB_PATCH_FILE}
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    RESULT_VARIABLE PATCH_ALREADY_APPLIED
    OUTPUT_QUIET
    ERROR_QUIET
  )

  # Apply patch if not already applied (reverse dry-run failed)
  if(NOT PATCH_ALREADY_APPLIED EQUAL 0)
    message(STATUS "Applying leveldb UB fix patch for CI build...")
    execute_process(
      COMMAND patch -p1 --forward --input=${LEVELDB_PATCH_FILE}
      WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
      RESULT_VARIABLE PATCH_RESULT
      OUTPUT_VARIABLE PATCH_OUTPUT
      ERROR_VARIABLE PATCH_ERROR
    )

    if(NOT PATCH_RESULT EQUAL 0)
      message(FATAL_ERROR "Failed to apply leveldb patch:\n${PATCH_OUTPUT}\n${PATCH_ERROR}")
    endif()
  endif()
endif()

include(CheckCXXSymbolExists)
check_cxx_symbol_exists(F_FULLFSYNC "fcntl.h" HAVE_FULLFSYNC)

add_library(leveldb STATIC EXCLUDE_FROM_ALL
  ${PROJECT_SOURCE_DIR}/src/leveldb/db/builder.cc
  ${PROJECT_SOURCE_DIR}/src/leveldb/db/c.cc
  ${PROJECT_SOURCE_DIR}/src/leveldb/db/db_impl.cc
  ${PROJECT_SOURCE_DIR}/src/leveldb/db/db_iter.cc
  ${PROJECT_SOURCE_DIR}/src/leveldb/db/dbformat.cc
  ${PROJECT_SOURCE_DIR}/src/leveldb/db/dumpfile.cc
  ${PROJECT_SOURCE_DIR}/src/leveldb/db/filename.cc
  ${PROJECT_SOURCE_DIR}/src/leveldb/db/log_reader.cc
  ${PROJECT_SOURCE_DIR}/src/leveldb/db/log_writer.cc
  ${PROJECT_SOURCE_DIR}/src/leveldb/db/memtable.cc
  ${PROJECT_SOURCE_DIR}/src/leveldb/db/repair.cc
  ${PROJECT_SOURCE_DIR}/src/leveldb/db/table_cache.cc
  ${PROJECT_SOURCE_DIR}/src/leveldb/db/version_edit.cc
  ${PROJECT_SOURCE_DIR}/src/leveldb/db/version_set.cc
  ${PROJECT_SOURCE_DIR}/src/leveldb/db/write_batch.cc
  ${PROJECT_SOURCE_DIR}/src/leveldb/table/block.cc
  ${PROJECT_SOURCE_DIR}/src/leveldb/table/block_builder.cc
  ${PROJECT_SOURCE_DIR}/src/leveldb/table/filter_block.cc
  ${PROJECT_SOURCE_DIR}/src/leveldb/table/format.cc
  ${PROJECT_SOURCE_DIR}/src/leveldb/table/iterator.cc
  ${PROJECT_SOURCE_DIR}/src/leveldb/table/merger.cc
  ${PROJECT_SOURCE_DIR}/src/leveldb/table/table.cc
  ${PROJECT_SOURCE_DIR}/src/leveldb/table/table_builder.cc
  ${PROJECT_SOURCE_DIR}/src/leveldb/table/two_level_iterator.cc
  ${PROJECT_SOURCE_DIR}/src/leveldb/util/arena.cc
  ${PROJECT_SOURCE_DIR}/src/leveldb/util/bloom.cc
  ${PROJECT_SOURCE_DIR}/src/leveldb/util/cache.cc
  ${PROJECT_SOURCE_DIR}/src/leveldb/util/coding.cc
  ${PROJECT_SOURCE_DIR}/src/leveldb/util/comparator.cc
  ${PROJECT_SOURCE_DIR}/src/leveldb/util/crc32c.cc
  ${PROJECT_SOURCE_DIR}/src/leveldb/util/env.cc
  $<$<NOT:$<BOOL:${WIN32}>>:${PROJECT_SOURCE_DIR}/src/leveldb/util/env_posix.cc>
  $<$<BOOL:${WIN32}>:${PROJECT_SOURCE_DIR}/src/leveldb/util/env_windows.cc>
  ${PROJECT_SOURCE_DIR}/src/leveldb/util/filter_policy.cc
  ${PROJECT_SOURCE_DIR}/src/leveldb/util/hash.cc
  ${PROJECT_SOURCE_DIR}/src/leveldb/util/histogram.cc
  ${PROJECT_SOURCE_DIR}/src/leveldb/util/logging.cc
  ${PROJECT_SOURCE_DIR}/src/leveldb/util/options.cc
  ${PROJECT_SOURCE_DIR}/src/leveldb/util/status.cc
  ${PROJECT_SOURCE_DIR}/src/leveldb/helpers/memenv/memenv.cc
)

target_compile_definitions(leveldb
  PRIVATE
    HAVE_SNAPPY=0
    HAVE_CRC32C=1
    HAVE_FDATASYNC=$<BOOL:${HAVE_FDATASYNC}>
    HAVE_FULLFSYNC=$<BOOL:${HAVE_FULLFSYNC}>
    HAVE_O_CLOEXEC=$<BOOL:${HAVE_O_CLOEXEC}>
    FALLTHROUGH_INTENDED=[[fallthrough]]
    $<$<NOT:$<BOOL:${WIN32}>>:LEVELDB_PLATFORM_POSIX>
    $<$<BOOL:${WIN32}>:LEVELDB_PLATFORM_WINDOWS>
    $<$<BOOL:${WIN32}>:_UNICODE;UNICODE>
)
if(MINGW)
  target_compile_definitions(leveldb
    PRIVATE
      __USE_MINGW_ANSI_STDIO=1
  )
endif()

target_include_directories(leveldb
  PRIVATE
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src/leveldb>
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src/leveldb/include>
)

add_library(nowarn_leveldb_interface INTERFACE)
if(MSVC)
  target_compile_options(nowarn_leveldb_interface INTERFACE
    /wd4722
  )
  target_compile_definitions(nowarn_leveldb_interface INTERFACE
    _CRT_NONSTDC_NO_WARNINGS
  )
else()
  try_append_cxx_flags("-Wconditional-uninitialized" TARGET nowarn_leveldb_interface SKIP_LINK
    IF_CHECK_PASSED "-Wno-conditional-uninitialized"
  )
endif()

target_link_libraries(leveldb PRIVATE
  core_interface
  nowarn_leveldb_interface
  crc32c
)

set_target_properties(leveldb PROPERTIES
  EXPORT_COMPILE_COMMANDS OFF
  CXX_CLANG_TIDY ""
)
