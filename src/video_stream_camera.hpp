#pragma once

#include <viam/sdk/components/camera.hpp>
#include <viam/sdk/resource/reconfigurable.hpp>
#include <viam/sdk/config/resource.hpp>
#include <atomic>
#include <memory>
#include <thread>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <queue>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/hwcontext.h>
}

namespace viam {
namespace video_stream {

/**
 * @struct EncodingTask
 * @brief Represents a unit of work for an encoder thread.
 *
 * Contains a raw, decoded AVFrame that needs to be encoded into a JPEG.
 */
struct EncodingTask {
    AVFrame* frame;
};

/**
 * @class VideoStreamCamera
 * @brief A high-performance Viam camera that replays a video file using a parallel architecture.
 *
 * This component uses a producer-consumer model to achieve high frame rates.
 * A single producer thread decodes the video, and a pool of consumer threads
 * handles the computationally expensive JPEG encoding in parallel.
 */
class VideoStreamCamera : public sdk::Camera, public sdk::Reconfigurable {
public:
    VideoStreamCamera(const sdk::Dependencies& deps, const sdk::ResourceConfig& cfg);
    ~VideoStreamCamera() override;

    // Viam Camera API
    sdk::Camera::raw_image get_image(std::string mime_type, const sdk::ProtoStruct& extra) override;
    sdk::Camera::image_collection get_images() override;
    sdk::Camera::point_cloud get_point_cloud(std::string mime_type, const sdk::ProtoStruct& extra) override;
    sdk::Camera::properties get_properties() override;
    
    // Viam Reconfigurable
    void reconfigure(const sdk::Dependencies& deps, const sdk::ResourceConfig& cfg) override;
    
    // Viam Resource API
    sdk::ProtoStruct do_command(const sdk::ProtoStruct& command) override;
    std::vector<sdk::GeometryConfig> get_geometries(const sdk::ProtoStruct& extra) override;

    // Factory methods
    static std::shared_ptr<sdk::Resource> create(const sdk::Dependencies& deps, const sdk::ResourceConfig& cfg);
    static sdk::Model model();

private:
    // Core pipeline control
    void start_pipeline();
    void stop_pipeline();
    bool initialize_decoder(const std::string& path);
    
    // Producer thread (decoding)
    void producer_thread_func();
    
    // Consumer threads (encoding)
    bool initialize_encoder_pool(int width, int height);
    void cleanup_encoder_pool();
    void consumer_thread_func(int thread_id);
    bool encode_task(int thread_id, EncodingTask& task, std::vector<uint8_t>& jpeg_buffer);
    
    // Hardware Acceleration (VideoToolbox for macOS)
#ifdef USE_VIDEOTOOLBOX
    AVBufferRef* hw_device_ctx_{nullptr};
    static enum AVPixelFormat get_hw_format(AVCodecContext* ctx, const enum AVPixelFormat* pix_fmts);
    bool initialize_hw_decoder_internal();
    bool transfer_hw_frame_to_sw(AVFrame* src, AVFrame* dst);
#endif

    // --- Member Variables ---

    // Configuration
    std::string video_path_;
    bool loop_playback_{true};
    int target_fps_{0};
    int quality_level_{15}; // JPEG quality (1=best, 31=worst). Higher value for more speed.
    
    // FFmpeg decoder
    AVFormatContext* format_ctx_{nullptr};
    AVCodecContext* decoder_ctx_{nullptr};
    const AVCodec* decoder_{nullptr};
    int video_stream_index_{-1};
    
    // Producer-Consumer Queue
    std::queue<EncodingTask> frame_queue_;
    std::mutex queue_mutex_;
    std::condition_variable queue_producer_cv_;
    std::condition_variable queue_consumer_cv_;
    size_t max_queue_size_{10};
    
    // Consumer (Encoder) Pool
    int num_encoder_threads_{4};
    std::vector<std::thread> encoder_threads_;
    std::vector<AVCodecContext*> mjpeg_encoder_ctxs_;
    std::vector<SwsContext*> sws_contexts_;
    std::vector<AVFrame*> yuv_frames_;
    
    // State for latest available JPEG
    std::mutex jpeg_mutex_;
    std::vector<uint8_t> latest_jpeg_buffer_;
    bool is_jpeg_ready_{false};
    std::condition_variable jpeg_ready_cv_;
    
    // Thread control
    std::atomic<bool> is_running_{false};
    std::thread producer_thread_;
    
    // Performance metrics
    std::atomic<uint64_t> frames_decoded_{0};
    std::atomic<uint64_t> frames_encoded_{0};
    std::atomic<uint64_t> frames_dropped_producer_{0};
    std::atomic<uint64_t> frames_dropped_consumer_{0};
    std::chrono::high_resolution_clock::time_point start_time_;
    
    // Frame timing
    double source_fps_{30.0};
    std::chrono::microseconds frame_duration_;
};

} // namespace video_stream
} // namespace viam
