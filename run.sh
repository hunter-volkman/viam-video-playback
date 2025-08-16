#!/bin/bash
cd "$(dirname "$0")"

# Log startup for debugging
echo "Starting video-playback-module with args: $@" >> /tmp/video-playback.log

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
    # macOS-specific configuration if needed
else
    echo "Detected Linux platform" >> /tmp/video-playback.log
    # Generic Linux configuration
fi

# Run the module with all arguments passed from viam-server
exec ./bin/video-playback-module "$@"