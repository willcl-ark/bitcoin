# Copyright (c) 2026-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

#[=[

target_native_capnp_sources
---------------------------

This function adds standard Cap'n Proto C++ generated files to a target. It only
generates native Cap'n Proto output:

  - *.capnp.c++
  - *.capnp.h

Arguments:

  target: The target receiving generated sources.

  include_prefix: Absolute path indicating what portion of schema paths should
    be used for generated include paths. For example, if the schema path is
    /home/src/ipc/capnp/init.capnp and include_prefix is /home/src, generated
    includes refer to ipc/capnp/init.capnp.h.

Additional unnamed arguments:

  Paths to .capnp files relative to CMAKE_CURRENT_SOURCE_DIR.

Optional keyword arguments:

  IMPORT_PATHS: Additional directories to search for imported .capnp files.

#]=]

function(target_native_capnp_sources target include_prefix)
  cmake_parse_arguments(PARSE_ARGV 2
    "TNCS"          # prefix
    ""              # options
    ""              # one_value_keywords
    "IMPORT_PATHS"  # multi_value_keywords
  )

  if(NOT TNCS_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "target_native_capnp_sources() called without schema files.")
  endif()

  if(NOT CAPNP_EXECUTABLE)
    message(FATAL_ERROR "Could not locate capnp executable (CAPNP_EXECUTABLE).")
  endif()
  if(NOT CAPNPC_CXX_EXECUTABLE)
    message(FATAL_ERROR "Could not locate capnpc-c++ executable (CAPNPC_CXX_EXECUTABLE).")
  endif()
  if(NOT CAPNP_INCLUDE_DIRECTORY)
    message(FATAL_ERROR "Could not locate Cap'n Proto includes (CAPNP_INCLUDE_DIRECTORY).")
  endif()

  get_filename_component(include_prefix "${include_prefix}" ABSOLUTE)

  set(build_include_prefix "${CMAKE_BINARY_DIR}")
  file(RELATIVE_PATH relative_path "${CMAKE_SOURCE_DIR}" "${include_prefix}")
  if(relative_path)
    string(APPEND build_include_prefix "/" "${relative_path}")
  endif()

  set(include_paths
    -I "${include_prefix}"
    -I "${CAPNP_INCLUDE_DIRECTORY}"
  )
  foreach(import_path IN LISTS TNCS_IMPORT_PATHS)
    get_filename_component(import_path "${import_path}" ABSOLUTE)
    list(APPEND include_paths -I "${import_path}")
  endforeach()

  set(generated_sources "")
  set(generated_headers "")
  foreach(capnp_file IN LISTS TNCS_UNPARSED_ARGUMENTS)
    get_filename_component(source_file "${capnp_file}" ABSOLUTE)
    if(NOT EXISTS "${source_file}")
      message(FATAL_ERROR "Cap'n Proto schema file '${source_file}' does not exist.")
    endif()

    string(LENGTH "${include_prefix}" prefix_len)
    string(SUBSTRING "${source_file}" 0 "${prefix_len}" source_prefix)
    if(NOT "${include_prefix}" STREQUAL "${source_prefix}")
      message(FATAL_ERROR
        "Could not determine output path for '${source_file}' with source "
        "prefix '${include_prefix}'."
      )
    endif()

    string(SUBSTRING "${source_file}" "${prefix_len}" -1 output_path)
    set(output_base "${build_include_prefix}${output_path}")

    add_custom_command(
      OUTPUT
        "${output_base}.c++"
        "${output_base}.h"
      COMMAND "${CAPNP_EXECUTABLE}"
      ARGS compile
        -o "${CAPNPC_CXX_EXECUTABLE}:${build_include_prefix}"
        --src-prefix "${include_prefix}"
        ${include_paths}
        "${source_file}"
      DEPENDS "${source_file}"
      COMMENT "Compiling Cap'n Proto schema ${capnp_file}"
      VERBATIM
    )

    list(APPEND generated_sources "${output_base}.c++")
    list(APPEND generated_headers "${output_base}.h")
  endforeach()

  set_source_files_properties(
    ${generated_sources}
    ${generated_headers}
    PROPERTIES
      GENERATED TRUE
      SKIP_LINTING TRUE
  )

  target_sources(${target} PRIVATE ${generated_sources})
  target_include_directories(${target} PUBLIC
    $<BUILD_INTERFACE:${build_include_prefix}>
  )
  target_link_libraries(${target} PRIVATE
    CapnProto::capnp-rpc
    CapnProto::capnp
    CapnProto::kj-async
    CapnProto::kj
  )

  if(NOT TARGET "${target}_headers")
    add_custom_target("${target}_headers" DEPENDS ${generated_headers})
  else()
    set(native_headers_target "${target}_native_capnp_headers")
    add_custom_target("${native_headers_target}" DEPENDS ${generated_headers})
    add_dependencies("${target}_headers" "${native_headers_target}")
  endif()
endfunction()
