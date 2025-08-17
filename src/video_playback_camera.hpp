#pragma once

#include <viam/sdk/components/camera.hpp>
#include <viam/sdk/resource/reconfigurable.hpp>
#include <viam/sdk/resource/resource.hpp>
#include <viam/sdk/config/resource.hpp>

#include <atomic>
#include <memory>
#include <thread>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <queue>
#include <array>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

#if defined(__aarch64__) && defined(__linux__)
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#endif

namespace hunter {
namespace video_playback {

// Lock-free ring buffer for zero-copy frame passing
template<typename T, size_t Size>
class RingBuffer {
public:
    bool try_push(T&& item) {
        size_t current_write = write_pos_.load(std::memory_order_relaxed);
        size_t next_write = (current_write + 1) % Size;
        
        if (next_write == read_pos_.load(std::memory_order_acquire)) {
            return false; // Buffer full
        }
        
        buffer_[current_write] = std::move(item);
        write_pos_.store(next_write, std::memory_order_release);
        return true;
    }
    
    bool try_pop(T& item) {
        size_t current_read = read_pos_.load(std::memory_order_relaxed);
        
        if (current_read == write_pos_.load(std::memory_order_acquire)) {
            return false; // Buffer empty
        }
        
        item = std::move(buffer_[current_read]);
        read_pos_.store((current_read + 1) % Size, std::memory_order_release);
        return true;
    }
    
    size_t size() const {
        size_t w = write_pos_.load(std::memory_order_acquire);
        size_t r = read_pos_.load(std::memory_order_acquire);
        return (w >= r) ? (w - r) : (Size - r + w);
    }

private:
    std::array<T, Size> buffer_;
    alignas(64) std::atomic<size_t> write_pos_{0};
    alignas(64) std::atomic<size_t> read_pos_{0};
};

struct FrameData {
    std::shared_ptr<std::vector<uint8_t>> data;
    int width;
    int height;
    int64_t pts;
};

class VideoPlaybackCamera : public viam::sdk::Camera, public viam::sdk::Reconfigurable {
public:
    VideoPlaybackCamera(const viam::sdk::Dependencies& deps, const viam::sdk::ResourceConfig& cfg);
    ~VideoPlaybackCamera() override;

    viam::sdk::Camera::raw_image get_image(std::string mime_type, const viam::sdk::ProtoStruct& extra) override;
    viam::sdk::Camera::image_collection get_images() override;
    viam::sdk::Camera::point_cloud get_point_cloud(std::string mime_type, const viam::sdk::ProtoStruct& extra) override;
    viam::sdk::Camera::properties get_properties() override;

    void reconfigure(const viam::sdk::Dependencies& deps, const viam::sdk::ResourceConfig& cfg) override;

    viam::sdk::ProtoStruct do_command(const viam::sdk::ProtoStruct& command) override;
    std::vector<viam::sdk::GeometryConfig> get_geometries(const viam::sdk::ProtoStruct& extra) override;

    static std::shared_ptr<viam::sdk::Resource> create(const viam::sdk::Dependencies& deps, const viam::sdk::ResourceConfig& cfg);
    static viam::sdk::Model model();

private:
    // Core pipeline management
    void start_pipeline();
    void stop_pipeline();
    
#if defined(__aarch64__) && defined(__linux__)
    // Jetson-specific GStreamer pipeline
    void setup_gstreamer_pipeline();
    static GstFlowReturn on_new_sample(GstAppSink* sink, gpointer user_data);
    static gboolean bus_callback(GstBus* bus, GstMessage* msg, gpointer user_data);
    void handle_gst_frame(const uint8_t* data, size_t size, int width, int height);
    
    GstElement* pipeline_{nullptr};
    GstElement* appsink_{nullptr};
    guint bus_watch_id_{0};
#else
    // FFmpeg software decode path
    bool initialize_decoder(const std::string& path);
    void decode_thread_func();
#endif

    // Shared JPEG encoding
    bool initialize_encoder_pool(int width, int height);
    void encoder_thread_func(int thread_id);
    bool encode_frame_to_jpeg(AVFrame* frame, int thread_id, std::vector<uint8_t>& output);
    
    // Configuration
    std::string video_path_;
    bool loop_playback_{true};
    int target_fps_{0};
    int quality_level_{12};
    int output_width_{1280};
    int output_height_{720};
    
    // Performance tuning
    static constexpr size_t FRAME_BUFFER_SIZE = 8;
    static constexpr int MAX_ENCODER_THREADS = 4;
    
    // Lock-free frame queue for maximum throughput
    RingBuffer<FrameData, FRAME_BUFFER_SIZE> frame_queue_;
    
    // JPEG encoder pool
    int num_encoder_threads_;
    std::vector<std::thread> encoder_threads_;
    std::vector<AVCodecContext*> mjpeg_contexts_;
    std::vector<AVFrame*> conversion_frames_;
    std::vector<SwsContext*> sws_contexts_;
    
    // Latest JPEG frame (double buffered)
    std::mutex jpeg_mutex_;
    std::vector<uint8_t> front_buffer_;
    std::vector<uint8_t> back_buffer_;
    std::atomic<bool> new_frame_ready_{false};
    std::chrono::steady_clock::time_point last_frame_time_;
    
    // Pipeline state
    std::atomic<bool> running_{false};
    std::thread decode_thread_;
    
    // Performance metrics
    std::atomic<uint64_t> frames_decoded_{0};
    std::atomic<uint64_t> frames_encoded_{0};
    std::atomic<uint64_t> frames_dropped_{0};
    std::chrono::high_resolution_clock::time_point start_time_;
    
    // FFmpeg components (non-Jetson)
#if !defined(__aarch64__) || !defined(__linux__)
    AVFormatContext* format_ctx_{nullptr};
    AVCodecContext* decoder_ctx_{nullptr};
    const AVCodec* decoder_{nullptr};
    int video_stream_index_{-1};
#endif
};

} // namespace video_playback
} // namespace hunter