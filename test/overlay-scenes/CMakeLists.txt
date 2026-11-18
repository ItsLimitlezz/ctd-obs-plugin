# Overlay scene sources (src/overlay-scenes.c) pure-logic tests: C only, no libobs. On their own:
#   cmake -S test/overlay-scenes -B build-overlay-scenes-tests && cmake --build build-overlay-scenes-tests
#   ctest --test-dir build-overlay-scenes-tests
cmake_minimum_required(VERSION 3.28...3.30)

project(ctd-overlay-scenes-tests LANGUAGES C)
enable_testing()

add_executable(ctd-overlay-scenes-tests overlay-scenes-test.c ../../src/overlay-scenes.c)
target_include_directories(ctd-overlay-scenes-tests PRIVATE ../../src)
if(MSVC)
  target_compile_options(ctd-overlay-scenes-tests PRIVATE /W3 /WX /utf-8)
  target_compile_definitions(ctd-overlay-scenes-tests PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(ctd-overlay-scenes-tests PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME ctd-overlay-scenes-tests COMMAND ctd-overlay-scenes-tests)
