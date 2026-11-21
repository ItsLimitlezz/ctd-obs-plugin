# CTD Relay stream compatibility (src/relay-encoder.c) pure-logic tests: C only, no libobs. On their own:
#   cmake -S test/relay-encoder -B build-relay-encoder-tests && cmake --build build-relay-encoder-tests
#   ctest --test-dir build-relay-encoder-tests
cmake_minimum_required(VERSION 3.28...3.30)

project(ctd-relay-encoder-tests LANGUAGES C)
enable_testing()

add_executable(ctd-relay-encoder-tests relay-encoder-test.c ../../src/relay-encoder.c)
target_include_directories(ctd-relay-encoder-tests PRIVATE ../../src)
if(MSVC)
  target_compile_options(ctd-relay-encoder-tests PRIVATE /W3 /WX /utf-8)
  target_compile_definitions(ctd-relay-encoder-tests PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(ctd-relay-encoder-tests PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME ctd-relay-encoder-tests COMMAND ctd-relay-encoder-tests)
