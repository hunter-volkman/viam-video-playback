#include "video_playback_camera.hpp"

#include <viam/sdk/common/exception.hpp>
#include <viam/sdk/common/proto_value.hpp>
#include <iostream>
#include <vector>
#include <string>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
extern "C" {
#include <libavutil/opt.h>
}
#pragma GCC diagnostic pop

namespace hunter {
namespace video_playback {

namespace vs = viam::sdk;

vs::Model VideoPlaybackCamera::model() {
    return vs::Model{"hunter", "video-playback", "camera"};
}

std::shared_ptr<vs::Resource> VideoPlaybackCamera::create(const vs::Dependencies& deps,
                                                         const vs::ResourceConfig& cfg) {
    return std::make_shared<VideoPlaybackCamera>(deps, cfg);
}

VideoPlaybackCamera::VideoPlaybackCamera(const vs::Dependencies& deps, const vs::ResourceConfig& cfg)
    : vs::Camera(cfg.name()) {
    num_encoder_threads_ = std::max(2, static_cast<int>(std::thread::hardware_concurrency() / 2));
    std::cout << "Initializing VideoPlaybackCamera: " << cfg.name() << std::endl;
    reconfigure(deps, cfg);
}

VideoPlaybackCamera::~VideoPlaybackCamera() {
    stop_pipeline();
}

void VideoPlaybackCamera::reconfigure(const vs::Dependencies& deps, const vs::ResourceConfig& cfg) {
    stop_pipeline();
    
    auto attrs = cfg.attributes();
    
    if (attrs.find("video_path") == attrs.end()) {
        throw vs::Exception("`video_path` attribute is required.");
    }
    video_path_ = *attrs.at("video_path").get<std::string>();
    
    if (attrs.find("loop") != attrs.end()) {
        loop_playback_ = *attrs.at("loop").get<bool>();
    }
    if (attrs.find("target_fps") != attrs.end()) {
        target_fps_ = static_cast<int>(*attrs.at("target_fps").get<double>());
    }
    if (attrs.find("jpeg_quality_level") != attrs.end()) {
        quality_level_ = static_cast<int>(*attrs.at("jpeg_quality_level").get<double>());
    }
    
    std::cout << "Reconfiguring video playback:" << std::endl;
    std::cout << "  - Path: " << video_path_ << std::endl;
    std::cout << "  - Loop: " << (loop_playback_ ? "yes" : "no") << std::endl;
    std::cout << "  - Target FPS: " << (target_fps_ > 0 ? std::to_string(target_fps_) : "source") << std::endl;
    std::cout << "  - JPEG Quality Level: " << quality_level_ << std::endl;

    if (!initialize_decoder(video_path_)) {
        throw vs::Exception("Failed to initialize video decoder for: " + video_path_);
    }
    
    if (!initialize_encoder_pool(decoder_ctx_->width, decoder_ctx_->height)) {
        throw vs::Exception("Failed to initialize JPEG encoder pool.");
    }
    
    start_pipeline();
}

bool VideoPlaybackCamera::initialize_decoder(const std::string& path) {
    if (avformat_open_input(&format_ctx_, path.c_str(), nullptr, nullptr) < 0) return false;
    if (avformat_find_stream_info(format_ctx_, nullptr) < 0) return false;

    video_stream_index_ = av_find_best_stream(format_ctx_, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder_, 0);
    if (video_stream_index_ < 0) return false;

    AVStream* video_stream = format_ctx_->streams[video_stream_index_];
    source_fps_ = av_q2d(video_stream->r_frame_rate);
    
    decoder_ctx_ = avcodec_alloc_context3(decoder_);
    avcodec_parameters_to_context(decoder_ctx_, video_stream->codecpar);
    
    initialize_hw_decoder();
    
    decoder_ctx_->thread_count = std::max(1u, std::thread::hardware_concurrency());
    decoder_ctx_->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    decoder_ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
    
    if (avcodec_open2(decoder_ctx_, decoder_, nullptr) < 0) return false;
    
    double fps = (target_fps_ > 0) ? target_fps_ : source_fps_;
    frame_duration_ = std::chrono::microseconds(static_cast<int64_t>(1000000.0 / fps));

    std::cout << "Decoder initialized successfully:" << std::endl;
    std::cout << "  - Resolution: " << decoder_ctx_->width << "x" << decoder_ctx_->height << std::endl;
    std::cout << "  - Pacing at " << fps << " FPS" << std::endl;
    std::cout << "  - Hardware acceleration: " << (decoder_ctx_->hw_device_ctx ? "Enabled" : "Software") << std::endl;

    return true;
}

bool VideoPlaybackCamera::initialize_encoder_pool(int width, int height) {
    std::cout << "Initializing encoder pool with " << num_encoder_threads_ << " threads." << std::endl;
    const AVCodec* mjpeg_encoder = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if (!mjpeg_encoder) return false;

    mjpeg_encoder_ctxs_.resize(num_encoder_threads_);
    sws_contexts_.resize(num_encoder_threads_, nullptr);
    yuv_frames_.resize(num_encoder_threads_);

    for (int i = 0; i < num_encoder_threads_; ++i) {
        mjpeg_encoder_ctxs_[i] = avcodec_alloc_context3(mjpeg_encoder);
        AVCodecContext* ctx = mjpeg_encoder_ctxs_[i];
        ctx->pix_fmt = AV_PIX_FMT_YUVJ420P;
        ctx->width = width;
        ctx->height = height;
        ctx->time_base = AVRational{1, (target_fps_ > 0) ? target_fps_ : static_cast<int>(source_fps_)};
        
        AVDictionary* opts = nullptr;
        av_dict_set(&opts, "strict", "-2", 0);
        
        if (avcodec_open2(ctx, mjpeg_encoder, &opts) < 0) {
            av_dict_free(&opts);
            return false;
        }
        av_dict_free(&opts);

        av_opt_set(ctx->priv_data, "q", std::to_string(quality_level_).c_str(), 0);

        yuv_frames_[i] = av_frame_alloc();
        yuv_frames_[i]->format = AV_PIX_FMT_YUVJ420P;
        yuv_frames_[i]->width = width;
        yuv_frames_[i]->height = height;
        if (av_frame_get_buffer(yuv_frames_[i], 32) < 0) return false;
    }
    return true;
}

void VideoPlaybackCamera::cleanup_encoder_pool() {
    for (auto& ctx : mjpeg_encoder_ctxs_) if (ctx) avcodec_free_context(&ctx);
    for (auto& frame : yuv_frames_) if (frame) av_frame_free(&frame);
    for (auto& sws_ctx : sws_contexts_) if (sws_ctx) sws_freeContext(sws_ctx);
    mjpeg_encoder_ctxs_.clear();
    yuv_frames_.clear();
    sws_contexts_.clear();
}

void VideoPlaybackCamera::start_pipeline() {
    if (is_running_) return;
    is_running_ = true;
    start_time_ = std::chrono::high_resolution_clock::now();
    
    producer_thread_ = std::thread(&VideoPlaybackCamera::producer_thread_func, this);
    for (int i = 0; i < num_encoder_threads_; ++i) {
        encoder_threads_.emplace_back(&VideoPlaybackCamera::consumer_thread_func, this, i);
    }
}

void VideoPlaybackCamera::stop_pipeline() {
    if (!is_running_) return;
    is_running_ = false;
    
    queue_consumer_cv_.notify_all();
    queue_producer_cv_.notify_all();
    jpeg_ready_cv_.notify_all();
    
    if (producer_thread_.joinable()) producer_thread_.join();
    for (auto& t : encoder_threads_) if (t.joinable()) t.join();
    encoder_threads_.clear();
    
    cleanup_encoder_pool();
    if (decoder_ctx_) {
        avcodec_free_context(&decoder_ctx_);
        decoder_ctx_ = nullptr;
    }
    if (format_ctx_) {
        avformat_close_input(&format_ctx_);
        format_ctx_ = nullptr;
    }

    while(!frame_queue_.empty()) {
        av_frame_free(&frame_queue_.front().frame);
        frame_queue_.pop();
    }
}

void VideoPlaybackCamera::producer_thread_func() {
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    AVFrame* sw_frame = av_frame_alloc();
    auto next_frame_time = std::chrono::high_resolution_clock::now();

    while (is_running_) {
        if (av_read_frame(format_ctx_, packet) < 0) {
            if (loop_playback_) {
                av_seek_frame(format_ctx_, video_stream_index_, 0, AVSEEK_FLAG_BACKWARD);
                next_frame_time = std::chrono::high_resolution_clock::now();
                continue;
            } else {
                break;
            }
        }

        if (packet->stream_index == video_stream_index_) {
            int ret = avcodec_send_packet(decoder_ctx_, packet);
            if (ret >= 0) {
                while (ret >= 0) {
                    ret = avcodec_receive_frame(decoder_ctx_, frame);
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                    if (ret < 0) break;
                    if (!is_running_) break;

                    std::this_thread::sleep_until(next_frame_time);
                    next_frame_time += frame_duration_;
                    frames_decoded_++;

                    AVFrame* frame_to_queue = av_frame_alloc();
                    if (frame->format == AV_PIX_FMT_VIDEOTOOLBOX || frame->format == AV_PIX_FMT_CUDA) {
                        if (transfer_hw_frame_to_sw(frame, sw_frame)) {
                            av_frame_move_ref(frame_to_queue, sw_frame);
                        } else {
                            frames_dropped_producer_++;
                            av_frame_free(&frame_to_queue);
                            av_frame_unref(frame);
                            continue;
                        }
                    } else {
                       av_frame_move_ref(frame_to_queue, frame);
                    }

                    {
                        std::unique_lock<std::mutex> lock(queue_mutex_);
                        if (frame_queue_.size() >= max_queue_size_) {
                            frames_dropped_producer_++;
                            av_frame_free(&frame_to_queue);
                        } else {
                            frame_queue_.push({frame_to_queue});
                            lock.unlock();
                            queue_consumer_cv_.notify_one();
                        }
                    }
                    av_frame_unref(frame);
                }
            }
        }
        av_packet_unref(packet);
    }
    
    is_running_ = false;
    queue_consumer_cv_.notify_all();
    av_packet_free(&packet);
    av_frame_free(&frame);
    av_frame_free(&sw_frame);
    std::cout << "Producer thread finished." << std::endl;
}

void VideoPlaybackCamera::consumer_thread_func(int thread_id) {
    std::vector<uint8_t> local_jpeg_buffer;
    if (decoder_ctx_) {
        local_jpeg_buffer.reserve(decoder_ctx_->width * decoder_ctx_->height);
    }

    while (is_running_) {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        queue_consumer_cv_.wait(lock, [this] { return !frame_queue_.empty() || !is_running_; });

        if (!is_running_ && frame_queue_.empty()) break;
        
        EncodingTask task = std::move(frame_queue_.front());
        frame_queue_.pop();
        lock.unlock();

        if (encode_task(thread_id, task, local_jpeg_buffer)) {
            frames_encoded_++;
            {
                std::lock_guard<std::mutex> jpeg_lock(jpeg_mutex_);
                latest_jpeg_buffer_ = local_jpeg_buffer;
                is_jpeg_ready_ = true;
            }
            jpeg_ready_cv_.notify_one();
        } else {
            frames_dropped_consumer_++;
        }
        av_frame_free(&task.frame);
    }
    std::cout << "Consumer thread " << thread_id << " finished." << std::endl;
}

bool VideoPlaybackCamera::encode_task(int thread_id, EncodingTask& task, std::vector<uint8_t>& jpeg_buffer) {
    AVFrame* yuv_frame = yuv_frames_[thread_id];
    
    if (!sws_contexts_[thread_id]) {
        sws_contexts_[thread_id] = sws_getContext(
            task.frame->width, task.frame->height, (AVPixelFormat)task.frame->format,
            yuv_frame->width, yuv_frame->height, AV_PIX_FMT_YUV420P,
            SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
        
        if (!sws_contexts_[thread_id]) return false;

        av_opt_set_int(sws_contexts_[thread_id], "src_range", 1, 0);
        av_opt_set_int(sws_contexts_[thread_id], "dst_range", 1, 0);
    }
    
    sws_scale(sws_contexts_[thread_id], task.frame->data, task.frame->linesize, 0,
              task.frame->height, yuv_frame->data, yuv_frame->linesize);

    AVPacket* pkt = av_packet_alloc();
    int ret = avcodec_send_frame(mjpeg_encoder_ctxs_[thread_id], yuv_frame);
    if (ret >= 0) {
        ret = avcodec_receive_packet(mjpeg_encoder_ctxs_[thread_id], pkt);
        if (ret >= 0) {
            jpeg_buffer.assign(pkt->data, pkt->data + pkt->size);
            av_packet_free(&pkt);
            return true;
        }
    }
    av_packet_free(&pkt);
    return false;
}

#if defined(USE_VIDEOTOOLBOX)
enum AVPixelFormat VideoPlaybackCamera::get_hw_format_videotoolbox(AVCodecContext* ctx, const enum AVPixelFormat* pix_fmts) {
    for (const enum AVPixelFormat* p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == AV_PIX_FMT_VIDEOTOOLBOX) return *p;
    }
    return AV_PIX_FMT_NONE;
}
#elif defined(USE_NVDEC)
enum AVPixelFormat VideoPlaybackCamera::get_hw_format_nvdec(AVCodecContext* ctx, const enum AVPixelFormat* pix_fmts) {
    for (const enum AVPixelFormat* p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == AV_PIX_FMT_CUDA) return *p;
    }
    return AV_PIX_FMT_NONE;
}
#endif

bool VideoPlaybackCamera::initialize_hw_decoder() {
#if defined(USE_VIDEOTOOLBOX)
    decoder_ctx_->get_format = &VideoPlaybackCamera::get_hw_format_videotoolbox;
    if (av_hwdevice_ctx_create(&hw_device_ctx_, AV_HWDEVICE_TYPE_VIDEOTOOLBOX, nullptr, nullptr, 0) < 0) {
        std::cerr << "Warning: Failed to create VideoToolbox hardware device; using software decoding." << std::endl;
        return false;
    }
#elif defined(USE_NVDEC)
    decoder_ctx_->get_format = &VideoPlaybackCamera::get_hw_format_nvdec;
    if (av_hwdevice_ctx_create(&hw_device_ctx_, AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr, 0) < 0) {
        std::cerr << "Warning: Failed to create CUDA hardware device; using software decoding." << std::endl;
        return false;
    }
#endif

#if defined(USE_VIDEOTOOLBOX) || defined(USE_NVDEC)
    decoder_ctx_->hw_device_ctx = av_buffer_ref(hw_device_ctx_);
    return true;
#else
    return false;
#endif
}

bool VideoPlaybackCamera::transfer_hw_frame_to_sw(AVFrame* src, AVFrame* dst) {
    return av_hwframe_transfer_data(dst, src, 0) >= 0;
}

vs::Camera::raw_image VideoPlaybackCamera::get_image(std::string mime_type, const vs::ProtoStruct& extra) {
    std::unique_lock<std::mutex> lock(jpeg_mutex_);
    if (!jpeg_ready_cv_.wait_for(lock, std::chrono::milliseconds(200), [this] { return is_jpeg_ready_; })) {
        throw vs::Exception("Timeout waiting for frame from stream.");
    }
    vs::Camera::raw_image img;
    img.bytes = latest_jpeg_buffer_;
    img.mime_type = "image/jpeg";
    return img;
}

vs::Camera::image_collection VideoPlaybackCamera::get_images() {
    throw vs::Exception("get_images is not implemented for this camera.");
}

vs::Camera::point_cloud VideoPlaybackCamera::get_point_cloud(std::string mime_type, const vs::ProtoStruct& extra) {
    throw vs::Exception("get_point_cloud is not implemented for this camera.");
}

vs::Camera::properties VideoPlaybackCamera::get_properties() {
    vs::Camera::properties props;
    props.supports_pcd = false;
    props.frame_rate = (target_fps_ > 0) ? target_fps_ : std::max(1.0, source_fps_);
    if (decoder_ctx_) {
        props.intrinsic_parameters.width_px = decoder_ctx_->width;
        props.intrinsic_parameters.height_px = decoder_ctx_->height;
    }
    return props;
}

vs::ProtoStruct VideoPlaybackCamera::do_command(const vs::ProtoStruct& command) {
    if (command.find("command") == command.end() || *command.at("command").get<std::string>() != "get_stats") {
        throw vs::Exception("Unknown command. Only 'get_stats' is supported.");
    }
    vs::ProtoStruct results;
    results["frames_decoded"] = vs::ProtoValue(static_cast<int>(frames_decoded_.load()));
    results["frames_encoded"] = vs::ProtoValue(static_cast<int>(frames_encoded_.load()));
    results["frames_dropped_producer"] = vs::ProtoValue(static_cast<int>(frames_dropped_producer_.load()));
    results["frames_dropped_consumer"] = vs::ProtoValue(static_cast<int>(frames_dropped_consumer_.load()));
    results["encoder_queue_size"] = vs::ProtoValue(static_cast<int>(frame_queue_.size()));
    results["encoder_threads"] = vs::ProtoValue(num_encoder_threads_);
    
    auto elapsed_s = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::high_resolution_clock::now() - start_time_).count();
    if (elapsed_s > 0) {
        results["actual_fps"] = vs::ProtoValue(static_cast<double>(frames_encoded_.load()) / elapsed_s);
    } else {
        results["actual_fps"] = vs::ProtoValue(0.0);
    }
    return results;
}

std::vector<vs::GeometryConfig> VideoPlaybackCamera::get_geometries(const vs::ProtoStruct& extra) { return {}; }

} // namespace video_playback
} // namespace hunter
