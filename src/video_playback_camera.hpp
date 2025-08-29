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
}

namespace hunter {
namespace video_playback {

struct EncodingTask {
    AVFrame* frame = nullptr;
};

class VideoPlaybackCamera : public viam::sdk::Camera, 
                           public viam::sdk::Reconfigurable {
public:
    VideoPlaybackCamera(const viam::sdk::Dependencies& deps, 
                       const viam::sdk::ResourceConfig& cfg);
    ~VideoPlaybackCamera() override;

    // Camera interface
    viam::sdk::Camera::raw_image get_image(
        std::string mime_type, 
        const viam::sdk::ProtoStruct& extra) override;
    
    viam::sdk::Camera::image_collection get_images() override;
    
    viam::sdk::Camera::point_cloud get_point_cloud(
        std::string mime_type, 
        const viam::sdk::ProtoStruct& extra) override;
    
    viam::sdk::Camera::properties get_properties() override;
    
    // Reconfigure
    void reconfigure(const viam::sdk::Dependencies& deps, 
                    const viam::sdk::ResourceConfig& cfg) override;
    
    // Additional methods
    viam::sdk::ProtoStruct do_command(
        const viam::sdk::ProtoStruct& command) override;
    
    std::vector<viam::sdk::GeometryConfig> get_geometries(
        const viam::sdk::ProtoStruct& extra) override;

    // Factory methods
    static std::shared_ptr<viam::sdk::Resource> create(
        const viam::sdk::Dependencies& deps, 
        const viam::sdk::ResourceConfig& cfg);
    
    static viam::sdk::Model model();

private:
    // Pipeline control
    void start_pipeline();
    void stop_pipeline();
    
    // Initialization
    bool initialize_decoder(const std::string& path);
    bool initialize_encoder_pool(int width, int height);
    void cleanup_encoder_pool();
    
    // Thread functions
    void producer_thread_func();
    void consumer_thread_func(int thread_id);
    bool encode_task(int thread_id, EncodingTask& task, 
                    std::vector<uint8_t>& jpeg_buffer);
    
    // Configuration
    std::string video_path_;
    bool loop_playback_ = true;
    int target_fps_ = 0;
    int quality_level_ = 15;
    int max_resolution_ = 0;
    bool use_hardware_acceleration_ = false; 
    
    // Output dimensions
    int output_width_ = 0;
    int output_height_ = 0;
    
    // Decoder state
    AVFormatContext* format_ctx_ = nullptr;
    AVCodecContext* decoder_ctx_ = nullptr;
    const AVCodec* decoder_ = nullptr;
    int video_stream_index_ = -1;
    
    // Encoder pool
    std::vector<AVCodecContext*> mjpeg_encoder_ctxs_;
    std::vector<SwsContext*> sws_contexts_;
    std::vector<AVFrame*> yuv_frames_;
    int num_encoder_threads_ = 4;
    std::vector<AVPacket*> encoder_packets_;
    std::vector<std::vector<uint8_t>> jpeg_buffers_;
    
    // Frame queue
    std::queue<EncodingTask> frame_queue_;
    std::mutex queue_mutex_;
    std::condition_variable queue_producer_cv_;
    std::condition_variable queue_consumer_cv_;
    size_t max_queue_size_ = 10;
    
    // Output buffer
    std::mutex jpeg_mutex_;
    std::vector<uint8_t> latest_jpeg_buffer_;
    bool is_jpeg_ready_ = false;
    std::condition_variable jpeg_ready_cv_;
    
    // Threading
    std::atomic<bool> is_running_{false};
    std::thread producer_thread_;
    std::vector<std::thread> encoder_threads_;
    
    // Statistics
    std::atomic<uint64_t> frames_decoded_{0};
    std::atomic<uint64_t> frames_encoded_{0};
    std::atomic<uint64_t> frames_dropped_producer_{0};
    std::atomic<uint64_t> frames_dropped_consumer_{0};
    
    // Timing
    double source_fps_ = 30.0;
    std::chrono::microseconds frame_duration_;
    std::chrono::high_resolution_clock::time_point start_time_;
    std::chrono::steady_clock::time_point last_frame_time_;
};

} // namespace video_playback
} // namespace hunter