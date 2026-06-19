cmake_minimum_required(VERSION 3.22)

project(BitcoinDependsSources NONE)

get_filename_component(bitcoin_source_dir "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(BITCOIN_DEPENDS_SOURCE_DIR "${bitcoin_source_dir}" CACHE PATH "Bitcoin Core source directory for CMake-built bundled dependencies.")

add_library(Threads::Threads INTERFACE IMPORTED)

include("${BITCOIN_DEPENDS_SOURCE_DIR}/cmake/depends/BitcoinDepends.cmake")

bitcoin_depends_add_boost()
bitcoin_depends_add_libevent()
bitcoin_depends_add_sqlite()
bitcoin_depends_add_systemtap()
bitcoin_depends_add_zeromq()
bitcoin_depends_add_libmultiprocess()
