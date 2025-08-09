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
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/hwcontext.h>
}

namespace viam {
namespace video_stream {

class FramePool;

class VideoStreamCamera : public sdk::Camera, public sdk::Reconfigurable {
public:
    VideoStreamCamera(const sdk::Dependencies& deps, const sdk::ResourceConfig& cfg);
    ~VideoStreamCamera();

    // Camera API implementation
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
    
    // --- MJPEG Encoding Pipeline ---
    bool init_mjpeg_encoder(int w, int h);
    bool encode_frame_to_jpeg(AVFrame* src, std::vector<uint8_t>& out_jpeg);
    bool ensure_yuvj_frame_from_current(AVFrame* src, AVFrame** out);
    
    // --- Hardware Acceleration (VideoToolbox) ---
#ifdef USE_VIDEOTOOLBOX
    AVBufferRef* hw_device_ctx_{nullptr};
    static enum AVPixelFormat get_hw_format(AVCodecContext* ctx, const enum AVPixelFormat* pix_fmts);
    bool init_hw_device();
    bool transfer_hwframe_if_needed(AVFrame* src, AVFrame* dst);
#endif
    
    // Configuration
    std::string video_path_;
    bool loop_playback_{true};
    int target_fps_{0};
    
    // FFmpeg decoder structures
    AVFormatContext* format_ctx_{nullptr};
    AVCodecContext* codec_ctx_{nullptr};
    const AVCodec* codec_{nullptr};
    int video_stream_index_{-1};
    
    // MJPEG encoder structures
    AVCodecContext* mjpeg_enc_ctx_{nullptr};
    const AVCodec* mjpeg_enc_{nullptr};
    SwsContext* sws_to_yuvj_ctx_{nullptr};
    AVFrame* yuvj_frame_{nullptr};
    
    // Frame management
    std::unique_ptr<FramePool> frame_pool_;
    std::mutex frame_mutex_;
    std::condition_variable frame_cv_;
    AVFrame* current_frame_{nullptr};
    
    // Pre-encoded JPEG cache for blazing fast get_image()
    std::mutex jpeg_mutex_;
    std::vector<uint8_t> current_jpeg_;
    bool jpeg_ready_{false};
    
    // Thread control
    std::atomic<bool> running_{false};
    std::thread decode_thread_;
    
    // Performance metrics
    std::atomic<uint64_t> frames_decoded_{0};
    std::atomic<uint64_t> frames_dropped_{0};
    std::atomic<uint64_t> frames_encoded_{0};
    std::chrono::high_resolution_clock::time_point start_time_;
    
    // Frame timing
    double source_fps_{30.0};
    std::chrono::microseconds frame_duration_;
    std::chrono::high_resolution_clock::time_point last_frame_time_;
};

} // namespace video_stream
} // namespace viam