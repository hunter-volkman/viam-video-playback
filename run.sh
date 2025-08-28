#!/bin/bash
cd "$(dirname "$0")"

# Log startup for debugging
echo "Starting video-playback-module with args: $@" >> /tmp/video-playback.log

# Add bundled libraries to path first
export LD_LIBRARY_PATH="$PWD/lib:${LD_LIBRARY_PATH}"

# Platform detection
if [ -f /etc/nv_tegra_release ]; then
    echo "Detected NVIDIA Jetson platform" >> /tmp/video-playback.log
    
    # Jetson-specific configuration
    # Ensure GStreamer can find NVIDIA plugins
    export GST_PLUGIN_PATH="/usr/lib/aarch64-linux-gnu/gstreamer-1.0:${GST_PLUGIN_PATH}"
    
    # Add Tegra libraries for hardware acceleration
    export LD_LIBRARY_PATH="/usr/lib/aarch64-linux-gnu/tegra:${LD_LIBRARY_PATH}"
    
elif [ "$(uname)" = "Darwin" ]; then
    echo "Detected macOS platform" >> /tmp/video-playback.log
    # macOS uses DYLD_LIBRARY_PATH
    export DYLD_LIBRARY_PATH="$PWD/lib:${DYLD_LIBRARY_PATH}"
else
    echo "Detected Linux platform" >> /tmp/video-playback.log
    # Generic Linux configuration
fi

# Check for missing libraries and log them
if command -v ldd >/dev/null 2>&1; then
    missing=$(ldd ./bin/video-playback-module 2>/dev/null | grep "not found" | awk '{print $1}')
    if [ -n "$missing" ]; then
        echo "Warning: Missing libraries: $missing" >> /tmp/video-playback.log
    fi
fi

# Run the module with all arguments passed from viam-server
exec ./bin/video-playback-module "$@"