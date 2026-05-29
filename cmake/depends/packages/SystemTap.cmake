# Copyright (c) 2026-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

set(systemtap_source_dir "${CMAKE_BINARY_DIR}/_deps/Source/systemtap")
set(systemtap_patch "${BITCOIN_DEPENDS_SOURCE_DIR}/patches/systemtap/remove_SDT_ASM_SECTION_AUTOGROUP_SUPPORT_check.patch")

ExternalProject_Add(systemtap
  URL "https://sourceware.org/ftp/systemtap/releases/systemtap-5.3.tar.gz"
  URL_HASH "SHA256=966a360fb73a4b65a8d0b51b389577b3c4f92a327e84aae58682103e8c65a69a"
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SOURCE_DIR "${systemtap_source_dir}"
  PATCH_COMMAND "${CMAKE_COMMAND}" -E chdir "<SOURCE_DIR>" "${Patch_EXECUTABLE}" -p1 -i "${systemtap_patch}"
  CONFIGURE_COMMAND ""
  BUILD_COMMAND ""
  INSTALL_COMMAND
    "${CMAKE_COMMAND}" -E make_directory "${BITCOIN_DEPENDS_PREFIX}/systemtap/include/sys"
    COMMAND "${CMAKE_COMMAND}" -E copy
      "${systemtap_source_dir}/includes/sys/sdt.h"
      "${BITCOIN_DEPENDS_PREFIX}/systemtap/include/sys/sdt.h"
)
ExternalProject_Add_StepTargets(systemtap download)
add_custom_target(systemtap-source DEPENDS systemtap-download)
add_dependencies(depends-download systemtap-source)
