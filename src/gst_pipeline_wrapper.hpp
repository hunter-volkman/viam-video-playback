#pragma once

#include <string>
#include <functional>
#include <atomic>
#include <cstdint>
#include <thread>
#include <chrono>

#include <gst/gst.h>
#include <gst/app/gstappsink.h>

/**
 * GStreamer Pipeline Wrapper for Hardware-Accelerated Video Playback
 * 
 * This wrapper manages a GStreamer pipeline optimized for NVIDIA Jetson platforms,
 * using hardware decoders (nvv4l2decoder) with software JPEG encoding (jpegenc)
 * to avoid libjpeg ABI mismatch issues.
 * 
 * Pipeline: filesrc → qtdemux → h264parse → nvv4l2decoder → nvvidconv → jpegenc → appsink
 */
class GstPipelineWrapper {
public:
    using FrameCallback = std::function<void(const uint8_t*, size_t)>;

    GstPipelineWrapper();
    ~GstPipelineWrapper();

    // Disable copy/move operations
    GstPipelineWrapper(const GstPipelineWrapper&) = delete;
    GstPipelineWrapper& operator=(const GstPipelineWrapper&) = delete;

    /**
     * Start the GStreamer pipeline for a video file
     * @param file_path Path to the video file (e.g., "/path/to/video.mp4")
     * @param cb Callback function to receive JPEG frames
     * @param loop Enable looping playback
     * @param max_buffers Maximum buffers in appsink queue
     * @return true if pipeline started successfully
     */
    bool start(const std::string& file_path, FrameCallback cb, 
               bool loop = true, int max_buffers = 4);

    /**
     * Stop the pipeline gracefully
     */
    void stop();

    /**
     * Check if pipeline is running
     */
    bool running() const { return running_.load(); }

    /**
     * Get number of frames processed
     */
    uint64_t frames_processed() const { return frames_processed_; }

private:
    GstElement* pipeline_ = nullptr;
    GstElement* appsink_ = nullptr;
    FrameCallback frame_cb_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> frames_processed_{0};
    bool loop_enabled_ = true;
    std::string file_path_;  // Store for restart
    guint bus_watch_id_ = 0;  // Bus watch ID for cleanup

    // Internal methods
    void cleanup_pipeline();
    bool restart_pipeline();

    // GStreamer callbacks
    static GstFlowReturn on_new_sample(GstAppSink* sink, gpointer user_data);
    static gboolean bus_callback(GstBus* bus, GstMessage* msg, gpointer user_data);
};