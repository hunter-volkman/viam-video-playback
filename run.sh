#!/bin/bash
cd "$(dirname "$0")"

# Log startup for debugging
echo "Starting video-playback-module with args: $@" >> /tmp/video-playback.log

# Run the module with all arguments passed from viam-server
exec ./bin/video-playback-module "$@"
