# Copyright (c) 2026-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

function(bitcoin_depends_patch_command out_var)
  set(patch_commands)
  foreach(patch IN LISTS ARGN)
    list(APPEND patch_commands
      COMMAND "${CMAKE_COMMAND}" -E chdir "<SOURCE_DIR>" "${Patch_EXECUTABLE}" -p1 -i "${patch}"
    )
  endforeach()
  set(${out_var} ${patch_commands} PARENT_SCOPE)
endfunction()

macro(bitcoin_depends_append_cmake_arg out_var name value)
  if(NOT "${value}" STREQUAL "")
    list(APPEND ${out_var} "-D${name}=${value}")
  endif()
endmacro()

function(bitcoin_depends_cmake_args out_var)
  set(options NATIVE)
  cmake_parse_arguments(ARG "${options}" "" "" ${ARGN})

  set(cmake_args)
  if(NOT ARG_NATIVE)
    bitcoin_depends_append_cmake_arg(cmake_args CMAKE_TOOLCHAIN_FILE "${BITCOIN_DEPENDS_TARGET_TOOLCHAIN_FILE}")
  endif()

  set(${out_var} ${cmake_args} PARENT_SCOPE)
endfunction()

function(bitcoin_depends_cmake_package name)
  set(options NATIVE)
  set(one_value_args URL SHA256 PREFIX)
  set(multi_value_args DEPENDS PATCHES CMAKE_ARGS INSTALL_COMMAND)
  cmake_parse_arguments(PKG "${options}" "${one_value_args}" "${multi_value_args}" ${ARGN})

  if(NOT PKG_PREFIX)
    set(PKG_PREFIX "${BITCOIN_DEPENDS_PREFIX}")
  endif()

  bitcoin_depends_patch_command(patch_command ${PKG_PATCHES})

  if(PKG_INSTALL_COMMAND)
    set(install_command ${PKG_INSTALL_COMMAND})
  else()
    set(install_command "${CMAKE_COMMAND}" --build "<BINARY_DIR>" --target install)
  endif()

  set(cmake_options)
  if(PKG_NATIVE)
    list(APPEND cmake_options NATIVE)
  endif()
  bitcoin_depends_cmake_args(package_cmake_args ${cmake_options})

  ExternalProject_Add(${name}
    LIST_SEPARATOR "|"
    URL "${PKG_URL}"
    URL_HASH "SHA256=${PKG_SHA256}"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    DEPENDS ${PKG_DEPENDS}
    PATCH_COMMAND ${patch_command}
    CMAKE_ARGS
      "-DCMAKE_INSTALL_PREFIX=${PKG_PREFIX}"
      "-DCMAKE_INSTALL_LIBDIR=lib"
      "-DCMAKE_POSITION_INDEPENDENT_CODE=ON"
      ${package_cmake_args}
      ${PKG_CMAKE_ARGS}
    INSTALL_COMMAND ${install_command}
  )

  ExternalProject_Add_StepTargets(${name} download)
  add_custom_target(${name}-source DEPENDS ${name}-download)
  add_dependencies(depends-download ${name}-source)
endfunction()
