#include "video_playback_camera.hpp"

#include <viam/sdk/common/exception.hpp>
#include <viam/sdk/common/proto_value.hpp>
#include <iostream>
#include <vector>
#include <string>
#include <algorithm>
#include <cstring>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
extern "C" {
#include <libavutil/opt.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
}
#pragma GCC diagnostic pop

namespace hunter {
namespace video_playback {

namespace vs = viam::sdk;

vs::Model VideoPlaybackCamera::model() {
    return vs::Model{"hunter", "video-playback", "camera"};
}

std::shared_ptr<vs::Resource> VideoPlaybackCamera::create(
    const vs::Dependencies& deps, const vs::ResourceConfig& cfg) {
    return std::make_shared<VideoPlaybackCamera>(deps, cfg);
}

VideoPlaybackCamera::VideoPlaybackCamera(
    const vs::Dependencies& deps, const vs::ResourceConfig& cfg)
    : vs::Camera(cfg.name()) {
    
    // Use reasonable thread count
    num_encoder_threads_ = std::min(4, std::max(2, 
        static_cast<int>(std::thread::hardware_concurrency() / 2)));
    
    std::cout << "Initializing VideoPlaybackCamera: " << cfg.name() << std::endl;
    std::cout << "Hardware threads: " << std::thread::hardware_concurrency() 
              << ", using " << num_encoder_threads_ << " encoder threads" << std::endl;
    
#ifdef USE_NVDEC
    std::cout << "NVIDIA hardware acceleration support enabled" << std::endl;
#endif
    
    reconfigure(deps, cfg);
}

VideoPlaybackCamera::~VideoPlaybackCamera() {
    stop_pipeline();
}

void VideoPlaybackCamera::reconfigure(
    const vs::Dependencies& deps, const vs::ResourceConfig& cfg) {
    
    stop_pipeline();
    
    auto attrs = cfg.attributes();
    
    // Required attribute
    if (attrs.find("video_path") == attrs.end()) {
        throw vs::Exception("`video_path` attribute is required.");
    }
    video_path_ = *attrs.at("video_path").get<std::string>();
    
    // Optional attributes
    if (attrs.find("loop") != attrs.end()) {
        loop_playback_ = *attrs.at("loop").get<bool>();
    }
    
    if (attrs.find("target_fps") != attrs.end()) {
        target_fps_ = static_cast<int>(*attrs.at("target_fps").get<double>());
    }
    
    if (attrs.find("jpeg_quality_level") != attrs.end()) {
        quality_level_ = std::clamp(
            static_cast<int>(*attrs.at("jpeg_quality_level").get<double>()), 
            2, 31);
    }
    
    if (attrs.find("max_resolution") != attrs.end()) {
        max_resolution_ = static_cast<int>(*attrs.at("max_resolution").get<double>());
    }
    
    if (attrs.find("use_hardware_acceleration") != attrs.end()) {
        use_hardware_acceleration_ = *attrs.at("use_hardware_acceleration").get<bool>();
    }
    
    std::cout << "Configuration:" << std::endl;
    std::cout << "  Path: " << video_path_ << std::endl;
    std::cout << "  Loop: " << (loop_playback_ ? "yes" : "no") << std::endl;
    std::cout << "  Target FPS: " << (target_fps_ > 0 ? std::to_string(target_fps_) : "source") << std::endl;
    std::cout << "  JPEG Quality: " << quality_level_ << std::endl;
    std::cout << "  Max Resolution: " << (max_resolution_ > 0 ? std::to_string(max_resolution_) : "source") << std::endl;
    std::cout << "  Hardware Acceleration: " << (use_hardware_acceleration_ ? "yes" : "no") << std::endl;
    
    // Initialize decoder
    if (!initialize_decoder(video_path_)) {
        throw vs::Exception("Failed to initialize video decoder for: " + video_path_);
    }
    
    // Calculate output dimensions
    output_width_ = decoder_ctx_->width;
    output_height_ = decoder_ctx_->height;
    
    if (max_resolution_ > 0 && (output_width_ > max_resolution_ || output_height_ > max_resolution_)) {
        // Scale down maintaining aspect ratio
        if (output_width_ > output_height_) {
            output_width_ = max_resolution_;
            output_height_ = (decoder_ctx_->height * max_resolution_) / decoder_ctx_->width;
        } else {
            output_height_ = max_resolution_;
            output_width_ = (decoder_ctx_->width * max_resolution_) / decoder_ctx_->height;
        }
        // Ensure even dimensions for encoding
        output_width_ = (output_width_ / 2) * 2;
        output_height_ = (output_height_ / 2) * 2;
        
        std::cout << "  Output scaled to: " << output_width_ << "x" << output_height_ << std::endl;
    }
    
    // Initialize encoder pool
    if (!initialize_encoder_pool(output_width_, output_height_)) {
        throw vs::Exception("Failed to initialize JPEG encoder pool.");
    }
    
    start_pipeline();
}

bool VideoPlaybackCamera::initialize_decoder(const std::string& path) {
    // Clean up any existing context
    if (format_ctx_) {
        avformat_close_input(&format_ctx_);
        format_ctx_ = nullptr;
    }
    
    // Open video file
    if (avformat_open_input(&format_ctx_, path.c_str(), nullptr, nullptr) != 0) {
        std::cerr << "Error: Cannot open video file: " << path << std::endl;
        return false;
    }
    
    // Get stream information
    if (avformat_find_stream_info(format_ctx_, nullptr) < 0) {
        std::cerr << "Error: Cannot find stream information" << std::endl;
        return false;
    }
    
    // Find video stream
    int stream_idx = av_find_best_stream(
        format_ctx_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    
    if (stream_idx < 0) {
        std::cerr << "Error: Cannot find video stream" << std::endl;
        return false;
    }
    
    video_stream_index_ = stream_idx;
    AVStream* video_stream = format_ctx_->streams[video_stream_index_];
    source_fps_ = av_q2d(video_stream->r_frame_rate);
    
    // Initialize decoder pointer
    decoder_ = nullptr;
    std::vector<std::string> tried_decoders;
    
    // Try to use hardware decoder if requested
    if (use_hardware_acceleration_) {
#ifdef USE_NVDEC
        std::vector<const char*> hw_decoders;
        
        // On Jetson, try NVIDIA hardware decoders
        if (video_stream->codecpar->codec_id == AV_CODEC_ID_H264) {
            hw_decoders = {"h264_nvv4l2dec", "h264_nvdec", "h264_v4l2m2m"};
        } else if (video_stream->codecpar->codec_id == AV_CODEC_ID_HEVC) {
            hw_decoders = {"hevc_nvv4l2dec", "hevc_nvdec", "hevc_v4l2m2m"};
        }
        
        // Try each hardware decoder with validation
        for (const char* dec_name : hw_decoders) {
            tried_decoders.push_back(dec_name);
            const AVCodec* test_decoder = avcodec_find_decoder_by_name(dec_name);
            
            if (!test_decoder) continue;
            
            AVCodecContext* test_ctx = avcodec_alloc_context3(test_decoder);
            if (!test_ctx) continue;
            
            if (avcodec_parameters_to_context(test_ctx, video_stream->codecpar) < 0) {
                avcodec_free_context(&test_ctx);
                continue;
            }
            
            // Actually try to open and test decode
            if (avcodec_open2(test_ctx, test_decoder, nullptr) == 0) {
                // Test decode one frame to verify it works
                AVPacket* test_pkt = av_packet_alloc();
                AVFrame* test_frame = av_frame_alloc();
                bool works = false;
                
                if (av_read_frame(format_ctx_, test_pkt) == 0 &&
                    test_pkt->stream_index == video_stream_index_) {
                    if (avcodec_send_packet(test_ctx, test_pkt) == 0) {
                        if (avcodec_receive_frame(test_ctx, test_frame) == 0) {
                            // Verify pixel format is valid
                            if (test_frame->format != AV_PIX_FMT_NONE &&
                                test_frame->width > 0 && test_frame->height > 0) {
                                works = true;
                                decoder_ = test_decoder;
                                std::cout << "Hardware decoder " << dec_name
                                         << " verified working (format: "
                                         << av_get_pix_fmt_name((AVPixelFormat)test_frame->format)
                                         << ")" << std::endl;
                            }
                        }
                    }
                }
                
                av_packet_free(&test_pkt);
                av_frame_free(&test_frame);
                avcodec_free_context(&test_ctx);
                
                // Seek back to beginning
                av_seek_frame(format_ctx_, video_stream_index_, 0, AVSEEK_FLAG_BACKWARD);
                
                if (works) break;
            } else {
                avcodec_free_context(&test_ctx);
            }
        }
        
        if (!decoder_ && !tried_decoders.empty()) {
            std::cout << "Tried hardware decoders: ";
            for (const auto& name : tried_decoders) std::cout << name << " ";
            std::cout << "- none worked, falling back to software" << std::endl;
        }
#endif
    }
    
    // Fall back to software decoder if hardware decoder not found or not requested
    if (!decoder_) {
        decoder_ = avcodec_find_decoder(video_stream->codecpar->codec_id);
        if (!decoder_) {
            std::cerr << "Error: Codec not found" << std::endl;
            return false;
        }
        std::cout << "Using software decoder: " << decoder_->name << std::endl;
    }
    
    // Allocate decoder context
    decoder_ctx_ = avcodec_alloc_context3(decoder_);
    if (!decoder_ctx_) {
        std::cerr << "Error: Failed to allocate decoder context" << std::endl;
        return false;
    }
    
    // Copy codec parameters
    if (avcodec_parameters_to_context(decoder_ctx_, video_stream->codecpar) < 0) {
        std::cerr << "Error: Failed to copy codec parameters" << std::endl;
        avcodec_free_context(&decoder_ctx_);
        return false;
    }
    
    // Configure threading for software decoders only
    if (strstr(decoder_->name, "nvv4l2") == nullptr &&
        strstr(decoder_->name, "nvdec") == nullptr &&
        strstr(decoder_->name, "v4l2") == nullptr) {
        // This is a software decoder, use multiple threads
        decoder_ctx_->thread_count = num_encoder_threads_;
        decoder_ctx_->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    }
    
    // Open decoder
    int ret = avcodec_open2(decoder_ctx_, decoder_, nullptr);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        std::cerr << "Error: Failed to open decoder: " << errbuf << std::endl;
        
        // If hardware decoder failed, try falling back to software
        if (use_hardware_acceleration_) {
            std::cout << "Hardware decoder failed, falling back to software decoder" << std::endl;
            avcodec_free_context(&decoder_ctx_);
            
            // Try software decoder
            decoder_ = avcodec_find_decoder(video_stream->codecpar->codec_id);
            if (!decoder_) {
                std::cerr << "Error: Software codec not found" << std::endl;
                return false;
            }
            
            decoder_ctx_ = avcodec_alloc_context3(decoder_);
            if (!decoder_ctx_) {
                std::cerr << "Error: Failed to allocate software decoder context" << std::endl;
                return false;
            }
            
            if (avcodec_parameters_to_context(decoder_ctx_, video_stream->codecpar) < 0) {
                std::cerr << "Error: Failed to copy codec parameters for software decoder" << std::endl;
                avcodec_free_context(&decoder_ctx_);
                return false;
            }
            
            // Configure threading for software decoder
            decoder_ctx_->thread_count = num_encoder_threads_;
            decoder_ctx_->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
            
            if (avcodec_open2(decoder_ctx_, decoder_, nullptr) < 0) {
                std::cerr << "Error: Failed to open software decoder" << std::endl;
                avcodec_free_context(&decoder_ctx_);
                return false;
            }
            
            std::cout << "Successfully opened software decoder: " << decoder_->name << std::endl;
        } else {
            avcodec_free_context(&decoder_ctx_);
            return false;
        }
    }
    
    // Calculate frame timing
    double fps = (target_fps_ > 0 && target_fps_ < source_fps_) ? 
                 target_fps_ : source_fps_;
    frame_duration_ = std::chrono::microseconds(
        static_cast<int64_t>(1000000.0 / fps));
    
    std::cout << "Decoder initialized:" << std::endl;
    std::cout << "  Codec: " << decoder_->name << std::endl;
    std::cout << "  Resolution: " << decoder_ctx_->width << "x" 
              << decoder_ctx_->height << std::endl;
    std::cout << "  Source FPS: " << source_fps_ << std::endl;
    std::cout << "  Target FPS: " << fps << std::endl;
    
    return true;
}

bool VideoPlaybackCamera::initialize_encoder_pool(int width, int height) {
    std::cout << "Initializing MJPEG encoder pool (" 
              << width << "x" << height << ")" << std::endl;
    
    const AVCodec* mjpeg_encoder = nullptr;
    
    // Always use software MJPEG encoder
    mjpeg_encoder = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if (!mjpeg_encoder) {
        std::cerr << "Error: MJPEG encoder not found" << std::endl;
        return false;
    }

    // Resize vectors for thread pooling
    mjpeg_encoder_ctxs_.resize(num_encoder_threads_);
    sws_contexts_.resize(num_encoder_threads_);
    yuv_frames_.resize(num_encoder_threads_);
    encoder_packets_.resize(num_encoder_threads_);
    jpeg_buffers_.resize(num_encoder_threads_);
    
    for (int i = 0; i < num_encoder_threads_; ++i) {
        // Create encoder context
        mjpeg_encoder_ctxs_[i] = avcodec_alloc_context3(mjpeg_encoder);
        AVCodecContext* ctx = mjpeg_encoder_ctxs_[i];
        
        ctx->pix_fmt = AV_PIX_FMT_YUVJ420P;
        ctx->width = width;
        ctx->height = height;
        ctx->time_base = AVRational{1, static_cast<int>(source_fps_)};
        
        // Set quality
        // Use lower quality for higher resolutions
        int adjusted_quality = quality_level_;
        if (width > 2560 || height > 1440) {
            // 4K: increase quality value (to lower quality)
            // Improve performance at cost of image quality
            adjusted_quality = std::min(31, quality_level_ + 5);
        }
        
        AVDictionary* opts = nullptr;
        av_dict_set(&opts, "qscale", std::to_string(adjusted_quality).c_str(), 0);
        
        if (avcodec_open2(ctx, mjpeg_encoder, &opts) < 0) {
            av_dict_free(&opts);
            std::cerr << "Error: Failed to open MJPEG encoder " << i << std::endl;
            return false;
        }
        av_dict_free(&opts);
        
        // Allocate YUV frame for encoder
        yuv_frames_[i] = av_frame_alloc();
        yuv_frames_[i]->format = AV_PIX_FMT_YUVJ420P;
        yuv_frames_[i]->width = width;
        yuv_frames_[i]->height = height;
        
        if (av_frame_get_buffer(yuv_frames_[i], 32) < 0) {
            std::cerr << "Error: Failed to allocate YUV frame " << i << std::endl;
            return false;
        }
        
        // Create scaler
        // Handles both color conversion and scaling
        bool needs_scaling = (width != decoder_ctx_->width || height != decoder_ctx_->height);
        bool needs_conversion = (decoder_ctx_->pix_fmt != AV_PIX_FMT_YUVJ420P);
        
        if (needs_scaling || needs_conversion) {
            sws_contexts_[i] = sws_getContext(
                decoder_ctx_->width, decoder_ctx_->height, decoder_ctx_->pix_fmt,
                width, height, AV_PIX_FMT_YUVJ420P,
                SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
            
            if (!sws_contexts_[i]) {
                std::cerr << "Error: Failed to create scaler " << i << std::endl;
                return false;
            }
        }
        
        // Pre-allocate packet for reuse
        encoder_packets_[i] = av_packet_alloc();
        if (!encoder_packets_[i]) {
            std::cerr << "Error: Failed to allocate packet " << i << std::endl;
            return false;
        }
        
        // Pre-reserve buffer space (1MB typical for JPEG)
        jpeg_buffers_[i].reserve(1024 * 1024);
    }
    
    return true;
}

void VideoPlaybackCamera::cleanup_encoder_pool() {
    for (auto& ctx : mjpeg_encoder_ctxs_) {
        if (ctx) avcodec_free_context(&ctx);
    }
    mjpeg_encoder_ctxs_.clear();
    
    for (auto& frame : yuv_frames_) {
        if (frame) av_frame_free(&frame);
    }
    yuv_frames_.clear();
    
    for (auto& sws : sws_contexts_) {
        if (sws) sws_freeContext(sws);
    }
    sws_contexts_.clear();
    
    // Clean up pre-allocated resources
    for (auto& pkt : encoder_packets_) {
        if (pkt) av_packet_free(&pkt);
    }
    encoder_packets_.clear();
    jpeg_buffers_.clear();
}

void VideoPlaybackCamera::start_pipeline() {
    if (is_running_) return;
    
    is_running_ = true;
    start_time_ = std::chrono::high_resolution_clock::now();
    
    // Start producer thread
    producer_thread_ = std::thread(&VideoPlaybackCamera::producer_thread_func, this);
    
    // Start consumer threads
    encoder_threads_.clear();
    for (int i = 0; i < num_encoder_threads_; ++i) {
        encoder_threads_.emplace_back(&VideoPlaybackCamera::consumer_thread_func, this, i);
    }
    
    std::cout << "Pipeline started" << std::endl;
}

void VideoPlaybackCamera::stop_pipeline() {
    if (!is_running_) return;
    
    is_running_ = false;
    
    // Wake up all threads
    queue_consumer_cv_.notify_all();
    queue_producer_cv_.notify_all();
    jpeg_ready_cv_.notify_all();
    
    // Wait for threads to finish
    if (producer_thread_.joinable()) {
        producer_thread_.join();
    }
    
    for (auto& t : encoder_threads_) {
        if (t.joinable()) {
            t.join();
        }
    }
    encoder_threads_.clear();
    
    // Clean up encoder pool
    cleanup_encoder_pool();
    
    // Clean up decoder
    if (decoder_ctx_) {
        avcodec_free_context(&decoder_ctx_);
        decoder_ctx_ = nullptr;
    }
    
    if (format_ctx_) {
        avformat_close_input(&format_ctx_);
        format_ctx_ = nullptr;
    }
    
    // Clear frame queue
    while (!frame_queue_.empty()) {
        if (frame_queue_.front().frame) {
            av_frame_free(&frame_queue_.front().frame);
        }
        frame_queue_.pop();
    }
    
    std::cout << "Pipeline stopped" << std::endl;
}

void VideoPlaybackCamera::producer_thread_func() {
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    
    auto next_frame_time = std::chrono::high_resolution_clock::now();
    
    while (is_running_) {
        // Read packet
        int ret = av_read_frame(format_ctx_, packet);
        
        if (ret < 0) {
            if (loop_playback_) {
                // Seek to beginning
                av_seek_frame(format_ctx_, video_stream_index_, 0, AVSEEK_FLAG_BACKWARD);
                avcodec_flush_buffers(decoder_ctx_);
                next_frame_time = std::chrono::high_resolution_clock::now();
                continue;
            } else {
                break;
            }
        }
        
        if (packet->stream_index != video_stream_index_) {
            av_packet_unref(packet);
            continue;
        }
        
        // Send packet to decoder
        ret = avcodec_send_packet(decoder_ctx_, packet);
        av_packet_unref(packet);
        
        if (ret < 0) {
            continue;
        }
        
        // Receive frames from decoder
        while (ret >= 0) {
            ret = avcodec_receive_frame(decoder_ctx_, frame);
            
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            }
            
            if (ret < 0) {
                std::cerr << "Error decoding frame" << std::endl;
                break;
            }
            
            // Frame timing
            auto now = std::chrono::high_resolution_clock::now();
            if (now < next_frame_time) {
                std::this_thread::sleep_until(next_frame_time);
            }
            next_frame_time += frame_duration_;
            
            frames_decoded_++;
            
            // Allocate frame structure only
            // Not pixel data (no deep copy)
            // Zero-copy queueing
            AVFrame* frame_to_queue = av_frame_alloc();
            if (!frame_to_queue) {
                frames_dropped_producer_++;
                av_frame_unref(frame);
                continue;
            }

            // Use reference counting (instead of cloning)
            if (av_frame_ref(frame_to_queue, frame) < 0) {
                av_frame_free(&frame_to_queue);
                frames_dropped_producer_++;
                av_frame_unref(frame);
                continue;
            }

            
            // Add to queue
            {
                std::unique_lock<std::mutex> lock(queue_mutex_);
                
                // Drop frame if queue is full
                if (frame_queue_.size() >= max_queue_size_) {
                    frames_dropped_producer_++;
                    av_frame_free(&frame_to_queue);
                } else {
                    EncodingTask task;
                    task.frame = frame_to_queue;
                    frame_queue_.push(std::move(task));
                    lock.unlock();
                    queue_consumer_cv_.notify_one();
                }
            }
            
            av_frame_unref(frame);
        }
    }
    
    av_packet_free(&packet);
    av_frame_free(&frame);
    
    // Signal consumers to stop
    is_running_ = false;
    queue_consumer_cv_.notify_all();
    
    std::cout << "Producer thread finished" << std::endl;
}

void VideoPlaybackCamera::consumer_thread_func(int thread_id) {
    std::vector<uint8_t> local_jpeg_buffer;
    
    while (is_running_) {
        EncodingTask task;
        
        // Get task from queue
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_consumer_cv_.wait(lock, [this] {
                return !frame_queue_.empty() || !is_running_;
            });
            
            if (!is_running_ && frame_queue_.empty()) {
                break;
            }
            
            task = std::move(frame_queue_.front());
            frame_queue_.pop();
        }
        
        // Encode task
        if (encode_task(thread_id, task, local_jpeg_buffer)) {
            frames_encoded_++;
            
            // Update latest JPEG
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
        
        // Clean up frame
        if (task.frame) {
            av_frame_free(&task.frame);
        }
    }
    
    std::cout << "Consumer thread " << thread_id << " finished" << std::endl;
}

bool VideoPlaybackCamera::encode_task(
    int thread_id, EncodingTask& task, std::vector<uint8_t>& jpeg_buffer) {
    
    if (!task.frame) {
        return false;
    }
    
    AVFrame* dst_yuv = yuv_frames_[thread_id];
    
    // Convert color space and/or scale (only if needed)
    if (sws_contexts_[thread_id]) {
        sws_scale(sws_contexts_[thread_id],
                 task.frame->data, task.frame->linesize,
                 0, task.frame->height,
                 dst_yuv->data, dst_yuv->linesize);
    } else {
        // Direct copy (if no conversion/scaling needed)
        av_frame_copy(dst_yuv, task.frame);
    }
    
    // Use pre-allocated packet
    // Instead of allocating a new packet
    AVPacket* pkt = encoder_packets_[thread_id];
    // Clear previous data without freeing
    av_packet_unref(pkt);
    
    int ret = avcodec_send_frame(mjpeg_encoder_ctxs_[thread_id], dst_yuv);
    if (ret >= 0) {
        ret = avcodec_receive_packet(mjpeg_encoder_ctxs_[thread_id], pkt);
        if (ret >= 0) {
            // Reuse pre-allocated buffer
            jpeg_buffer = jpeg_buffers_[thread_id];
            jpeg_buffer.assign(pkt->data, pkt->data + pkt->size);
            // Don't free the packet (it's reused)
            return true;
        }
    }
    
    // Don't free the packet on error (it's reused)
    return false;
}

vs::Camera::raw_image VideoPlaybackCamera::get_image(
    std::string /*mime_type*/, const vs::ProtoStruct& /*extra*/) {
    
    std::unique_lock<std::mutex> lock(jpeg_mutex_);
    
    // Wait for a frame
    if (!jpeg_ready_cv_.wait_for(lock, std::chrono::milliseconds(200),
                                 [this] { return is_jpeg_ready_; })) {
        throw vs::Exception("Timeout waiting for frame");
    }
    
    vs::Camera::raw_image img;
    img.bytes = latest_jpeg_buffer_;
    img.mime_type = "image/jpeg";
    
    return img;
}

vs::Camera::properties VideoPlaybackCamera::get_properties() {
    vs::Camera::properties props;
    props.supports_pcd = false;
    props.frame_rate = (target_fps_ > 0) ? target_fps_ : source_fps_;
    
    // Report output dimensions (not source dimensions)
    props.intrinsic_parameters.width_px = output_width_;
    props.intrinsic_parameters.height_px = output_height_;
    
    return props;
}

vs::ProtoStruct VideoPlaybackCamera::do_command(const vs::ProtoStruct& command) {
    auto cmd = command.find("command");
    if (cmd == command.end()) {
        throw vs::Exception("Missing 'command' field");
    }
    
    auto cmd_str = *cmd->second.get<std::string>();
    
    if (cmd_str == "get_stats") {
        vs::ProtoStruct stats;
        
        auto now = std::chrono::high_resolution_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - start_time_).count();
        
        stats["frames_decoded"] = vs::ProtoValue(static_cast<int>(frames_decoded_.load()));
        stats["frames_encoded"] = vs::ProtoValue(static_cast<int>(frames_encoded_.load()));
        stats["frames_dropped_producer"] = vs::ProtoValue(static_cast<int>(frames_dropped_producer_.load()));
        stats["frames_dropped_consumer"] = vs::ProtoValue(static_cast<int>(frames_dropped_consumer_.load()));
        stats["actual_fps"] = vs::ProtoValue(
            elapsed > 0 ? static_cast<double>(frames_encoded_.load()) / elapsed : 0.0);
        stats["encoder_queue_size"] = vs::ProtoValue(static_cast<int>(frame_queue_.size()));
        stats["encoder_threads"] = vs::ProtoValue(num_encoder_threads_);
        stats["output_width"] = vs::ProtoValue(output_width_);
        stats["output_height"] = vs::ProtoValue(output_height_);
        stats["hardware_acceleration"] = vs::ProtoValue(use_hardware_acceleration_);
        stats["decoder_name"] = vs::ProtoValue(std::string(decoder_ ? decoder_->name : "unknown"));
        
        return stats;
    }
    
    throw vs::Exception("Unknown command: " + cmd_str);
}

vs::Camera::image_collection VideoPlaybackCamera::get_images(
    std::vector<std::string> filter_source_names,
    const vs::ProtoStruct& extra) {
    throw vs::Exception("get_images not implemented");
}

vs::Camera::point_cloud VideoPlaybackCamera::get_point_cloud(
    std::string /*mime_type*/, const vs::ProtoStruct& /*extra*/) {
    throw vs::Exception("get_point_cloud not implemented");
}

std::vector<vs::GeometryConfig> VideoPlaybackCamera::get_geometries(
    const vs::ProtoStruct& /*extra*/) {
    return {};
}

} // namespace video_playback
} // namespace hunter