# Copyright (c) 2026-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

function(bitcoin_depends_add_capnproto)
  if(TARGET CapnProto::capnp)
    return()
  endif()

  set(capnp_version "1.4.0")
  set(capnp_url "https://capnproto.org/capnproto-c++-${capnp_version}.tar.gz")
  set(capnp_sha256 "fa02378ad522b318916b9ad928d1372fc9abd43dd1f4f0392e50450f5c87828f")
  set(capnp_include_dir "${BITCOIN_DEPENDS_PREFIX}/include")
  set(capnp_lib_dir "${BITCOIN_DEPENDS_PREFIX}/lib")
  set(native_capnp_bin_dir "${BITCOIN_DEPENDS_NATIVE_PREFIX}/bin")
  file(MAKE_DIRECTORY "${capnp_include_dir}" "${capnp_lib_dir}" "${native_capnp_bin_dir}")

  set(capnp_common_cmake_args
    "-DBUILD_TESTING=OFF"
    "-DWITH_OPENSSL=OFF"
    "-DWITH_ZLIB=OFF"
    "-DBUILD_SHARED_LIBS=OFF"
  )

  if(DEFINED CMAKE_HOST_EXECUTABLE_SUFFIX)
    set(native_executable_suffix "${CMAKE_HOST_EXECUTABLE_SUFFIX}")
  else()
    set(native_executable_suffix "")
  endif()
  set(capnp_executable "${native_capnp_bin_dir}/capnp${native_executable_suffix}")
  set(capnpc_cxx_executable "${native_capnp_bin_dir}/capnpc-c++${native_executable_suffix}")

  bitcoin_depends_external_native_cmake_project(bitcoin_depends_native_capnp
    URL "${capnp_url}"
    SHA256 "${capnp_sha256}"
    CMAKE_ARGS ${capnp_common_cmake_args}
    BUILD_BYPRODUCTS
      "${capnp_executable}"
      "${capnpc_cxx_executable}"
  )

  set(capnp_kj_library "${capnp_lib_dir}/${CMAKE_STATIC_LIBRARY_PREFIX}kj${CMAKE_STATIC_LIBRARY_SUFFIX}")
  set(capnp_kj_async_library "${capnp_lib_dir}/${CMAKE_STATIC_LIBRARY_PREFIX}kj-async${CMAKE_STATIC_LIBRARY_SUFFIX}")
  set(capnp_capnp_library "${capnp_lib_dir}/${CMAKE_STATIC_LIBRARY_PREFIX}capnp${CMAKE_STATIC_LIBRARY_SUFFIX}")
  set(capnp_capnp_rpc_library "${capnp_lib_dir}/${CMAKE_STATIC_LIBRARY_PREFIX}capnp-rpc${CMAKE_STATIC_LIBRARY_SUFFIX}")
  set(capnp_capnpc_library "${capnp_lib_dir}/${CMAKE_STATIC_LIBRARY_PREFIX}capnpc${CMAKE_STATIC_LIBRARY_SUFFIX}")

  bitcoin_depends_external_cmake_project(bitcoin_depends_capnp
    URL "${capnp_url}"
    SHA256 "${capnp_sha256}"
    DEPENDS bitcoin_depends_native_capnp
    CMAKE_ARGS ${capnp_common_cmake_args}
    BUILD_BYPRODUCTS
      "${capnp_kj_library}"
      "${capnp_kj_async_library}"
      "${capnp_capnp_library}"
      "${capnp_capnp_rpc_library}"
      "${capnp_capnpc_library}"
  )

  set(capnp_config_dir "${PROJECT_BINARY_DIR}/_depends/CapnProto")
  file(MAKE_DIRECTORY "${capnp_config_dir}")
  configure_file(
    "${PROJECT_SOURCE_DIR}/cmake/depends/templates/CapnProtoConfig.cmake.in"
    "${capnp_config_dir}/CapnProtoConfig.cmake"
    @ONLY
  )
  configure_file(
    "${PROJECT_SOURCE_DIR}/cmake/depends/templates/CapnProtoConfigVersion.cmake.in"
    "${capnp_config_dir}/CapnProtoConfigVersion.cmake"
    @ONLY
  )

  set(CapnProto_DIR "${capnp_config_dir}" CACHE PATH "Cap'n Proto package directory." FORCE)
  find_package(CapnProto 0.7 QUIET NO_MODULE REQUIRED)
endfunction()

function(bitcoin_depends_add_libmultiprocess)
  bitcoin_depends_add_capnproto()

  if(DEFINED CMAKE_HOST_EXECUTABLE_SUFFIX)
    set(native_executable_suffix "${CMAKE_HOST_EXECUTABLE_SUFFIX}")
  else()
    set(native_executable_suffix "")
  endif()
  set(mpgen_executable "${BITCOIN_DEPENDS_NATIVE_PREFIX}/bin/mpgen${native_executable_suffix}")

  bitcoin_depends_external_native_cmake_project(bitcoin_depends_native_libmultiprocess
    SOURCE_DIR "${PROJECT_SOURCE_DIR}/src/ipc/libmultiprocess"
    DEPENDS bitcoin_depends_native_capnp
    CMAKE_ARGS
      "-DCMAKE_PREFIX_PATH=${BITCOIN_DEPENDS_NATIVE_PREFIX}"
      "-DBUILD_TESTING=OFF"
    INSTALL_COMMAND
      "${CMAKE_COMMAND}" --build <BINARY_DIR> --target install-bin
    BUILD_BYPRODUCTS "${mpgen_executable}"
  )

  set(MPGEN_EXECUTABLE "${mpgen_executable}" CACHE FILEPATH "External mpgen executable." FORCE)
  set(MPGEN_EXECUTABLE_DEPENDS bitcoin_depends_native_libmultiprocess CACHE STRING "Target producing MPGEN_EXECUTABLE." FORCE)
endfunction()
