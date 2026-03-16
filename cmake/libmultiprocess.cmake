# Copyright (c) 2025-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

function(add_libmultiprocess subdir)
  set(BUILD_TESTING "${BUILD_TESTS}")
  add_subdirectory(${subdir} EXCLUDE_FROM_ALL)
  target_link_libraries(multiprocess PUBLIC $<BUILD_INTERFACE:core_interface>)
  target_link_libraries(mputil PUBLIC $<BUILD_INTERFACE:core_interface>)
  target_link_libraries(mpgen PUBLIC $<BUILD_INTERFACE:core_interface>)
endfunction()
