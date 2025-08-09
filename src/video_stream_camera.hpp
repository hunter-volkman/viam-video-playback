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

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

namespace viam {
namespace video_stream {

class FramePool;

class VideoStreamCamera : public sdk::Camera, public sdk::Reconfigurable {
public:
    VideoStreamCamera(const sdk::Dependencies& deps, const sdk::ResourceConfig& cfg);
    ~VideoStreamCamera();

    // Camera API implementation - matching exact signatures from SDK
    sdk::Camera::raw_image get_image(std::string mime_type, const sdk::ProtoStruct& extra) override;
    sdk::Camera::image_collection get_images() override;
    sdk::Camera::point_cloud get_point_cloud(std::string mime_type, const sdk::ProtoStruct& extra) override;
    sdk::Camera::properties get_properties() override;
    
    // Reconfigurable
    void reconfigure(const sdk::Dependencies& deps, const sdk::ResourceConfig& cfg) override;
    
    // Resource interface
    sdk::ProtoStruct do_command(const sdk::ProtoStruct& command) override;
    std::vector<sdk::GeometryConfig> get_geometries(const sdk::ProtoStruct& extra) override;

    // Factory methods
    static std::shared_ptr<sdk::Resource> create(const sdk::Dependencies& deps,
                                                  const sdk::ResourceConfig& cfg);
    static sdk::Model model();

private:
    void decode_thread();
    void start_streaming();
    void stop_streaming();
    bool open_video_file(const std::string& path);
    
    // Configuration
    std::string video_path_;
    bool loop_playback_{true};
    int target_fps_{0};
    
    // FFmpeg structures
    AVFormatContext* format_ctx_{nullptr};
    AVCodecContext* codec_ctx_{nullptr};
    const AVCodec* codec_{nullptr};
    int video_stream_index_{-1};
    SwsContext* sws_ctx_{nullptr};
    
    // Frame management
    std::unique_ptr<FramePool> frame_pool_;
    std::mutex frame_mutex_;
    std::condition_variable frame_cv_;
    AVFrame* current_frame_{nullptr};
    
    // Thread control
    std::atomic<bool> running_{false};
    std::thread decode_thread_;
    
    // Performance metrics
    std::atomic<uint64_t> frames_decoded_{0};
    std::atomic<uint64_t> frames_dropped_{0};
    std::chrono::high_resolution_clock::time_point start_time_;
    
    // Frame timing
    double source_fps_{30.0};
    std::chrono::microseconds frame_duration_;
    std::chrono::high_resolution_clock::time_point last_frame_time_;
};

} // namespace video_stream
} // namespace viam
