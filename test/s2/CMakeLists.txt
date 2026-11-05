# Spike S2 harness as its own small CMake project, for the PC (Windows) and Mac. It links the same libcurl and FFmpeg
# the plugin builds against (avformat only because http-client.c's JSON half uses avio): obs-deps from the plugin's
# .deps (configure the plugin once first), or the system's.
#
#   cmake -S test/s2 -B build-s2 -DCMAKE_PREFIX_PATH=<repo>/.deps/obs-deps-2026-07-15-x64
#   cmake --build build-s2 --config RelWithDebInfo
#
# test/s2/run-s2.ps1 does this and runs it.
cmake_minimum_required(VERSION 3.28)
project(ctd-s2-harness C)

set(root "${CMAKE_CURRENT_SOURCE_DIR}/../..")
list(APPEND CMAKE_MODULE_PATH "${root}/cmake/finders")
find_package(CURL REQUIRED)
find_package(FFmpeg REQUIRED COMPONENTS avformat avutil)

add_executable(s2-test s2-test.c "${root}/src/http-client.c" shim/shim.c)
target_include_directories(s2-test PRIVATE shim "${root}/src")
target_link_libraries(s2-test PRIVATE CURL::libcurl FFmpeg::avformat FFmpeg::avutil)
target_compile_features(s2-test PRIVATE c_std_11)
if(WIN32)
  target_compile_definitions(s2-test PRIVATE _CRT_SECURE_NO_WARNINGS)
  target_link_libraries(s2-test PRIVATE psapi)
else()
  target_compile_definitions(s2-test PRIVATE _FILE_OFFSET_BITS=64 _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE)
endif()
