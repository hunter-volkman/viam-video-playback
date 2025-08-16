#pragma once

#include <string>
#include <functional>
#include <atomic>
#include <cstdint>

#include <gst/gst.h>
#include <gst/app/gstappsink.h>

// Lightweight Gst wrapper for the module.
// Starts a pipeline: filesrc -> qtdemux -> h264parse -> nvv4l2decoder -> nvvidconv -> nvjpegenc -> appsink
// Calls the provided frame callback with (const uint8_t* data, size_t size) for each JPEG produced.
//
// The wrapper hides GStreamer initialization and pipeline lifecycle. The callback is invoked from a GStreamer thread.
class GstPipelineWrapper {
public:
    using FrameCallback = std::function<void(const uint8_t*, size_t)>;

    GstPipelineWrapper();
    ~GstPipelineWrapper();

    GstPipelineWrapper(const GstPipelineWrapper&) = delete;
    GstPipelineWrapper& operator=(const GstPipelineWrapper&) = delete;

    // Start the pipeline for a local file path (e.g., "/path/to/video.mp4")
    // Returns true if pipeline was created and set to PLAYING.
    bool start(const std::string& file_path, FrameCallback cb, bool loop = true, int max_buffers = 4);

    // Gracefully stop and cleanup.
    void stop();

    bool running() const { return running_.load(); }

private:
    GstElement* pipeline_ = nullptr;
    GstElement* appsink_ = nullptr;
    FrameCallback frame_cb_;
    std::atomic<bool> running_{false};

    // appsink callback
    static GstFlowReturn on_new_sample(GstAppSink* sink, gpointer user_data);
};
