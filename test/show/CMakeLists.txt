# Show Profiles pure-logic tests: C only, no libobs, no FFmpeg. Built from the plugin's CMakeLists (target
# ctd-show-tests, not part of "all") or on its own:
#   cmake -S test/show -B build-show-tests && cmake --build build-show-tests && ctest --test-dir build-show-tests
cmake_minimum_required(VERSION 3.28...3.30)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
  project(ctd-show-tests LANGUAGES C)
  enable_testing()
  set(_ctd_show_exclude "")
else()
  set(_ctd_show_exclude EXCLUDE_FROM_ALL)
endif()

set(_ctd_show_src "${CMAKE_CURRENT_SOURCE_DIR}/../../src/show")

# T6: device identity extractor against its fixtures
add_executable(ctd-show-tests ${_ctd_show_exclude})
target_sources(ctd-show-tests PRIVATE identity-test.c "${_ctd_show_src}/devices-identity.c")
target_include_directories(ctd-show-tests PRIVATE "${_ctd_show_src}")
file(GLOB CTD_SHOW_IDENTITY_FIXTURES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/identity-fixtures/*.json")
add_test(NAME ctd-show-tests COMMAND ctd-show-tests ${CTD_SHOW_IDENTITY_FIXTURES})

# T7, T9, T10, T11, M4: JSON model, manifest validation, scrubber, asset rewriter, INI filter, export, local store
add_executable(ctd-show-unit ${_ctd_show_exclude})
target_sources(
  ctd-show-unit
  PRIVATE
    unit-main.c
    assets-test.c
    devices-test.c
    export-test.c
    ini-test.c
    json-test.c
    manifest-test.c
    scrub-test.c
    store-test.c
    sync-test.c
    "${_ctd_show_src}/assets.c"
    "${_ctd_show_src}/bundle.c"
    "${_ctd_show_src}/devices-identity.c"
    "${_ctd_show_src}/devices.c"
    "${_ctd_show_src}/export.c"
    "${_ctd_show_src}/ini.c"
    "${_ctd_show_src}/json.c"
    "${_ctd_show_src}/manifest.c"
    "${_ctd_show_src}/scrub.c"
    "${_ctd_show_src}/sha256.c"
    "${_ctd_show_src}/show-fs.c"
    "${_ctd_show_src}/store.c"
    "${_ctd_show_src}/sync-core.c"
    "${_ctd_show_src}/sync-flow.c"
    "${_ctd_show_src}/tar.c"
)
target_include_directories(ctd-show-unit PRIVATE "${_ctd_show_src}")
if(WIN32)
  target_link_libraries(ctd-show-unit PRIVATE bcrypt)
  target_compile_definitions(ctd-show-unit PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_link_libraries(ctd-show-unit PRIVATE m)
endif()
if(MSVC)
  target_compile_options(ctd-show-unit PRIVATE /W3 /WX /utf-8)
else()
  target_compile_options(ctd-show-unit PRIVATE -Wall -Wextra -Werror -Wno-unused-function)
endif()
# argv: the fixtures folder, then a scratch folder the tests may fill and empty
add_test(
  NAME ctd-show-unit
  COMMAND ctd-show-unit "${CMAKE_CURRENT_SOURCE_DIR}/fixtures" "${CMAKE_CURRENT_BINARY_DIR}/scratch"
)
