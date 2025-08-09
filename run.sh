#!/bin/bash
cd "$(dirname "$0")"

# Log startup for debugging
echo "Starting video-stream-module with args: $@" >> /tmp/video-stream.log

# Run the module with all arguments passed from viam-server
exec ./bin/video-stream-module "$@"
