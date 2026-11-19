# Auto-failover rules tests: C only, no libobs. Built from the plugin's CMakeLists (target ctd-failover-tests, not part
# of "all") or on its own:
#   cmake -S test/failover -B build-failover-tests && cmake --build build-failover-tests && ctest --test-dir
#   build-failover-tests
cmake_minimum_required(VERSION 3.28...3.30)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
  project(ctd-failover-tests LANGUAGES C)
  enable_testing()
  set(_ctd_fo_exclude "")
else()
  set(_ctd_fo_exclude EXCLUDE_FROM_ALL)
endif()

set(_ctd_fo_src "${CMAKE_CURRENT_SOURCE_DIR}/../../src/obs")

add_executable(ctd-failover-tests ${_ctd_fo_exclude})
target_sources(ctd-failover-tests PRIVATE failover-test.c "${_ctd_fo_src}/failover-core.c")
target_include_directories(ctd-failover-tests PRIVATE "${_ctd_fo_src}")
if(MSVC)
  target_compile_definitions(ctd-failover-tests PRIVATE _CRT_SECURE_NO_WARNINGS)
  target_compile_options(ctd-failover-tests PRIVATE /W3 /WX /utf-8)
else()
  target_compile_options(ctd-failover-tests PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME ctd-failover-tests COMMAND ctd-failover-tests)
