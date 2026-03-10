# Copyright (c) 2026-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

if(CPACK_GENERATOR STREQUAL "NSIS64")
  set(cpack_nsis_resource_dir "${CMAKE_CURRENT_LIST_DIR}/../share/pixmaps")
  set(CPACK_PACKAGE_INSTALL_DIRECTORY "Bitcoin")
  set(CPACK_NSIS_COMPRESSOR "/SOLID lzma")
  set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
  if(CPACK_COMPONENTS_ALL MATCHES "(^|;)bitcoin_qt(;|$)")
    set(CPACK_NSIS_INSTALLED_ICON_NAME "bin\\bitcoin-qt.exe")
  endif()
  set(CPACK_NSIS_MUI_ICON "${cpack_nsis_resource_dir}/bitcoin.ico")
  set(CPACK_NSIS_MUI_UNIICON "${cpack_nsis_resource_dir}/bitcoin.ico")
  set(CPACK_NSIS_MUI_WELCOMEFINISHPAGE_BITMAP "${cpack_nsis_resource_dir}/nsis-wizard.bmp")
  set(CPACK_NSIS_MUI_UNWELCOMEFINISHPAGE_BITMAP "${cpack_nsis_resource_dir}/nsis-wizard.bmp")
  set(CPACK_NSIS_MUI_HEADERIMAGE "${cpack_nsis_resource_dir}/nsis-header.bmp")
  set(CPACK_NSIS_DISPLAY_NAME "Bitcoin Core ${CPACK_PACKAGE_VERSION}")
  set(CPACK_NSIS_PACKAGE_NAME "Bitcoin Core")
  set(CPACK_NSIS_URL_INFO_ABOUT "https://bitcoincore.org")
  set(CPACK_NSIS_EXECUTABLES_DIRECTORY "bin")
  set(CPACK_PACKAGE_EXECUTABLES "bitcoin-qt" "Bitcoin Core")
  set(CPACK_NSIS_CREATE_ICONS_EXTRA [=[
CreateShortCut "$SMPROGRAMS\$STARTMENU_FOLDER\Bitcoin Core (testnet).lnk" "$INSTDIR\bin\bitcoin-qt.exe" "-testnet" "$INSTDIR\bin\bitcoin-qt.exe" 1
CreateShortCut "$SMPROGRAMS\$STARTMENU_FOLDER\Bitcoin Core (test signet).lnk" "$INSTDIR\bin\bitcoin-qt.exe" "-signet" "$INSTDIR\bin\bitcoin-qt.exe" 2
CreateShortCut "$SMPROGRAMS\$STARTMENU_FOLDER\Bitcoin Core (testnet4).lnk" "$INSTDIR\bin\bitcoin-qt.exe" "-testnet4" "$INSTDIR\bin\bitcoin-qt.exe" 3
]=])
  set(CPACK_NSIS_DELETE_ICONS_EXTRA [=[
Delete "$SMPROGRAMS\$STARTMENU_FOLDER\Bitcoin Core (testnet).lnk"
Delete "$SMPROGRAMS\$STARTMENU_FOLDER\Bitcoin Core (test signet).lnk"
Delete "$SMPROGRAMS\$STARTMENU_FOLDER\Bitcoin Core (testnet4).lnk"
]=])
  set(CPACK_NSIS_EXTRA_INSTALL_COMMANDS [=[
WriteRegStr HKCR "bitcoin" "URL Protocol" ""
WriteRegStr HKCR "bitcoin" "" "URL:Bitcoin"
WriteRegStr HKCR "bitcoin\DefaultIcon" "" "$INSTDIR\bin\bitcoin-qt.exe"
WriteRegStr HKCR "bitcoin\shell\open\command" "" '"$INSTDIR\bin\bitcoin-qt.exe" "%1"'
DeleteRegValue HKCU "SOFTWARE\Bitcoin Core (64-bit)\Components" "Main"
DeleteRegKey HKCU "SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Bitcoin Core (64-bit)"
Delete /REBOOTOK "$SMPROGRAMS\$STARTMENU_FOLDER\Uninstall Bitcoin Core (64-bit).lnk"
Delete /REBOOTOK "$SMPROGRAMS\$STARTMENU_FOLDER\Bitcoin Core (64-bit).lnk"
DeleteRegValue HKCU "SOFTWARE\Bitcoin Core (64-bit)" "StartMenuGroup"
DeleteRegValue HKCU "SOFTWARE\Bitcoin Core (64-bit)" "Path"
DeleteRegKey /IfEmpty HKCU "SOFTWARE\Bitcoin Core (64-bit)\Components"
DeleteRegKey /IfEmpty HKCU "SOFTWARE\Bitcoin Core (64-bit)"
Delete /REBOOTOK "$SMPROGRAMS\$STARTMENU_FOLDER\Bitcoin Core (testnet, 64-bit).lnk"
]=])
  set(CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS [=[
DeleteRegKey HKCR "bitcoin"
]=])
endif()
