# CTD Producer dock logic tests (viewer totals, destination states, the merged chat ring): C only, no libobs, no Qt.
# Built from the plugin's CMakeLists (target ctd-producer-tests, not part of "all") or on its own:
#   cmake -S test/producer -B build-producer-tests && cmake --build build-producer-tests && ctest --test-dir
#   build-producer-tests
cmake_minimum_required(VERSION 3.28...3.30)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
  project(ctd-producer-tests LANGUAGES C)
  enable_testing()
  set(_ctd_producer_exclude "")
else()
  set(_ctd_producer_exclude EXCLUDE_FROM_ALL)
endif()

set(_ctd_producer_src "${CMAKE_CURRENT_SOURCE_DIR}/../../src/ui")

add_executable(ctd-producer-tests ${_ctd_producer_exclude})
target_sources(ctd-producer-tests PRIVATE producer-test.c "${_ctd_producer_src}/producer-core.c")
target_include_directories(ctd-producer-tests PRIVATE "${_ctd_producer_src}")
if(MSVC)
  target_compile_definitions(ctd-producer-tests PRIVATE _CRT_SECURE_NO_WARNINGS)
  target_compile_options(ctd-producer-tests PRIVATE /W3 /WX /utf-8)
else()
  target_compile_options(ctd-producer-tests PRIVATE -Wall -Wextra -Werror)
  target_link_libraries(ctd-producer-tests PRIVATE m)
endif()
add_test(NAME ctd-producer-tests COMMAND ctd-producer-tests)
