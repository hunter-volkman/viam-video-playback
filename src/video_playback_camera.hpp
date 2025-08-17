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

#if defined(USE_NVDEC)
class GstPipelineWrapper;
#endif

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/hwcontext.h>
}

namespace hunter {
namespace video_playback {

struct EncodingTask {
    AVFrame* frame;                                   // references memory owned by 'backing' if set
    std::shared_ptr<std::vector<uint8_t>> backing;    // optional owner of plane data (for Jetson I420 path)
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
    void start_pipeline();
    void stop_pipeline();

    // FFmpeg decode-only path (macOS/Linux x86)
    bool initialize_decoder(const std::string& path);
    void producer_thread_func();
    void receive_and_queue_frames(AVFrame* frame, AVFrame* sw_frame, std::chrono::high_resolution_clock::time_point& next_frame_time);

    // MJPEG encoder pool (shared)
    bool initialize_encoder_pool(int width, int height);
    void cleanup_encoder_pool();
    void consumer_thread_func(int thread_id);
    bool encode_task(int thread_id, EncodingTask& task, std::vector<uint8_t>& jpeg_buffer);

    // HW accel helpers
    AVBufferRef* hw_device_ctx_{nullptr};
    AVBufferRef* hw_frames_ctx_{nullptr};
    AVBSFContext* bsf_ctx_{nullptr};
    bool transfer_hw_frame_to_sw(AVFrame* src, AVFrame* dst);

    // --- Config ---
    std::string video_path_;
    bool   loop_playback_{true};
    int    target_fps_{0};
    int    quality_level_{15};
    int    output_width_{0};           // Jetson scaling
    int    output_height_{0};          // Jetson scaling
    int    appsink_max_buffers_{24};   // Jetson appsink queue

    // --- FFmpeg demux/decoder (non-Jetson path) ---
    AVFormatContext* format_ctx_{nullptr};
    AVCodecContext*  decoder_ctx_{nullptr};
    const AVCodec*   decoder_{nullptr};
    int              video_stream_index_{-1};

    // --- Shared producer/consumer queue ---
    std::queue<EncodingTask> frame_queue_;
    std::mutex               queue_mutex_;
    std::condition_variable  queue_producer_cv_;
    std::condition_variable  queue_consumer_cv_;
    size_t                   max_queue_size_{10};

    // --- MJPEG encoder threads ---
    int                       num_encoder_threads_{4};
    std::vector<std::thread>  encoder_threads_;
    std::vector<AVCodecContext*> mjpeg_encoder_ctxs_;
    std::vector<SwsContext*>     sws_contexts_;
    std::vector<AVFrame*>        yuv_frames_;

    // --- Latest JPEG for get_image ---
    std::mutex                      jpeg_mutex_;
    std::vector<uint8_t>            latest_jpeg_buffer_;
    bool                            is_jpeg_ready_{false};
    std::condition_variable         jpeg_ready_cv_;
    std::chrono::steady_clock::time_point last_frame_time_;

    std::atomic<bool> is_running_{false};
    std::thread       producer_thread_;  // only used in FFmpeg decode path

    std::atomic<uint64_t> frames_decoded_{0};
    std::atomic<uint64_t> frames_encoded_{0};
    std::atomic<uint64_t> frames_dropped_producer_{0};
    std::atomic<uint64_t> frames_dropped_consumer_{0};
    std::chrono::high_resolution_clock::time_point start_time_;

    double source_fps_{30.0};
    std::chrono::microseconds frame_duration_{std::chrono::microseconds(33333)};

#if defined(USE_NVDEC)
    std::unique_ptr<GstPipelineWrapper> gst_pipeline_;
#endif
};

} // namespace video_playback
} // namespace hunter
