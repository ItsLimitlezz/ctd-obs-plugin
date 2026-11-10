cmake_minimum_required(VERSION 3.28...3.30)

# Built as a core module inside CTD OBS (plugins/ctd-live-source in the ctd-obs repo), using OBS's own targets and
# dependencies. Everything below this block is the standalone plugin build.
if(CMAKE_PROJECT_NAME STREQUAL "obs-studio")
  find_package(Qt6 REQUIRED Core Widgets)
  find_package(FFmpeg REQUIRED COMPONENTS avcodec avformat avutil swresample swscale)

  add_library(ctd-engine STATIC)
  target_sources(
    ctd-engine
    PRIVATE
      src/engine/audio-norm.c
      src/engine/delay-queue.c
      src/engine/engine.c
      src/engine/hw-decode.c
      src/engine/ltc.c
      src/engine/sync-group.c
  )
  target_include_directories(ctd-engine PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src/engine")
  target_link_libraries(ctd-engine PUBLIC FFmpeg::avcodec FFmpeg::avformat FFmpeg::avutil FFmpeg::swresample)
  set_property(TARGET ctd-engine PROPERTY FOLDER plugins/ctd-live-source)

  file(READ buildspec.json _ctd_buildspec)
  string(JSON _ctd_version GET "${_ctd_buildspec}" version)
  file(READ src/plugin-support.c.in _ctd_support)
  string(REPLACE "@CMAKE_PROJECT_NAME@" "ctd-live-source" _ctd_support "${_ctd_support}")
  string(REPLACE "@CMAKE_PROJECT_VERSION@" "${_ctd_version}" _ctd_support "${_ctd_support}")
  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/plugin-support.c" "${_ctd_support}")

  add_library(ctd-live-source MODULE)
  add_library(OBS::ctd-live-source ALIAS ctd-live-source)
  target_sources(
    ctd-live-source
    PRIVATE
      "${CMAKE_CURRENT_BINARY_DIR}/plugin-support.c"
      src/api/mediamtx-api.c
      src/api/poller.c
      src/http-client.c
      src/multistream.c
      src/obs/ctd-live-source.c
      src/obs/issues.c
      src/obs/loss-card.c
      src/obs/loss-video.c
      src/obs/obs-watch.c
      src/obs/servers.c
      src/obs/sync-groups.c
      src/obs/webhook.c
      src/obs/ws-vendor.c
      src/obsws-config.c
      src/plugin-main.c
      src/show/apply.c
      src/show/assets.c
      src/show/bundle.c
      src/show/devices-identity.c
      src/show/export.c
      src/show/ini.c
      src/show/json.c
      src/show/manifest.c
      src/show/scrub.c
      src/show/sha256.c
      src/show/show-fs.c
      src/show/show.c
      src/show/store.c
      src/ui/hub-dock.cpp
      src/ui/multistream-dock.cpp
      src/ui/servers-dialog.cpp
      src/ui/show-section.cpp
  )
  target_include_directories(ctd-live-source PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(
    ctd-live-source
    PRIVATE
      ctd-engine
      FFmpeg::avcodec
      FFmpeg::avformat
      FFmpeg::avutil
      FFmpeg::swresample
      FFmpeg::swscale
      OBS::frontend-api
      OBS::libobs
      Qt::Core
      Qt::Widgets
      $<$<PLATFORM_ID:Windows>:ws2_32>
      $<$<PLATFORM_ID:Windows>:bcrypt>
  )
  set_target_properties_obs(
    ctd-live-source
    PROPERTIES FOLDER plugins/ctd-live-source PREFIX "" AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
  return()
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_QT "Use Qt functionality" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

# Link the FFmpeg that OBS itself runs with (it is built with libsrt, which is how srt:// works): obs-deps on Windows
# and macOS, the system libraries on Linux. Nothing is bundled.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/finders")
find_package(FFmpeg REQUIRED COMPONENTS avcodec avformat avutil)
find_package(FFmpeg REQUIRED COMPONENTS swresample swscale)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE FFmpeg::avcodec FFmpeg::avformat FFmpeg::avutil)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE FFmpeg::swresample FFmpeg::swscale)

# libcurl for streaming file transfers to and from CTD Relay (Show Profiles, docs/show-profiles-s2.md). Also the copy OBS
# runs with: obs-deps' libcurl.dll (Schannel) on Windows, the SDK's on macOS, the system's on Linux. Nothing is bundled.
find_package(CURL REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE CURL::libcurl)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
  # moc's predefines step runs clang++ inside an Xcode script phase, where Xcode 26 exports a deployment target for
  # every Apple platform and clang refuses them ("conflicting deployment targets"). The plugin's moc input doesn't
  # need the compiler's predefined macros.
  set_property(TARGET ${CMAKE_PROJECT_NAME} PROPERTY AUTOMOC_COMPILER_PREDEFINES OFF)
endif()

# The engine (SPEC E1): C and FFmpeg only, no libobs, shared by the plugin and ctd-live-probe
add_library(ctd-engine STATIC)
target_sources(ctd-engine PRIVATE src/engine/engine.c src/engine/audio-norm.c src/engine/hw-decode.c)
target_sources(ctd-engine PRIVATE src/engine/delay-queue.c src/engine/ltc.c src/engine/sync-group.c)
target_include_directories(ctd-engine PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src/engine")
target_link_libraries(ctd-engine PUBLIC FFmpeg::avcodec FFmpeg::avformat FFmpeg::avutil FFmpeg::swresample)
set_property(TARGET ctd-engine PROPERTY POSITION_INDEPENDENT_CODE ON)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ctd-engine)

target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/plugin-main.c src/obs/ctd-live-source.c)
target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/obs/loss-card.c src/obs/loss-video.c src/api/mediamtx-api.c)
target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/api/poller.c src/ui/hub-dock.cpp src/obs/ws-vendor.c)
target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/obs/issues.c src/obs/sync-groups.c src/obs/webhook.c)
target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/obs/obs-watch.c src/obs/servers.c src/ui/servers-dialog.cpp)
target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/http-client.c src/multistream.c src/ui/multistream-dock.cpp)
target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/obsws-config.c)
# Show Profiles (design: hub epics/show-profiles-design.md). The pure-logic files are shared with ctd-show-tests.
set(
  CTD_SHOW_PURE_SOURCES
  src/show/assets.c
  src/show/bundle.c
  src/show/devices-identity.c
  src/show/devices.c
  src/show/export.c
  src/show/ini.c
  src/show/json.c
  src/show/manifest.c
  src/show/scrub.c
  src/show/sha256.c
  src/show/show-fs.c
  src/show/store.c
  src/show/sync-core.c
  src/show/sync-flow.c
  src/show/tar.c
)
target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE ${CTD_SHOW_PURE_SOURCES} src/show/apply.c src/show/device-map.c src/show/show.c src/show/sync.c
)
target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/ui/device-map-dialog.cpp src/ui/show-section.cpp)
if(WIN32)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ws2_32 bcrypt)
endif()

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

# ctd-live-probe (SPEC E5): the engine on the command line, for the test harness
option(ENABLE_PROBE "Build the ctd-live-probe command-line tool" OFF)
if(ENABLE_PROBE)
  add_executable(ctd-live-probe)
  target_sources(ctd-live-probe PRIVATE tools/ctd-live-probe/main.c tools/ctd-live-probe/group.c)
  target_link_libraries(ctd-live-probe PRIVATE ctd-engine)
  if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(ctd-live-probe PRIVATE Threads::Threads)
  endif()
  # the harness's LTC source (T17)
  add_executable(ltc-gen)
  target_sources(ltc-gen PRIVATE tools/ctd-live-probe/ltc-gen.c src/engine/ltc.c)
  target_include_directories(ltc-gen PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/engine")
  if(NOT WIN32)
    target_link_libraries(ltc-gen PRIVATE m)
  endif()
endif()

# Show Profiles pure-logic tests (tickets T6, T7, T9, T10): C only, no libobs. Not part of "all"; build the target, then
# run ctest. test/show/CMakeLists.txt also builds on its own (CI: .github/workflows/show-tests.yaml).
#   cmake --build --preset <preset> --target ctd-show-tests && ctest --test-dir <build dir> -C RelWithDebInfo
enable_testing()
add_subdirectory(test/show)
