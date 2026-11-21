# CTD Overlays dock (src/overlays-core.c) pure-logic tests: C only, no libobs. On their own:
#   cmake -S test/overlays-dock -B build-overlays-dock-tests && cmake --build build-overlays-dock-tests
#   ctest --test-dir build-overlays-dock-tests
cmake_minimum_required(VERSION 3.28...3.30)

project(ctd-overlays-dock-tests LANGUAGES C)
enable_testing()

add_executable(ctd-overlays-dock-tests overlays-dock-test.c ../../src/overlays-core.c ../../src/overlay-scenes.c)
target_include_directories(ctd-overlays-dock-tests PRIVATE ../../src)
if(MSVC)
  target_compile_options(ctd-overlays-dock-tests PRIVATE /W3 /WX /utf-8)
  target_compile_definitions(ctd-overlays-dock-tests PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(ctd-overlays-dock-tests PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME ctd-overlays-dock-tests COMMAND ctd-overlays-dock-tests)
