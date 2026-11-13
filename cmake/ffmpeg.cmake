# FFmpeg: on Windows and macOS from the obs-deps package OBS itself is built
# with, so the plugin uses the FFmpeg libraries shipped with OBS at runtime.
# On Linux from the system (same libraries the distribution's OBS uses).
if(OS_LINUX)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(
    FFMPEG
    REQUIRED
    IMPORTED_TARGET
    libavformat
    libavcodec
    libavdevice
    libavutil
    libswscale
  )
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE PkgConfig::FFMPEG)
else()
  find_path(FFMPEG_INCLUDE_DIR libavformat/avformat.h REQUIRED)
  target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE "${FFMPEG_INCLUDE_DIR}")
  foreach(
    _lib
    IN
    ITEMS avformat avcodec avdevice avutil swscale
  )
    find_library(FFMPEG_${_lib}_LIBRARY NAMES ${_lib} REQUIRED)
    target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE "${FFMPEG_${_lib}_LIBRARY}")
  endforeach()
endif()
