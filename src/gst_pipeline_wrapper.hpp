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
 * GStreamer Pipeline Wrapper for Hardware-Accelerated Decode (Jetson)
 *
 * Decode-only pipeline (no JPEG encode in GStreamer):
 *   filesrc → qtdemux → queue → h26xparse → nvv4l2decoder → nvvidconv
 *          → video/x-raw,format=I420[,width=..,height=..][,framerate=..]
 *          [→ videorate drop-only=true → video/x-raw,framerate=N/1]
 *          → appsink
 *
 * Notes:
 *  - We explicitly repack frames to tight I420 in the appsink callback using
 *    GstVideoFrame stride information, then deliver that buffer to the caller.
 *  - If desired_fps > 0, we inject videorate (drop-only) to reduce CPU wakeups.
 *  - Scaling can be requested via desired_w / desired_h (handled by nvvidconv).
 */
class GstPipelineWrapper {
public:
    // data: tight I420 buffer (Y…U…V), size: w*h*3/2, width/height provided
    using FrameCallback = std::function<void(const uint8_t* data, size_t size, int width, int height)>;

    GstPipelineWrapper();
    ~GstPipelineWrapper();

    // Disable copy/move
    GstPipelineWrapper(const GstPipelineWrapper&)            = delete;
    GstPipelineWrapper& operator=(const GstPipelineWrapper&) = delete;

    /**
     * Start the GStreamer pipeline for a video file.
     * @param file_path   Path to the video file
     * @param cb          Callback to receive tight I420 frames
     * @param loop        Enable looping playback
     * @param max_buffers appsink queue depth (drop=true)
     * @param desired_w   Optional output width (0 = source)
     * @param desired_h   Optional output height (0 = source)
     * @param desired_fps Optional target FPS (0 = source). When set, we add videorate drop-only.
     * @return true on success
     */
    bool start(const std::string& file_path,
               FrameCallback cb,
               bool loop             = true,
               int  max_buffers      = 24,
               int  desired_w        = 0,
               int  desired_h        = 0,
               int  desired_fps      = 0);

    /** Stop the pipeline gracefully (blocking teardown). */
    void stop();

    /** Check if pipeline is running. */
    bool running() const { return running_.load(); }

    /** Frames processed so far (cumulative across loops). */
    uint64_t frames_processed() const { return frames_processed_.load(); }

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

    // Start-time tuning
    int desired_w_   = 0;
    int desired_h_   = 0;
    int desired_fps_ = 0;

    // Lock to serialize start/stop/restart operations
    std::mutex lifecycle_mtx_;

    // Helpers
    bool build_and_start_pipeline(int max_buffers, std::string* err_out);
    void cleanup_pipeline();     // blocking NULL + unref + remove bus watch
    bool restart_pipeline();     // safe blocking restart
    static std::string make_pipeline_str(const std::string& file_path,
                                         int max_buffers,
                                         int desired_w,
                                         int desired_h,
                                         int desired_fps);

    // GStreamer callbacks
    static GstFlowReturn on_new_sample(GstAppSink* sink, gpointer user_data);
    static gboolean      bus_callback(GstBus* bus, GstMessage* msg, gpointer user_data);

    // Bus handlers
    void handle_eos();
    void handle_error(GError* err, gchar* debug);

    // Loop helpers
    bool try_seek_to_start();    // pause → seek(0) → play
};
