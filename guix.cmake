cmake_host_system_information(RESULT HOST_NAME QUERY HOSTNAME)

set(CTEST_SITE ${HOST_NAME})
set(CTEST_BUILD_NAME "Guix")
set(CTEST_SOURCE_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}")
set(CTEST_BINARY_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}")

# NOTE: If include(CTest) is added to the main CMakeLists.txt, this script
# and CTestConfig.cmake can live outside the repo, eliminating the need
# for a rebasing branch.
set(CTEST_BUILD_COMMAND "bash -c \"unset SOURCE_DATE_EPOCH && git fetch upstream && git rebase upstream/master && ${CTEST_SOURCE_DIRECTORY}/contrib/guix/guix-build\"")

file(REMOVE_RECURSE "${CTEST_BINARY_DIRECTORY}/Testing")

ctest_start("Continuous")

ctest_build()

ctest_submit()
