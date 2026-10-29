# FindFFmpeg: find the FFmpeg libraries OBS itself runs with
#
# On Windows and macOS they come from the obs-deps package on CMAKE_PREFIX_PATH, the same FFmpeg build OBS ships; on
# Linux they are the system libraries, found through pkg-config. Creates an FFmpeg::<component> imported target for each
# requested component (avcodec, avformat, avutil, ...).

include(FindPackageHandleStandardArgs)

if(NOT CMAKE_HOST_SYSTEM_NAME MATCHES "Windows|Darwin")
  find_package(PkgConfig QUIET)
endif()

foreach(_component IN LISTS FFmpeg_FIND_COMPONENTS)
  if(PKG_CONFIG_FOUND)
    pkg_search_module(PC_FFmpeg_${_component} QUIET lib${_component})
  endif()

  find_path(
    FFmpeg_${_component}_INCLUDE_DIR
    NAMES lib${_component}/${_component}.h
    HINTS ${PC_FFmpeg_${_component}_INCLUDE_DIRS}
  )
  find_library(
    FFmpeg_${_component}_LIBRARY
    NAMES ${_component} lib${_component}
    HINTS ${PC_FFmpeg_${_component}_LIBRARY_DIRS}
  )
  mark_as_advanced(FFmpeg_${_component}_INCLUDE_DIR FFmpeg_${_component}_LIBRARY)

  if(FFmpeg_${_component}_INCLUDE_DIR AND FFmpeg_${_component}_LIBRARY)
    set(FFmpeg_${_component}_FOUND TRUE)
    list(APPEND FFmpeg_LIBRARIES "${FFmpeg_${_component}_LIBRARY}")

    if(NOT TARGET FFmpeg::${_component})
      add_library(FFmpeg::${_component} UNKNOWN IMPORTED)
      set_target_properties(
        FFmpeg::${_component}
        PROPERTIES
          IMPORTED_LOCATION "${FFmpeg_${_component}_LIBRARY}"
          INTERFACE_INCLUDE_DIRECTORIES "${FFmpeg_${_component}_INCLUDE_DIR}"
      )
    endif()
  endif()
endforeach()

find_package_handle_standard_args(FFmpeg REQUIRED_VARS FFmpeg_LIBRARIES HANDLE_COMPONENTS)
