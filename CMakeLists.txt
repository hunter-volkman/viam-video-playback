cmake_minimum_required(VERSION 3.22)
project(viam-video-playback)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Detect architecture
if(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64" OR CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(IS_JETSON ON)
        message(STATUS "Detected Jetson platform (aarch64 Linux)")
    else()
        set(IS_JETSON OFF)
    endif()
else()
    set(IS_JETSON OFF)
endif()

# Optimization flags
if(IS_JETSON)
    # Jetson Orin NX specific optimizations
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -march=armv8.2-a+crypto+fp16+rcpc+dotprod")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mtune=cortex-a78ae")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -ffast-math -funroll-loops -fprefetch-loop-arrays")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -flto=auto")  # Link-time optimization
    add_definitions(-DUSE_NEON)
elseif(APPLE)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -ffast-math")
    add_definitions(-DUSE_VIDEOTOOLBOX)
    if(CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mcpu=apple-m1")
    else()
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=native")
    endif()
else()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -march=native -ffast-math")
endif()

# Find required packages
find_package(viam-cpp-sdk REQUIRED)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)

# FFmpeg (always required)
pkg_check_modules(FFMPEG REQUIRED libavformat libavcodec libavutil libswscale)

# Platform-specific dependencies
if(IS_JETSON)
    # GStreamer for Jetson hardware acceleration (optional for future use)
    pkg_check_modules(GST gstreamer-1.0>=1.14 gstreamer-app-1.0 gstreamer-video-1.0)
    if(GST_FOUND)
        add_definitions(-DUSE_NVDEC)
        message(STATUS "Enabling Jetson NVDEC hardware acceleration support")
    else()
        message(STATUS "GStreamer not found, using software decoding")
    endif()
elseif(APPLE)
    # VideoToolbox for macOS hardware acceleration
    find_library(VIDEOTOOLBOX VideoToolbox REQUIRED)
    find_library(COREFOUNDATION CoreFoundation REQUIRED)
    find_library(COREMEDIA CoreMedia REQUIRED)
    find_library(COREVIDEO CoreVideo REQUIRED)
    message(STATUS "Enabling VideoToolbox hardware acceleration")
endif()

# Source files
set(SOURCES
    src/main.cpp
    src/video_playback_camera.cpp
)

# Create executable
add_executable(video-playback-module ${SOURCES})

# Include directories
target_include_directories(video-playback-module PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${FFMPEG_INCLUDE_DIRS}
)

# Link directories
target_link_directories(video-playback-module PRIVATE 
    ${FFMPEG_LIBRARY_DIRS}
)

# Core libraries
target_link_libraries(video-playback-module
    viam-cpp-sdk::viamsdk
    ${FFMPEG_LIBRARIES}
    Threads::Threads
)

# Platform-specific linking
if(IS_JETSON AND GST_FOUND)
    target_include_directories(video-playback-module PRIVATE ${GST_INCLUDE_DIRS})
    target_link_directories(video-playback-module PRIVATE ${GST_LIBRARY_DIRS})
    target_link_libraries(video-playback-module ${GST_LIBRARIES})
elseif(APPLE)
    target_link_libraries(video-playback-module
        ${VIDEOTOOLBOX}
        ${COREFOUNDATION}
        ${COREMEDIA}
        ${COREVIDEO}
    )
endif()

# Set RPATH for runtime linking
if(APPLE)
    # Find the actual Viam SDK library location
    get_target_property(VIAM_SDK_LIB viam-cpp-sdk::viamsdk LOCATION)
    get_filename_component(VIAM_SDK_LIB_DIR ${VIAM_SDK_LIB} DIRECTORY)
    
    set_target_properties(video-playback-module PROPERTIES
        BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "@loader_path/../lib;${VIAM_SDK_LIB_DIR};/opt/homebrew/lib;/usr/local/lib"
    )
    
    # Copy Viam SDK library to output directory
    add_custom_command(TARGET video-playback-module POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_SOURCE_DIR}/lib
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${VIAM_SDK_LIB} ${CMAKE_SOURCE_DIR}/lib/
        COMMENT "Copying Viam SDK library to lib directory"
    )
elseif(UNIX AND NOT APPLE)
    set_target_properties(video-playback-module PROPERTIES
        BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "$ORIGIN/../lib:/usr/local/lib:/usr/lib/aarch64-linux-gnu"
    )
endif()

# Copy to bin directory
add_custom_command(TARGET video-playback-module POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_SOURCE_DIR}/bin
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:video-playback-module> ${CMAKE_SOURCE_DIR}/bin/
    COMMENT "Copying executable to bin directory"
)

# Print configuration summary
message(STATUS "========================================")
message(STATUS "Video Playback Module Configuration")
message(STATUS "========================================")
message(STATUS "Platform: ${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR}")
message(STATUS "Compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
if(IS_JETSON)
    message(STATUS "Hardware acceleration: NVIDIA NVDEC (if available)")
elseif(APPLE)
    message(STATUS "Hardware acceleration: VideoToolbox")
else()
    message(STATUS "Hardware acceleration: None (software only)")
endif()
message(STATUS "Optimization flags: ${CMAKE_CXX_FLAGS}")
message(STATUS "========================================")