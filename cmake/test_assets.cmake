# Copyright (c) 2024-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

# Download test assets for CI builds
# This file is only included when CI_BUILD=ON

include_guard()

if(NOT CI_BUILD)
  return()
endif()

include(FetchContent)

# 1. Fuzz test corpora from qa-assets repository
if(BUILD_FUZZ_BINARY)
  message(STATUS "Fetching qa-assets for fuzz test corpora...")

  FetchContent_Declare(
    qa_assets
    GIT_REPOSITORY https://github.com/bitcoin-core/qa-assets.git
    GIT_SHALLOW TRUE
    GIT_TAG main
  )

  FetchContent_MakeAvailable(qa_assets)

  # Set the directory that fuzz tests will use
  set(FUZZ_CORPUS_DIR "${qa_assets_SOURCE_DIR}/fuzz_corpora" CACHE PATH "Fuzz test corpus directory" FORCE)

  message(STATUS "Fuzz corpus directory: ${FUZZ_CORPUS_DIR}")

  # Log the qa-assets commit being used
  execute_process(
    COMMAND git log -1 --oneline
    WORKING_DIRECTORY "${qa_assets_SOURCE_DIR}"
    OUTPUT_VARIABLE QA_ASSETS_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  message(STATUS "Using qa-assets repo from commit: ${QA_ASSETS_COMMIT}")
endif()

# 2. Unit test data (single JSON file)
if(BUILD_TESTS)
  set(UNIT_TEST_DATA_DIR "${CMAKE_BINARY_DIR}/_deps/unit_test_data")
  set(UNIT_TEST_DATA_FILE "${UNIT_TEST_DATA_DIR}/script_assets_test.json")

  if(NOT EXISTS "${UNIT_TEST_DATA_FILE}")
    message(STATUS "Downloading unit test data...")
    file(MAKE_DIRECTORY "${UNIT_TEST_DATA_DIR}")

    file(DOWNLOAD
      "https://github.com/bitcoin-core/qa-assets/raw/main/unit_test_data/script_assets_test.json"
      "${UNIT_TEST_DATA_FILE}"
      SHOW_PROGRESS
      STATUS DOWNLOAD_STATUS
    )

    list(GET DOWNLOAD_STATUS 0 STATUS_CODE)
    list(GET DOWNLOAD_STATUS 1 ERROR_MESSAGE)

    if(NOT STATUS_CODE EQUAL 0)
      message(FATAL_ERROR "Failed to download unit test data: ${ERROR_MESSAGE}")
    endif()

    message(STATUS "Unit test data downloaded successfully")
  else()
    message(STATUS "Unit test data already exists: ${UNIT_TEST_DATA_FILE}")
  endif()

  # Export the directory for use in tests
  set(DIR_UNIT_TEST_DATA "${UNIT_TEST_DATA_DIR}" CACHE PATH "Unit test data directory" FORCE)
endif()

# 3. Previous releases download
# Note: This requires the Python script and is more complex to download
# For now, we'll create a custom target that can be run if needed
# The actual download is controlled by DOWNLOAD_PREVIOUS_RELEASES env var
if(DEFINED ENV{DOWNLOAD_PREVIOUS_RELEASES} AND "$ENV{DOWNLOAD_PREVIOUS_RELEASES}" STREQUAL "true")
  find_package(Python3 COMPONENTS Interpreter REQUIRED)

  set(PREVIOUS_RELEASES_DIR "${CMAKE_BINARY_DIR}/prev_releases" CACHE PATH "Previous releases directory")

  # Create directory if it doesn't exist
  file(MAKE_DIRECTORY "${PREVIOUS_RELEASES_DIR}")

  # Create a custom target that downloads previous releases
  # This runs at build time, not configure time
  add_custom_target(download_previous_releases
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/test/get_previous_releases.py"
            --target-dir "${PREVIOUS_RELEASES_DIR}"
    COMMENT "Downloading previous Bitcoin releases..."
    BYPRODUCTS "${PREVIOUS_RELEASES_DIR}"
  )

  message(STATUS "Previous releases will be downloaded to: ${PREVIOUS_RELEASES_DIR}")
  message(STATUS "Run 'cmake --build . --target download_previous_releases' to download")
else()
  message(STATUS "Skipping previous releases download (DOWNLOAD_PREVIOUS_RELEASES not set)")
endif()
