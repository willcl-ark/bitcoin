# Copyright (c) 2023-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

macro(fatal_error)
  message(FATAL_ERROR "\n"
    "Usage:\n"
    "  cmake -D BUILD_INFO_HEADER_PATH=<path> [-D BUILD_GIT_TAG=<tag> | -D BUILD_GIT_COMMIT=<commit>] -P ${CMAKE_CURRENT_LIST_FILE}\n"
    "All specified paths must be absolute ones.\n"
  )
endmacro()

if(DEFINED BUILD_INFO_HEADER_PATH AND IS_ABSOLUTE "${BUILD_INFO_HEADER_PATH}")
  if(EXISTS "${BUILD_INFO_HEADER_PATH}")
    file(STRINGS ${BUILD_INFO_HEADER_PATH} INFO LIMIT_COUNT 1)
  endif()
else()
  fatal_error()
endif()

if(DEFINED BUILD_GIT_TAG AND NOT BUILD_GIT_TAG STREQUAL "")
  set(NEWINFO "#define BUILD_GIT_TAG \"${BUILD_GIT_TAG}\"")
elseif(DEFINED BUILD_GIT_COMMIT AND NOT BUILD_GIT_COMMIT STREQUAL "")
  set(NEWINFO "#define BUILD_GIT_COMMIT \"${BUILD_GIT_COMMIT}\"")
else()
  set(NEWINFO "// No build information available")
endif()

# Only update the header if necessary.
if(NOT "${INFO}" STREQUAL "${NEWINFO}")
  file(WRITE ${BUILD_INFO_HEADER_PATH} "${NEWINFO}\n")
endif()
