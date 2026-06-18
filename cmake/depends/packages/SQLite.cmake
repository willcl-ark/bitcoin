# Copyright (c) 2026-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

function(bitcoin_depends_add_sqlite)
  if(TARGET SQLite3::SQLite3)
    return()
  endif()

  set(sqlite_include_dir "${BITCOIN_DEPENDS_PREFIX}/include")
  set(sqlite_lib_dir "${BITCOIN_DEPENDS_PREFIX}/lib")
  file(MAKE_DIRECTORY "${sqlite_include_dir}" "${sqlite_lib_dir}")

  set(sqlite_library "${sqlite_lib_dir}/${CMAKE_STATIC_LIBRARY_PREFIX}sqlite3${CMAKE_STATIC_LIBRARY_SUFFIX}")
  set(sqlite_cppflags
    "-DSQLITE_DQS=0"
    "-DSQLITE_DEFAULT_MEMSTATUS=0"
    "-DSQLITE_OMIT_DEPRECATED"
    "-DSQLITE_OMIT_SHARED_CACHE"
    "-DSQLITE_OMIT_JSON"
    "-DSQLITE_LIKE_DOESNT_MATCH_BLOBS"
    "-DSQLITE_OMIT_DECLTYPE"
    "-DSQLITE_OMIT_PROGRESS_CALLBACK"
    "-DSQLITE_OMIT_AUTOINIT"
    "-DSQLITE_OMIT_LOAD_EXTENSION"
  )
  list(JOIN sqlite_cppflags " " sqlite_cppflags)
  if(NOT "${BITCOIN_DEPENDS_CPP_FLAGS}" STREQUAL "")
    string(APPEND sqlite_cppflags " ${BITCOIN_DEPENDS_CPP_FLAGS}")
  endif()

  set(sqlite_configure_args
    "--disable-shared"
    "--disable-readline"
    "--disable-rtree"
    "--disable-fts4"
    "--disable-fts5"
  )
  if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    list(APPEND sqlite_configure_args "--debug")
  endif()

  set(sqlite_patch_dir "${PROJECT_SOURCE_DIR}/depends/patches/sqlite")
  set(sqlite_build_env
    "CPPFLAGS=${sqlite_cppflags}"
  )
  bitcoin_depends_external_autoconf_project(bitcoin_depends_sqlite_project
    URL "https://sqlite.org/2025/sqlite-autoconf-3500400.tar.gz"
    SHA256 "a3db587a1b92ee5ddac2f66b3edb41b26f9c867275782d46c3a088977d6a5b18"
    PATCH_COMMAND patch -p1 -i "${sqlite_patch_dir}/autosetup-fixup.patch"
    CONFIGURE_ENV ${sqlite_build_env}
    CONFIGURE_ARGS ${sqlite_configure_args}
    BUILD_COMMAND
      "${CMAKE_COMMAND}" -E env ${sqlite_build_env}
      "${BITCOIN_DEPENDS_MAKE_PROGRAM}" libsqlite3.a
    INSTALL_COMMAND
      "${CMAKE_COMMAND}" -E env ${sqlite_build_env}
      "${BITCOIN_DEPENDS_MAKE_PROGRAM}" install-headers install-lib
    BUILD_BYPRODUCTS "${sqlite_library}"
  )

  add_library(bitcoin_depends_sqlite INTERFACE)
  add_library(SQLite3::SQLite3 ALIAS bitcoin_depends_sqlite)
  target_include_directories(bitcoin_depends_sqlite SYSTEM INTERFACE
    "${sqlite_include_dir}"
  )
  target_link_libraries(bitcoin_depends_sqlite INTERFACE
    "${sqlite_library}"
    Threads::Threads
  )
  add_dependencies(bitcoin_depends_sqlite bitcoin_depends_sqlite_project)
endfunction()
