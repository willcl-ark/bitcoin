# Copyright (c) 2026-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

function(bitcoin_depends_add_systemtap)
  if(TARGET USDT::headers)
    return()
  endif()

  set(systemtap_include_dir "${BITCOIN_DEPENDS_PREFIX}/systemtap/include")
  set(systemtap_source_dir "${BITCOIN_DEPENDS_BUILD_DIR}/bitcoin_depends_systemtap/src/bitcoin_depends_systemtap")
  set(systemtap_install_script "${PROJECT_BINARY_DIR}/_depends/systemtap-install.cmake")
  file(MAKE_DIRECTORY "${systemtap_include_dir}")
  file(WRITE "${systemtap_install_script}" "
file(MAKE_DIRECTORY \"${systemtap_include_dir}/sys\")
file(COPY \"${systemtap_source_dir}/includes/sys/sdt.h\" DESTINATION \"${systemtap_include_dir}/sys\")
")

  ExternalProject_Add(bitcoin_depends_systemtap
    URL "https://sourceware.org/ftp/systemtap/releases/systemtap-5.3.tar.gz"
    URL_HASH "SHA256=966a360fb73a4b65a8d0b51b389577b3c4f92a327e84aae58682103e8c65a69a"
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE
    DOWNLOAD_DIR "${BITCOIN_DEPENDS_DOWNLOAD_DIR}"
    PREFIX "${BITCOIN_DEPENDS_BUILD_DIR}/bitcoin_depends_systemtap"
    PATCH_COMMAND patch -p1 -i "${PROJECT_SOURCE_DIR}/depends/patches/systemtap/remove_SDT_ASM_SECTION_AUTOGROUP_SUPPORT_check.patch"
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ""
    INSTALL_COMMAND "${CMAKE_COMMAND}" -P "${systemtap_install_script}"
    BUILD_BYPRODUCTS "${systemtap_include_dir}/sys/sdt.h"
  )
  ExternalProject_Add_StepTargets(bitcoin_depends_systemtap download)

  add_dependencies(bitcoin-depends bitcoin_depends_systemtap)
  add_dependencies(bitcoin-depends-download bitcoin_depends_systemtap-download)

  add_library(bitcoin_depends_usdt_headers INTERFACE)
  add_library(USDT::headers ALIAS bitcoin_depends_usdt_headers)
  target_include_directories(bitcoin_depends_usdt_headers SYSTEM INTERFACE
    "${systemtap_include_dir}"
  )
  add_dependencies(bitcoin_depends_usdt_headers bitcoin_depends_systemtap)
  set(ENABLE_TRACING TRUE PARENT_SCOPE)
endfunction()
