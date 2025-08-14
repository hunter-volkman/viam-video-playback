#!/bin/bash
cd "$(dirname "$0")"

# Log startup for debugging
echo "Starting video-playback-module with args: $@" >> /tmp/video-playback.log

# Prepend NVIDIA Tegra libraries to the library path
# Allow FFmpeg's NVDEC to find the necessary backend driver libraries
export LD_LIBRARY_PATH=/usr/lib/aarch64-linux-gnu/tegra:$LD_LIBRARY_PATH

# Run the module with all arguments passed from viam-server
exec ./bin/video-playback-module "$@"