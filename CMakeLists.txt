cmake_minimum_required(VERSION 3.22)
project(viam-video-playback)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Performance optimizations
if(CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64" OR CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3")
else()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -march=native")
endif()

set(CMAKE_MACOSX_RPATH ON)
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)

find_package(viam-cpp-sdk REQUIRED)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)

# FFmpeg (always)
pkg_check_modules(FFMPEG REQUIRED libavformat libavcodec libavutil libswscale)

# macOS VideoToolbox
if(APPLE)
    add_definitions(-DUSE_VIDEOTOOLBOX)
    find_library(VIDEOTOOLBOX VideoToolbox REQUIRED)
    find_library(COREFOUNDATION CoreFoundation REQUIRED)
    find_library(COREMEDIA CoreMedia REQUIRED)
    find_library(COREVIDEO CoreVideo REQUIRED)
    find_library(SECURITY Security REQUIRED)
    find_library(CORESERVICES CoreServices REQUIRED)
endif()

# Jetson / aarch64: enable NVDEC + REQUIRE GStreamer fast path
set(USE_GST_FASTPATH OFF)
if(UNIX AND NOT APPLE AND (CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64"))
    message(STATUS "ARM64 Linux detected, enabling NVIDIA NVDEC + GStreamer fast path.")
    add_definitions(-DUSE_NVDEC)
    set(USE_GST_FASTPATH ON)
    pkg_check_modules(GST REQUIRED gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0)
endif()

# Sources
set(SRC_FILES
    src/main.cpp
    src/video_playback_camera.cpp
)

# Only compile the Gst wrapper on Jetson
if(USE_GST_FASTPATH)
    list(APPEND SRC_FILES src/gst_pipeline_wrapper.cpp)
endif()

add_executable(video-playback-module ${SRC_FILES})

target_include_directories(video-playback-module PRIVATE
    ${FFMPEG_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_directories(video-playback-module PRIVATE ${FFMPEG_LIBRARY_DIRS})

target_link_libraries(video-playback-module
    viam-cpp-sdk::viamsdk
    ${FFMPEG_LIBRARIES}
    Threads::Threads
)

# Link GST only on Jetson
if(USE_GST_FASTPATH)
    target_include_directories(video-playback-module PRIVATE ${GST_INCLUDE_DIRS})
    target_link_directories(video-playback-module PRIVATE ${GST_LIBRARY_DIRS})
    target_link_libraries(video-playback-module ${GST_LIBRARIES})
endif()

if(APPLE)
    target_link_libraries(video-playback-module
        ${VIDEOTOOLBOX} ${COREFOUNDATION} ${COREMEDIA} ${COREVIDEO} ${SECURITY} ${CORESERVICES}
    )
    set_target_properties(video-playback-module PROPERTIES
        BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "@loader_path/../lib;/opt/homebrew/lib;/usr/local/lib"
    )
endif()

add_custom_command(TARGET video-playback-module POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_SOURCE_DIR}/bin
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:video-playback-module> ${CMAKE_SOURCE_DIR}/bin/
    COMMENT "Copying executable to bin directory"
)

message(STATUS "=== Video Playback Module Configuration ===")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "System: ${CMAKE_SYSTEM_NAME}")
message(STATUS "Processor: ${CMAKE_SYSTEM_PROCESSOR}")
if(APPLE)
    message(STATUS "VideoToolbox: Enabled")
endif()
if(USE_GST_FASTPATH)
    message(STATUS "Jetson fast path: ENABLED (GStreamer + nvv4l2 + nvjpegenc)")
else()
    message(STATUS "Jetson fast path: DISABLED on this platform")
endif()
message(STATUS "===========================================")
