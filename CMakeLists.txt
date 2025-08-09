cmake_minimum_required(VERSION 3.25)
project(viam-video-stream)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Optimize for Apple Silicon
if(CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -mcpu=apple-m1")
else()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -march=native")
endif()

# Set RPATH for macOS
set(CMAKE_MACOSX_RPATH ON)
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)

# Find packages
find_package(viam-cpp-sdk REQUIRED)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)

# FFmpeg
pkg_check_modules(FFMPEG REQUIRED libavformat libavcodec libavutil libswscale)

# macOS VideoToolbox
if(APPLE)
    add_definitions(-DUSE_VIDEOTOOLBOX)
    find_library(VIDEOTOOLBOX VideoToolbox REQUIRED)
    find_library(COREFOUNDATION CoreFoundation REQUIRED)
    find_library(COREMEDIA CoreMedia REQUIRED)
    find_library(COREVIDEO CoreVideo REQUIRED)
endif()

# Create executable
add_executable(video-stream-module
    src/main.cpp
    src/video_stream_camera.cpp
    src/buffer/frame_pool.cpp
)

target_include_directories(video-stream-module PRIVATE
    ${FFMPEG_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_directories(video-stream-module PRIVATE ${FFMPEG_LIBRARY_DIRS})

# Link libraries with full paths
target_link_libraries(video-stream-module
    viam-cpp-sdk::viamsdk
    ${FFMPEG_LIBRARIES}
    Threads::Threads
)

if(APPLE)
    target_link_libraries(video-stream-module
        ${VIDEOTOOLBOX}
        ${COREFOUNDATION}
        ${COREMEDIA}
        ${COREVIDEO}
    )
    
    # Set install name for libraries
    set_target_properties(video-stream-module PROPERTIES
        BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "@loader_path/../lib;/opt/homebrew/lib"
    )
endif()

# Copy to bin directory after building
add_custom_command(TARGET video-stream-module POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:video-stream-module> ${CMAKE_SOURCE_DIR}/bin/
)
