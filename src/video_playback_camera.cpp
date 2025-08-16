#include "video_playback_camera.hpp"

#include <viam/sdk/common/exception.hpp>
#include <viam/sdk/common/proto_value.hpp>
#include <iostream>
#include <vector>
#include <string>
#include <functional>
#include <memory>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

extern "C" {
#include <libavutil/opt.h>
}

#pragma GCC diagnostic pop

#if defined(USE_NVDEC)
#include "gst_pipeline_wrapper.hpp"
#endif

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

#if defined(USE_NVDEC)
    // JETSON PATH: GStreamer + HW decode; JPEG encoding via nvjpegenc if present, else jpegenc.
    try {
        if (!gst_pipeline_) {
            gst_pipeline_ = std::make_unique<GstPipelineWrapper>();
        }

        // Receive JPEG frames from GStreamer
        auto cb = [this](const uint8_t* data, size_t size) {
            std::vector<uint8_t> jpeg_data(data, data + size);
            {
                std::lock_guard<std::mutex> lock(this->jpeg_mutex_);
                this->latest_jpeg_buffer_ = std::move(jpeg_data);
                this->is_jpeg_ready_ = true;
                this->last_frame_time_ = std::chrono::steady_clock::now();
            }
            this->jpeg_ready_cv_.notify_all();
            this->frames_decoded_.fetch_add(1);
            this->frames_encoded_.fetch_add(1);
        };

        if (gst_pipeline_->start(video_path_, cb, loop_playback_)) {
            std::cout << "✓ GStreamer pipeline started successfully" << std::endl;
            std::cout << "  Using: nvv4l2decoder (hardware) → " << gst_pipeline_->encoder_name() << std::endl;
            std::cout << "  This configuration avoids libjpeg ABI issues when using nvjpegenc." << std::endl;

            // No placeholder JPEG. We wait for first real frame.
            is_running_ = true;
            start_time_ = std::chrono::high_resolution_clock::now();
            return;  // success
        }
        throw vs::Exception("GStreamer pipeline failed to start. "
                            "Verify file path/permissions, codec support, and GStreamer plugins.");
    } catch (const std::exception& ex) {
        throw vs::Exception(std::string("GStreamer initialization failed: ") + ex.what());
    }
#else
    // OTHER PLATFORMS: FFmpeg software pipeline.
    if (!initialize_decoder(video_path_)) {
        throw vs::Exception("Failed to initialize video decoder for: " + video_path_);
    }
    if (!initialize_encoder_pool(decoder_ctx_->width, decoder_ctx_->height)) {
        throw vs::Exception("Failed to initialize JPEG encoder pool.");
    }
    start_pipeline();
#endif
}

bool VideoPlaybackCamera::initialize_decoder(const std::string& path) {
    if (format_ctx_) {
        avformat_close_input(&format_ctx_);
        format_ctx_ = nullptr;
    }
    if (avformat_open_input(&format_ctx_, path.c_str(), nullptr, nullptr) != 0) {
        std::cerr << "Error: Cannot open video file: " << path << std::endl;
        return false;
    }
    if (avformat_find_stream_info(format_ctx_, nullptr) < 0) {
        std::cerr << "Error: Cannot find stream information" << std::endl;
        return false;
    }

    int stream_idx = av_find_best_stream(format_ctx_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (stream_idx < 0) {
        std::cerr << "Error: Cannot find video stream in the input file" << std::endl;
        return false;
    }

    video_stream_index_ = stream_idx;
    AVStream* video_stream = format_ctx_->streams[video_stream_index_];
    source_fps_ = av_q2d(video_stream->r_frame_rate);

#if defined(USE_VIDEOTOOLBOX)
    // macOS: try VideoToolbox
    const AVCodec* hw_decoder = nullptr;
    if (video_stream->codecpar->codec_id == AV_CODEC_ID_H264) {
        hw_decoder = avcodec_find_decoder_by_name("h264_videotoolbox");
        if (hw_decoder) {
            std::cout << "Found VideoToolbox hardware decoder for H.264" << std::endl;
            decoder_ = hw_decoder;
        }
    }
#endif

    if (!decoder_) {
        decoder_ = avcodec_find_decoder(video_stream->codecpar->codec_id);
        if (!decoder_) {
            std::cerr << "Error: Failed to find decoder for codec ID "
                      << video_stream->codecpar->codec_id << std::endl;
            return false;
        }
    }

    decoder_ctx_ = avcodec_alloc_context3(decoder_);
    if (!decoder_ctx_) {
        std::cerr << "Error: Failed to allocate decoder context" << std::endl;
        return false;
    }

    avcodec_parameters_to_context(decoder_ctx_, video_stream->codecpar);

    if (std::string(decoder_->name).find("videotoolbox") == std::string::npos) {
        decoder_ctx_->thread_count = std::max(1u, std::thread::hardware_concurrency());
        decoder_ctx_->thread_type  = FF_THREAD_FRAME | FF_THREAD_SLICE;
    }

    if (avcodec_open2(decoder_ctx_, decoder_, nullptr) < 0) {
        std::cerr << "Error: Failed to open decoder" << std::endl;
        avcodec_free_context(&decoder_ctx_);
        return false;
    }

    if (video_stream->codecpar->codec_id == AV_CODEC_ID_H264 &&
        std::string(format_ctx_->iformat->name).find("mp4") != std::string::npos) {
        const AVBitStreamFilter* bsf = av_bsf_get_by_name("h264_mp4toannexb");
        if (bsf) {
            av_bsf_alloc(bsf, &bsf_ctx_);
            avcodec_parameters_copy(bsf_ctx_->par_in, video_stream->codecpar);
            av_bsf_init(bsf_ctx_);
        }
    }

    const double fps = (target_fps_ > 0) ? target_fps_ : source_fps_;
    frame_duration_ = std::chrono::microseconds(static_cast<int64_t>(1000000.0 / fps));

    std::cout << "Decoder initialized successfully:" << std::endl;
    std::cout << "  - Codec: " << decoder_->name << std::endl;
    std::cout << "  - Resolution: " << decoder_ctx_->width << "x" << decoder_ctx_->height << std::endl;
    std::cout << "  - Source FPS: " << source_fps_ << std::endl;
    std::cout << "  - Target FPS: " << fps << std::endl;

    return true;
}

bool VideoPlaybackCamera::initialize_encoder_pool(int width, int height) {
    std::cout << "Initializing JPEG encoder pool with " << num_encoder_threads_ << " threads" << std::endl;

    const AVCodec* mjpeg_encoder = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if (!mjpeg_encoder) {
        std::cerr << "Error: MJPEG encoder not found" << std::endl;
        return false;
    }

    mjpeg_encoder_ctxs_.resize(num_encoder_threads_);
    sws_contexts_.resize(num_encoder_threads_, nullptr);
    yuv_frames_.resize(num_encoder_threads_);

    for (int i = 0; i < num_encoder_threads_; ++i) {
        mjpeg_encoder_ctxs_[i] = avcodec_alloc_context3(mjpeg_encoder);
        AVCodecContext* ctx = mjpeg_encoder_ctxs_[i];

        ctx->pix_fmt  = AV_PIX_FMT_YUVJ420P;
        ctx->width    = width;
        ctx->height   = height;
        ctx->time_base = AVRational{1, (target_fps_ > 0) ? target_fps_ : static_cast<int>(source_fps_)};

        AVDictionary* opts = nullptr;
        av_dict_set(&opts, "strict", "-2", 0);
        if (avcodec_open2(ctx, mjpeg_encoder, &opts) < 0) {
            av_dict_free(&opts);
            std::cerr << "Error: Failed to open MJPEG encoder for thread " << i << std::endl;
            return false;
        }
        av_dict_free(&opts);

        av_opt_set(ctx->priv_data, "q", std::to_string(quality_level_).c_str(), 0);

        yuv_frames_[i] = av_frame_alloc();
        yuv_frames_[i]->format = AV_PIX_FMT_YUVJ420P;
        yuv_frames_[i]->width  = width;
        yuv_frames_[i]->height = height;

        if (av_frame_get_buffer(yuv_frames_[i], 32) < 0) {
            std::cerr << "Error: Failed to allocate YUV frame buffer for thread " << i << std::endl;
            return false;
        }
    }
    return true;
}

void VideoPlaybackCamera::cleanup_encoder_pool() {
    for (auto& ctx : mjpeg_encoder_ctxs_) {
        if (ctx) avcodec_free_context(&ctx);
    }
    for (auto& frame : yuv_frames_) {
        if (frame) av_frame_free(&frame);
    }
    for (auto& sws_ctx : sws_contexts_) {
        if (sws_ctx) sws_freeContext(sws_ctx);
    }
    mjpeg_encoder_ctxs_.clear();
    yuv_frames_.clear();
    sws_contexts_.clear();
}

void VideoPlaybackCamera::start_pipeline() {
    if (is_running_) return;
    is_running_ = true;
    start_time_ = std::chrono::high_resolution_clock::now();

#if defined(USE_NVDEC)
    // On Jetson with GStreamer, threads are owned by GStreamer.
    if (!gst_pipeline_ || !gst_pipeline_->running()) {
        producer_thread_ = std::thread(&VideoPlaybackCamera::producer_thread_func, this);
        for (int i = 0; i < num_encoder_threads_; ++i) {
            encoder_threads_.emplace_back(&VideoPlaybackCamera::consumer_thread_func, this, i);
        }
    }
#else
    producer_thread_ = std::thread(&VideoPlaybackCamera::producer_thread_func, this);
    for (int i = 0; i < num_encoder_threads_; ++i) {
        encoder_threads_.emplace_back(&VideoPlaybackCamera::consumer_thread_func, this, i);
    }
#endif
}

void VideoPlaybackCamera::stop_pipeline() {
#if defined(USE_NVDEC)
    if (gst_pipeline_) {
        try { gst_pipeline_->stop(); } catch (...) {}
        gst_pipeline_.reset();
    }
#endif
    if (!is_running_) return;
    is_running_ = false;

    queue_consumer_cv_.notify_all();
    queue_producer_cv_.notify_all();
    jpeg_ready_cv_.notify_all();

    if (producer_thread_.joinable()) producer_thread_.join();
    for (auto& t : encoder_threads_) if (t.joinable()) t.join();
    encoder_threads_.clear();

    if (bsf_ctx_) { av_bsf_free(&bsf_ctx_); bsf_ctx_ = nullptr; }
    if (hw_frames_ctx_) { av_buffer_unref(&hw_frames_ctx_); hw_frames_ctx_ = nullptr; }
    if (hw_device_ctx_) { av_buffer_unref(&hw_device_ctx_); hw_device_ctx_ = nullptr; }

    cleanup_encoder_pool();

    if (decoder_ctx_) { avcodec_free_context(&decoder_ctx_); decoder_ctx_ = nullptr; }
    if (format_ctx_)  { avformat_close_input(&format_ctx_);  format_ctx_  = nullptr; }

    while (!frame_queue_.empty()) {
        av_frame_free(&frame_queue_.front().frame);
        frame_queue_.pop();
    }
}

void VideoPlaybackCamera::producer_thread_func() {
    AVPacket* packet = av_packet_alloc();
    AVFrame*  frame  = av_frame_alloc();
    AVFrame*  sw_frame = av_frame_alloc();
    auto next_frame_time = std::chrono::high_resolution_clock::now();

    while (is_running_) {
        if (av_read_frame(format_ctx_, packet) < 0) {
            if (loop_playback_) {
                av_seek_frame(format_ctx_, video_stream_index_, 0, AVSEEK_FLAG_BACKWARD);
                avcodec_flush_buffers(decoder_ctx_);
                next_frame_time = std::chrono::high_resolution_clock::now();
                continue;
            } else {
                break;
            }
        }

        if (packet->stream_index == video_stream_index_) {
            int ret = 0;
            if (bsf_ctx_) {
                ret = av_bsf_send_packet(bsf_ctx_, packet);
                if (ret < 0) { av_packet_unref(packet); continue; }
                while ((ret = av_bsf_receive_packet(bsf_ctx_, packet)) == 0) {
                    if (avcodec_send_packet(decoder_ctx_, packet) < 0) break;
                    receive_and_queue_frames(frame, sw_frame, next_frame_time);
                }
            } else {
                ret = avcodec_send_packet(decoder_ctx_, packet);
                if (ret >= 0) receive_and_queue_frames(frame, sw_frame, next_frame_time);
            }
        }
        av_packet_unref(packet);
    }

    is_running_ = false;
    queue_consumer_cv_.notify_all();

    av_packet_free(&packet);
    av_frame_free(&frame);
    av_frame_free(&sw_frame);
    std::cout << "Producer thread finished" << std::endl;
}

void VideoPlaybackCamera::receive_and_queue_frames(AVFrame* frame, AVFrame* sw_frame,
                                                   std::chrono::high_resolution_clock::time_point& next_frame_time) {
    while (true) {
        int ret = avcodec_receive_frame(decoder_ctx_, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) {
            char err_buf[AV_ERROR_MAX_STRING_SIZE] = {0};
            av_make_error_string(err_buf, AV_ERROR_MAX_STRING_SIZE, ret);
            std::cerr << "Error receiving frame from decoder: " << err_buf << std::endl;
            break;
        }
        if (!is_running_) break;

        std::this_thread::sleep_until(next_frame_time);
        next_frame_time += frame_duration_;
        frames_decoded_++;

        AVFrame* frame_to_queue = av_frame_alloc();
        if (frame->format == AV_PIX_FMT_VIDEOTOOLBOX ||
            frame->format == AV_PIX_FMT_DRM_PRIME ||
            frame->format == AV_PIX_FMT_CUDA) {
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

void VideoPlaybackCamera::consumer_thread_func(int thread_id) {
    std::vector<uint8_t> local_jpeg_buffer;
    if (decoder_ctx_) {
        local_jpeg_buffer.reserve(decoder_ctx_->width * decoder_ctx_->height);
    }

    while (is_running_) {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        queue_consumer_cv_.wait(lock, [this] {
            return !frame_queue_.empty() || !is_running_;
        });
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
                last_frame_time_ = std::chrono::steady_clock::now();
            }
            jpeg_ready_cv_.notify_all();
        } else {
            frames_dropped_consumer_++;
        }
        av_frame_free(&task.frame);
    }
    std::cout << "Consumer thread " << thread_id << " finished" << std::endl;
}

bool VideoPlaybackCamera::encode_task(int thread_id, EncodingTask& task,
                                     std::vector<uint8_t>& jpeg_buffer) {
    AVFrame* yuv_frame = yuv_frames_[thread_id];
    if (!sws_contexts_[thread_id]) {
        sws_contexts_[thread_id] = sws_getContext(
            task.frame->width, task.frame->height, (AVPixelFormat)task.frame->format,
            yuv_frame->width, yuv_frame->height, AV_PIX_FMT_YUV420P,
            SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws_contexts_[thread_id]) {
            std::cerr << "Error: Failed to create scaler context for thread " << thread_id << std::endl;
            return false;
        }
        av_opt_set_int(sws_contexts_[thread_id], "src_range", 1, 0);
        av_opt_set_int(sws_contexts_[thread_id], "dst_range", 1, 0);
    }

    sws_scale(sws_contexts_[thread_id],
              task.frame->data, task.frame->linesize, 0, task.frame->height,
              yuv_frame->data, yuv_frame->linesize);

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

bool VideoPlaybackCamera::transfer_hw_frame_to_sw(AVFrame* src, AVFrame* dst) {
    if (av_hwframe_transfer_data(dst, src, 0) < 0) {
        std::cerr << "Error: Failed to transfer hardware frame to software" << std::endl;
        return false;
    }
    return true;
}

vs::Camera::raw_image VideoPlaybackCamera::get_image(std::string /*mime_type*/,
                                                    const vs::ProtoStruct& /*extra*/) {
    std::unique_lock<std::mutex> lock(jpeg_mutex_);

#if defined(USE_NVDEC)
    // Wait up to 500ms for a fresh frame. If we have a recent one (<1s), return it.
    auto now = std::chrono::steady_clock::now();
    if (is_jpeg_ready_) {
        auto age = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_frame_time_);
        if (age < std::chrono::seconds(1)) {
            vs::Camera::raw_image img;
            img.bytes = latest_jpeg_buffer_;
            img.mime_type = "image/jpeg";
            return img;
        }
    }
    if (!jpeg_ready_cv_.wait_for(lock, std::chrono::milliseconds(500), [this]{ return is_jpeg_ready_; })) {
        if (!latest_jpeg_buffer_.empty()) {
            vs::Camera::raw_image img;
            img.bytes = latest_jpeg_buffer_;
            img.mime_type = "image/jpeg";
            return img;
        }
        throw vs::Exception("Timeout waiting for frame from stream");
    }
#else
    if (!jpeg_ready_cv_.wait_for(lock, std::chrono::milliseconds(200),
                                 [this]{ return is_jpeg_ready_; })) {
        throw vs::Exception("Timeout waiting for frame from stream");
    }
    is_jpeg_ready_ = false; // FFmpeg path consumes frames
#endif

    vs::Camera::raw_image img;
    img.bytes = latest_jpeg_buffer_;
    img.mime_type = "image/jpeg";
    return img;
}

vs::Camera::image_collection VideoPlaybackCamera::get_images() {
    throw vs::Exception("get_images is not implemented for this camera");
}

vs::Camera::point_cloud VideoPlaybackCamera::get_point_cloud(std::string /*mime_type*/,
                                                            const vs::ProtoStruct& /*extra*/) {
    throw vs::Exception("get_point_cloud is not implemented for this camera");
}

vs::Camera::properties VideoPlaybackCamera::get_properties() {
    vs::Camera::properties props;
    props.supports_pcd = false;
    props.frame_rate   = (target_fps_ > 0) ? target_fps_ : 30.0;

#if defined(USE_NVDEC)
    if (gst_pipeline_) {
        int w = gst_pipeline_->width();
        int h = gst_pipeline_->height();
        if (w > 0 && h > 0) {
            props.intrinsic_parameters.width_px  = w;
            props.intrinsic_parameters.height_px = h;
        } else {
            props.intrinsic_parameters.width_px  = 1920;
            props.intrinsic_parameters.height_px = 1080;
        }
    } else {
        props.intrinsic_parameters.width_px  = 1920;
        props.intrinsic_parameters.height_px = 1080;
    }
#else
    if (decoder_ctx_) {
        props.intrinsic_parameters.width_px  = decoder_ctx_->width;
        props.intrinsic_parameters.height_px = decoder_ctx_->height;
    }
#endif
    return props;
}

vs::ProtoStruct VideoPlaybackCamera::do_command(const vs::ProtoStruct& command) {
    if (command.find("command") == command.end() ||
        *command.at("command").get<std::string>() != "get_stats") {
        throw vs::Exception("Unknown command. Only 'get_stats' is supported.");
    }

    vs::ProtoStruct results;
    results["frames_decoded"] = vs::ProtoValue(static_cast<int>(frames_decoded_.load()));
    results["frames_encoded"] = vs::ProtoValue(static_cast<int>(frames_encoded_.load()));
    results["frames_dropped_producer"] = vs::ProtoValue(static_cast<int>(frames_dropped_producer_.load()));
    results["frames_dropped_consumer"] = vs::ProtoValue(static_cast<int>(frames_dropped_consumer_.load()));

#if defined(USE_NVDEC)
    results["pipeline_type"]   = vs::ProtoValue(
        (gst_pipeline_ && gst_pipeline_->running())
        ? std::string("GStreamer/") + gst_pipeline_->encoder_name()
        : "Unknown"
    );
    results["encoder_queue_size"] = vs::ProtoValue(0);
    results["encoder_threads"]    = vs::ProtoValue(0);
#else
    results["pipeline_type"]      = vs::ProtoValue("FFmpeg/Software");
    results["encoder_queue_size"] = vs::ProtoValue(static_cast<int>(frame_queue_.size()));
    results["encoder_threads"]    = vs::ProtoValue(num_encoder_threads_);
#endif

    auto elapsed_s = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::high_resolution_clock::now() - start_time_
    ).count();
    results["actual_fps"] = vs::ProtoValue(
        (elapsed_s > 0) ? static_cast<double>(frames_encoded_.load()) / elapsed_s : 0.0
    );

    return results;
}

std::vector<vs::GeometryConfig> VideoPlaybackCamera::get_geometries(const vs::ProtoStruct& /*extra*/) {
    return {};
}

} // namespace video_playback
} // namespace hunter
