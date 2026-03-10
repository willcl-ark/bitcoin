# Copyright (c) 2023-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

include_guard(GLOBAL)

function(setup_split_debug_script)
  if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    set(OBJCOPY ${CMAKE_OBJCOPY})
    set(STRIP ${CMAKE_STRIP})
    configure_file(
      contrib/devtools/split-debug.sh.in split-debug.sh
      FILE_PERMISSIONS OWNER_READ OWNER_EXECUTE
                       GROUP_READ GROUP_EXECUTE
                       WORLD_READ
      @ONLY
    )
  endif()
endfunction()

function(setup_macos_app_bundle)
  if(CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND TARGET bitcoin-qt)
    set(macos_app "Bitcoin-Qt.app")
    set(macos_unsigned_app "Bitcoin-Qt-unsigned.app")
    # Populate Contents subdirectory.
    configure_file(${PROJECT_SOURCE_DIR}/share/qt/Info.plist.in ${macos_unsigned_app}/Contents/Info.plist NO_SOURCE_PERMISSIONS)
    file(CONFIGURE OUTPUT ${macos_unsigned_app}/Contents/PkgInfo CONTENT "APPL????")
    # Populate Contents/Resources subdirectory.
    file(CONFIGURE OUTPUT ${macos_unsigned_app}/Contents/Resources/empty.lproj CONTENT "")
    configure_file(${PROJECT_SOURCE_DIR}/src/qt/res/icons/bitcoin.icns ${macos_unsigned_app}/Contents/Resources/bitcoin.icns NO_SOURCE_PERMISSIONS COPYONLY)
    file(CONFIGURE OUTPUT ${macos_unsigned_app}/Contents/Resources/Base.lproj/InfoPlist.strings
      CONTENT "{ CFBundleDisplayName = \"@CLIENT_NAME@\"; CFBundleName = \"@CLIENT_NAME@\"; }"
    )

    add_custom_command(
      OUTPUT ${PROJECT_BINARY_DIR}/${macos_unsigned_app}/Contents/MacOS/Bitcoin-Qt
      COMMAND ${CMAKE_COMMAND} --install ${PROJECT_BINARY_DIR} --config $<CONFIG> --component bitcoin_qt --prefix ${macos_unsigned_app}/Contents/MacOS
      COMMAND ${CMAKE_COMMAND} -E rename ${macos_unsigned_app}/Contents/MacOS/bin/$<TARGET_FILE_NAME:bitcoin-qt> ${macos_unsigned_app}/Contents/MacOS/Bitcoin-Qt
      COMMAND ${CMAKE_COMMAND} -E rm -rf ${macos_unsigned_app}/Contents/MacOS/bin
      COMMAND ${CMAKE_COMMAND} -E rm -rf ${macos_unsigned_app}/Contents/MacOS/share
      DEPENDS bitcoin-qt
      WORKING_DIRECTORY ${PROJECT_BINARY_DIR}
      VERBATIM
    )

    if(CMAKE_HOST_APPLE)
      add_custom_command(
        OUTPUT ${PROJECT_BINARY_DIR}/${macos_app}/Contents/Resources/qt.conf
        COMMAND $<TARGET_FILE:Python3::Interpreter> ${PROJECT_SOURCE_DIR}/contrib/macdeploy/macdeployqtplus ${macos_unsigned_app} -translations-dir=${QT_TRANSLATIONS_DIR}
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${macos_app}
        COMMAND ${CMAKE_COMMAND} -E rename dist/${macos_app} ${macos_app}
        DEPENDS ${PROJECT_BINARY_DIR}/${macos_unsigned_app}/Contents/MacOS/Bitcoin-Qt
        WORKING_DIRECTORY ${PROJECT_BINARY_DIR}
        VERBATIM
      )
    else()
      add_custom_command(
        OUTPUT ${PROJECT_BINARY_DIR}/${macos_app}/Contents/Resources/qt.conf
        COMMAND ${CMAKE_COMMAND} -E env OBJDUMP=${CMAKE_OBJDUMP} $<TARGET_FILE:Python3::Interpreter> ${PROJECT_SOURCE_DIR}/contrib/macdeploy/macdeployqtplus ${macos_unsigned_app} -translations-dir=${QT_TRANSLATIONS_DIR}
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${macos_app}
        COMMAND ${CMAKE_COMMAND} -E rename dist/${macos_app} ${macos_app}
        DEPENDS ${PROJECT_BINARY_DIR}/${macos_unsigned_app}/Contents/MacOS/Bitcoin-Qt
        WORKING_DIRECTORY ${PROJECT_BINARY_DIR}
        VERBATIM
      )
    endif()

    add_custom_target(bitcoin-qt-app ALL
      DEPENDS ${PROJECT_BINARY_DIR}/${macos_app}/Contents/Resources/qt.conf
    )
  endif()
endfunction()

function(setup_cpack)
  set(CPACK_PACKAGE_NAME "Bitcoin Core")
  set(CPACK_PACKAGE_VENDOR "Bitcoin Core")
  set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Bitcoin Core full node and wallet")
  set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
  set(CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/COPYING")
  set(CPACK_STRIP_FILES ON)
  if(NOT DEFINED CPACK_PACKAGE_FILE_NAME)
    set(CPACK_PACKAGE_FILE_NAME "${PROJECT_NAME}-${PROJECT_VERSION}")
  endif()
  set(CPACK_PROJECT_CONFIG_FILE "${PROJECT_SOURCE_DIR}/cmake/CPackConfig.cmake")

  if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(CPACK_GENERATOR "NSIS64")
  endif()

  set(cpack_components_all auxiliary)
  if(BUILD_BITCOIN_BIN)
    list(APPEND cpack_components_all bitcoin)
  endif()
  if(BUILD_DAEMON)
    list(APPEND cpack_components_all bitcoind)
  endif()
  if(BUILD_CLI)
    list(APPEND cpack_components_all bitcoin_cli)
  endif()
  if(BUILD_TX)
    list(APPEND cpack_components_all bitcoin_tx)
  endif()
  if(BUILD_UTIL)
    list(APPEND cpack_components_all bitcoin_util)
  endif()
  if(ENABLE_WALLET AND BUILD_WALLET_TOOL)
    list(APPEND cpack_components_all bitcoin_wallet)
  endif()
  if(BUILD_GUI)
    list(APPEND cpack_components_all bitcoin_qt)
  endif()
  set(CPACK_COMPONENTS_ALL ${cpack_components_all})

  include(CPackComponent)
  cpack_add_component(bitcoin
    DISPLAY_NAME "Bitcoin command dispatcher"
    DESCRIPTION "Wrapper executable for Bitcoin Core tools"
  )
  cpack_add_component(bitcoind
    DISPLAY_NAME "Bitcoin daemon"
    DESCRIPTION "Background process for running a Bitcoin node"
  )
  cpack_add_component(bitcoin_cli
    DISPLAY_NAME "Bitcoin CLI"
    DESCRIPTION "Command line interface for RPC interaction"
  )
  cpack_add_component(bitcoin_tx
    DISPLAY_NAME "Bitcoin transaction tool"
    DESCRIPTION "Transaction utility for creating and mutating transactions"
  )
  cpack_add_component(bitcoin_util
    DISPLAY_NAME "Bitcoin utility"
    DESCRIPTION "Additional Bitcoin Core command line utilities"
  )
  cpack_add_component(bitcoin_wallet
    DISPLAY_NAME "Bitcoin wallet tool"
    DESCRIPTION "Wallet management command line utility"
  )
  cpack_add_component(bitcoin_qt
    DISPLAY_NAME "Bitcoin Qt"
    DESCRIPTION "Graphical Bitcoin Core application"
  )
  cpack_add_component(auxiliary
    DISPLAY_NAME "Auxiliary files"
    DESCRIPTION "Example configuration and helper files"
  )
  include(CPack)
endfunction()
