cmake_minimum_required(VERSION 3.25)
project(viam-video-stream)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Performance optimizations
if(CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
    # Apple Silicon optimizations
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -mcpu=apple-m1 -mtune=apple-m1")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -flto -ffast-math -funroll-loops")
else()
    # x86_64 optimizations
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -march=native -mtune=native")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -flto -ffast-math -funroll-loops")
endif()

# Enable SIMD and vectorization
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fvectorize -fslp-vectorize")

# Set RPATH for macOS
set(CMAKE_MACOSX_RPATH ON)
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)

# Find packages
find_package(viam-cpp-sdk REQUIRED)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)

# FFmpeg with all components for hardware acceleration
pkg_check_modules(FFMPEG REQUIRED 
    libavformat 
    libavcodec 
    libavutil 
    libswscale
    libavfilter
)

# macOS VideoToolbox and other hardware acceleration
if(APPLE)
    add_definitions(-DUSE_VIDEOTOOLBOX)
    find_library(VIDEOTOOLBOX VideoToolbox REQUIRED)
    find_library(COREFOUNDATION CoreFoundation REQUIRED)
    find_library(COREMEDIA CoreMedia REQUIRED)
    find_library(COREVIDEO CoreVideo REQUIRED)
    find_library(SECURITY Security REQUIRED)
    find_library(CORESERVICES CoreServices REQUIRED)
endif()

# Linux VAAPI/VDPAU support (for future Raspberry Pi/Jetson)
if(LINUX)
    # These are optional for now
    find_package(PkgConfig)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(VAAPI libva)
        pkg_check_modules(VDPAU vdpau)
        if(VAAPI_FOUND)
            add_definitions(-DUSE_VAAPI)
        endif()
        if(VDPAU_FOUND)
            add_definitions(-DUSE_VDPAU)
        endif()
    endif()
endif()

# Create executable
add_executable(video-stream-module
    src/main.cpp
    src/video_stream_camera.cpp
)

target_include_directories(video-stream-module PRIVATE
    ${FFMPEG_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_directories(video-stream-module PRIVATE ${FFMPEG_LIBRARY_DIRS})

# Link libraries with optimized linking
target_link_libraries(video-stream-module
    viam-cpp-sdk::viamsdk
    ${FFMPEG_LIBRARIES}
    Threads::Threads
)

# Platform-specific hardware acceleration libraries
if(APPLE)
    target_link_libraries(video-stream-module
        ${VIDEOTOOLBOX}
        ${COREFOUNDATION}
        ${COREMEDIA}
        ${COREVIDEO}
        ${SECURITY}
        ${CORESERVICES}
    )
    
    # Optimized RPATH for macOS
    set_target_properties(video-stream-module PROPERTIES
        BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "@loader_path/../lib;/opt/homebrew/lib;/usr/local/lib"
    )
endif()

if(LINUX)
    if(VAAPI_FOUND)
        target_link_libraries(video-stream-module ${VAAPI_LIBRARIES})
    endif()
    if(VDPAU_FOUND)
        target_link_libraries(video-stream-module ${VDPAU_LIBRARIES})
    endif()
    
    # Linux RPATH settings
    set_target_properties(video-stream-module PROPERTIES
        BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "$ORIGIN/../lib:/usr/local/lib:/usr/lib"
    )
endif()

# Additional compiler-specific optimizations
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    # Clang-specific optimizations
    target_compile_options(video-stream-module PRIVATE
        -fno-omit-frame-pointer
        -fno-exceptions  # We don't use C++ exceptions in the hot path
    )
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # GCC-specific optimizations
    target_compile_options(video-stream-module PRIVATE
        -fno-omit-frame-pointer
        -fno-exceptions
        -ftree-vectorize
    )
endif()

# Copy to bin directory after building
add_custom_command(TARGET video-stream-module POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_SOURCE_DIR}/bin
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:video-stream-module> ${CMAKE_SOURCE_DIR}/bin/
    COMMENT "Copying executable to bin directory"
)

# Optional: Create debug version with symbols
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(video-stream-module PRIVATE -g -O0)
    target_compile_definitions(video-stream-module PRIVATE DEBUG=1)
else()
    # Release optimizations
    target_compile_definitions(video-stream-module PRIVATE NDEBUG=1)
    # Strip symbols for smaller binary
    if(NOT APPLE)
        target_link_options(video-stream-module PRIVATE -s)
    endif()
endif()

# Print configuration summary
message(STATUS "=== Video Stream Module Configuration ===")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "System: ${CMAKE_SYSTEM_NAME}")
message(STATUS "Processor: ${CMAKE_SYSTEM_PROCESSOR}")
message(STATUS "Compiler: ${CMAKE_CXX_COMPILER_ID}")
message(STATUS "FFmpeg libraries: ${FFMPEG_LIBRARIES}")
if(APPLE)
    message(STATUS "VideoToolbox: Enabled")
endif()
if(VAAPI_FOUND)
    message(STATUS "VAAPI: Enabled")
endif()
if(VDPAU_FOUND)
    message(STATUS "VDPAU: Enabled")
endif()
message(STATUS "===========================================")
