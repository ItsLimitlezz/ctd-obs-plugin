cmake_minimum_required(VERSION 3.28...3.30)

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
if(WIN32)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ws2_32)
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
