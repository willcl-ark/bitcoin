# Copyright (c) 2024-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

# Download test assets for CI builds
# This file is only included when CI_BUILD=ON

include_guard()

if(NOT CI_BUILD)
  return()
endif()

# 1. Fuzz test corpora from qa-assets repository
# Create a custom target instead of downloading at configure time
if(BUILD_FUZZ_BINARY)
  # Use DIR_QA_ASSETS env var if set (for CI caching), otherwise use build dir
  if(DEFINED ENV{DIR_QA_ASSETS})
    set(QA_ASSETS_DIR "$ENV{DIR_QA_ASSETS}")
  else()
    set(QA_ASSETS_DIR "${CMAKE_BINARY_DIR}/_deps/qa-assets")
  endif()
  set(FUZZ_CORPUS_DIR "${QA_ASSETS_DIR}/fuzz_corpora" CACHE PATH "Fuzz test corpus directory" FORCE)

  add_custom_target(download-qa-assets
    COMMAND ${CMAKE_COMMAND} -E echo "Downloading qa-assets repository..."
    COMMAND ${CMAKE_COMMAND} -E make_directory "${QA_ASSETS_DIR}"
    COMMAND git clone --depth 1 https://github.com/bitcoin-core/qa-assets.git "${QA_ASSETS_DIR}" ||
            (cd "${QA_ASSETS_DIR}" && git pull)
    COMMAND git -C "${QA_ASSETS_DIR}" log -1 --oneline
    COMMENT "Downloading fuzz test corpora from qa-assets"
    BYPRODUCTS "${FUZZ_CORPUS_DIR}"
  )

  message(STATUS "Fuzz corpus will be available at: ${FUZZ_CORPUS_DIR}")
  message(STATUS "Run 'cmake --build . --target download-qa-assets' to download")
endif()

# 2. Unit test data (single JSON file)
if(BUILD_TESTS)
  set(UNIT_TEST_DATA_DIR "${CMAKE_BINARY_DIR}/_deps/unit_test_data")
  set(UNIT_TEST_DATA_FILE "${UNIT_TEST_DATA_DIR}/script_assets_test.json")
  set(DIR_UNIT_TEST_DATA "${UNIT_TEST_DATA_DIR}" CACHE PATH "Unit test data directory" FORCE)

  add_custom_target(download-unit-test-data
    COMMAND ${CMAKE_COMMAND} -E make_directory "${UNIT_TEST_DATA_DIR}"
    COMMAND ${CMAKE_COMMAND} -E echo "Downloading unit test data..."
    COMMAND curl -L -o "${UNIT_TEST_DATA_FILE}"
            "https://github.com/bitcoin-core/qa-assets/raw/main/unit_test_data/script_assets_test.json"
    COMMENT "Downloading unit test data"
    BYPRODUCTS "${UNIT_TEST_DATA_FILE}"
  )

  message(STATUS "Unit test data will be available at: ${UNIT_TEST_DATA_FILE}")
  message(STATUS "Run 'cmake --build . --target download-unit-test-data' to download")
endif()

# 3. Previous releases download
if(DEFINED ENV{DOWNLOAD_PREVIOUS_RELEASES} AND "$ENV{DOWNLOAD_PREVIOUS_RELEASES}" STREQUAL "true")
  find_package(Python3 COMPONENTS Interpreter REQUIRED)

  set(PREVIOUS_RELEASES_DIR "${CMAKE_BINARY_DIR}/prev_releases" CACHE PATH "Previous releases directory")
  file(MAKE_DIRECTORY "${PREVIOUS_RELEASES_DIR}")

  add_custom_target(download-previous-releases
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/test/get_previous_releases.py"
            --target-dir "${PREVIOUS_RELEASES_DIR}"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Downloading previous Bitcoin releases..."
    BYPRODUCTS "${PREVIOUS_RELEASES_DIR}"
  )

  message(STATUS "Previous releases will be available at: ${PREVIOUS_RELEASES_DIR}")
  message(STATUS "Run 'cmake --build . --target download-previous-releases' to download")
endif()

# Create a meta-target that downloads all test assets
if(BUILD_FUZZ_BINARY OR BUILD_TESTS)
  set(ALL_ASSET_TARGETS "")

  if(BUILD_FUZZ_BINARY)
    list(APPEND ALL_ASSET_TARGETS download-qa-assets)
  endif()

  if(BUILD_TESTS)
    list(APPEND ALL_ASSET_TARGETS download-unit-test-data)
  endif()

  if(DEFINED ENV{DOWNLOAD_PREVIOUS_RELEASES} AND "$ENV{DOWNLOAD_PREVIOUS_RELEASES}" STREQUAL "true")
    list(APPEND ALL_ASSET_TARGETS download-previous-releases)
  endif()

  add_custom_target(download-test-assets
    DEPENDS ${ALL_ASSET_TARGETS}
    COMMENT "Downloading all test assets"
  )

  message(STATUS "Run 'cmake --build . --target download-test-assets' to download all test assets")
endif()
