#pragma once

#include <string>
#include <functional>
#include <atomic>
#include <cstdint>
#include <thread>
#include <chrono>
#include <mutex>

#include <gst/gst.h>
#include <gst/app/gstappsink.h>

/**
 * GStreamer Pipeline Wrapper for Hardware-Accelerated Video Playback (Jetson)
 *
 * Decode-only pipeline (NO in-process JPEG encoding):
 *   filesrc → qtdemux → queue → h264parse → nvv4l2decoder → nvvidconv → video/x-raw,format=I420 → appsink
 *
 * Looping Strategy:
 *   1) Try post-EOS seek (pause → seek-to-0 (flush|key-unit) → play)
 *   2) Fallback to full pipeline restart with strict teardown ordering
 */
class GstPipelineWrapper {
public:
    // I420 raw frame callback: data is a single contiguous I420 buffer (Y plane, U plane, V plane)
    // with tight packing (stride == width). size == width*height + (width/2)*(height/2)*2.
    using FrameCallback = std::function<void(const uint8_t* data, size_t size, int width, int height)>;

    GstPipelineWrapper();
    ~GstPipelineWrapper();

    // Disable copy/move
    GstPipelineWrapper(const GstPipelineWrapper&) = delete;
    GstPipelineWrapper& operator=(const GstPipelineWrapper&) = delete;

    /**
     * Start the GStreamer pipeline for a video file (decode-only).
     * @param file_path Path to the video file
     * @param cb Callback function to receive I420 frames (contiguous, tight stride)
     * @param loop Enable looping playback
     * @param max_buffers Maximum buffers in appsink queue
     * @return true if pipeline started successfully
     */
    bool start(const std::string& file_path, FrameCallback cb,
               bool loop = true, int max_buffers = 8);

    /** Stop the pipeline gracefully (blocking teardown). */
    void stop();

    /** Check if pipeline is running. */
    bool running() const { return running_.load(); }

    /** Frames processed so far (cumulative across loops). */
    uint64_t frames_processed() const { return frames_processed_; }

    /** Output dimensions (best-effort; populated from first sample caps). */
    int width()  const { return width_; }
    int height() const { return height_; }

private:
    // Pipeline objects
    GstElement* pipeline_ = nullptr;
    GstElement* appsink_  = nullptr;
    guint       bus_watch_id_ = 0;

    // State
    FrameCallback         frame_cb_;
    std::atomic<bool>     running_{false};
    std::atomic<uint64_t> frames_processed_{0};
    bool                  loop_enabled_ = true;
    std::string           file_path_;
    int                   width_  = 0;
    int                   height_ = 0;

    // Lock to serialize start/stop/restart operations
    std::mutex            lifecycle_mtx_;

    // Internal helpers
    bool build_and_start_pipeline(int max_buffers, std::string* err_out);
    void cleanup_pipeline();     // blocking NULL + unref + remove bus watch
    bool restart_pipeline();     // safe blocking restart
    static std::string make_pipeline_str(const std::string& file_path, int max_buffers);

    // GStreamer callbacks
    static GstFlowReturn on_new_sample(GstAppSink* sink, gpointer user_data);
    static gboolean bus_callback(GstBus* bus, GstMessage* msg, gpointer user_data);

    // Bus handlers
    void handle_eos();
    void handle_error(GError* err, gchar* debug);

    // Loop helpers
    bool try_seek_to_start();    // pause → seek(0) → play
};
