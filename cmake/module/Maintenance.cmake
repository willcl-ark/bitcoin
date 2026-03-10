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

function(add_macos_deploy_target)
  if(CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND TARGET bitcoin-qt)
    set(macos_app "Bitcoin-Qt.app")
    # Populate Contents subdirectory.
    configure_file(${PROJECT_SOURCE_DIR}/share/qt/Info.plist.in ${macos_app}/Contents/Info.plist NO_SOURCE_PERMISSIONS)
    file(CONFIGURE OUTPUT ${macos_app}/Contents/PkgInfo CONTENT "APPL????")
    # Populate Contents/Resources subdirectory.
    file(CONFIGURE OUTPUT ${macos_app}/Contents/Resources/empty.lproj CONTENT "")
    configure_file(${PROJECT_SOURCE_DIR}/src/qt/res/icons/bitcoin.icns ${macos_app}/Contents/Resources/bitcoin.icns NO_SOURCE_PERMISSIONS COPYONLY)
    file(CONFIGURE OUTPUT ${macos_app}/Contents/Resources/Base.lproj/InfoPlist.strings
      CONTENT "{ CFBundleDisplayName = \"@CLIENT_NAME@\"; CFBundleName = \"@CLIENT_NAME@\"; }"
    )

    add_custom_command(
      OUTPUT ${PROJECT_BINARY_DIR}/${macos_app}/Contents/MacOS/Bitcoin-Qt
      COMMAND ${CMAKE_COMMAND} --install ${PROJECT_BINARY_DIR} --config $<CONFIG> --component bitcoin-qt --prefix ${macos_app}/Contents/MacOS --strip
      COMMAND ${CMAKE_COMMAND} -E rename ${macos_app}/Contents/MacOS/bin/$<TARGET_FILE_NAME:bitcoin-qt> ${macos_app}/Contents/MacOS/Bitcoin-Qt
      COMMAND ${CMAKE_COMMAND} -E rm -rf ${macos_app}/Contents/MacOS/bin
      COMMAND ${CMAKE_COMMAND} -E rm -rf ${macos_app}/Contents/MacOS/share
      VERBATIM
    )

    set(macos_zip "bitcoin-macos-app")
    if(CMAKE_HOST_APPLE)
      add_custom_command(
        OUTPUT ${PROJECT_BINARY_DIR}/${macos_zip}.zip
        COMMAND Python3::Interpreter ${PROJECT_SOURCE_DIR}/contrib/macdeploy/macdeployqtplus ${macos_app} -translations-dir=${QT_TRANSLATIONS_DIR} -zip=${macos_zip}
        DEPENDS ${PROJECT_BINARY_DIR}/${macos_app}/Contents/MacOS/Bitcoin-Qt
        VERBATIM
      )
      add_custom_target(deploydir
        DEPENDS ${PROJECT_BINARY_DIR}/${macos_zip}.zip
      )
      add_custom_target(deploy
        DEPENDS ${PROJECT_BINARY_DIR}/${macos_zip}.zip
      )
    else()
      add_custom_command(
        OUTPUT ${PROJECT_BINARY_DIR}/dist/${macos_app}/Contents/MacOS/Bitcoin-Qt
        COMMAND ${CMAKE_COMMAND} -E env OBJDUMP=${CMAKE_OBJDUMP} $<TARGET_FILE:Python3::Interpreter> ${PROJECT_SOURCE_DIR}/contrib/macdeploy/macdeployqtplus ${macos_app} -translations-dir=${QT_TRANSLATIONS_DIR}
        DEPENDS ${PROJECT_BINARY_DIR}/${macos_app}/Contents/MacOS/Bitcoin-Qt
        VERBATIM
      )
      add_custom_target(deploydir
        DEPENDS ${PROJECT_BINARY_DIR}/dist/${macos_app}/Contents/MacOS/Bitcoin-Qt
      )

      find_program(ZIP_EXECUTABLE zip)
      if(NOT ZIP_EXECUTABLE)
        add_custom_target(deploy
          COMMAND ${CMAKE_COMMAND} -E echo "Error: ZIP not found"
        )
      else()
        add_custom_command(
          OUTPUT ${PROJECT_BINARY_DIR}/dist/${macos_zip}.zip
          WORKING_DIRECTORY dist
          COMMAND ${PROJECT_SOURCE_DIR}/cmake/script/macos_zip.sh ${ZIP_EXECUTABLE} ${macos_zip}.zip
          VERBATIM
        )
        add_custom_target(deploy
          DEPENDS ${PROJECT_BINARY_DIR}/dist/${macos_zip}.zip
        )
      endif()
    endif()
    add_dependencies(deploydir bitcoin-qt)
    add_dependencies(deploy deploydir)
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
