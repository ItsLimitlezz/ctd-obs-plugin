# KeepScenesLive bookkeeping tests: C only, no libobs. Built from the plugin's CMakeLists (target ctd-keep-live-tests,
# not part of "all") or on its own:
#   cmake -S test/keep-live -B build-keep-live-tests && cmake --build build-keep-live-tests && ctest --test-dir
#   build-keep-live-tests
cmake_minimum_required(VERSION 3.28...3.30)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
  project(ctd-keep-live-tests LANGUAGES C)
  enable_testing()
  set(_ctd_kl_exclude "")
else()
  set(_ctd_kl_exclude EXCLUDE_FROM_ALL)
endif()

set(_ctd_kl_src "${CMAKE_CURRENT_SOURCE_DIR}/../../src/obs")

add_executable(ctd-keep-live-tests ${_ctd_kl_exclude})
target_sources(ctd-keep-live-tests PRIVATE keep-live-test.c "${_ctd_kl_src}/keep-live-core.c")
target_include_directories(ctd-keep-live-tests PRIVATE "${_ctd_kl_src}")
if(MSVC)
  target_compile_definitions(ctd-keep-live-tests PRIVATE _CRT_SECURE_NO_WARNINGS)
  target_compile_options(ctd-keep-live-tests PRIVATE /W3 /WX /utf-8)
else()
  target_compile_options(ctd-keep-live-tests PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME ctd-keep-live-tests COMMAND ctd-keep-live-tests)
