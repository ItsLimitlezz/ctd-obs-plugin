# CTD Overlays dock (src/overlays-core.c) and overlay address (src/overlay-address.c) pure-logic tests: C only, no
# libobs. On their own:
#   cmake -S test/overlays-dock -B build-overlays-dock-tests && cmake --build build-overlays-dock-tests
#   ctest --test-dir build-overlays-dock-tests
cmake_minimum_required(VERSION 3.28...3.30)

project(ctd-overlays-dock-tests LANGUAGES C)
enable_testing()

add_executable(ctd-overlays-dock-tests overlays-dock-test.c ../../src/overlays-core.c ../../src/overlay-scenes.c)
add_executable(ctd-overlay-address-tests overlay-address-test.c ../../src/overlay-address.c)
foreach(target IN ITEMS ctd-overlays-dock-tests ctd-overlay-address-tests)
  target_include_directories(${target} PRIVATE ../../src)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W3 /WX /utf-8)
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
  endif()
  add_test(NAME ${target} COMMAND ${target})
endforeach()
