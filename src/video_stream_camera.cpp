#include "video_stream_camera.hpp"
#include "buffer/frame_pool.hpp"
#include <viam/sdk/common/exception.hpp>
#include <viam/sdk/common/proto_value.hpp>
#include <iostream>
#include <vector>

namespace viam {
namespace video_stream {

sdk::Model VideoStreamCamera::model() {
    return sdk::Model{"viam", "video-stream", "replay"};
}

VideoStreamCamera::VideoStreamCamera(const sdk::Dependencies& deps, const sdk::ResourceConfig& cfg)
    : Camera(cfg.name()), frame_pool_(std::make_unique<FramePool>(10)) {
    std::cout << "Initializing VideoStreamCamera: " << cfg.name() << std::endl;
    reconfigure(deps, cfg);
}

VideoStreamCamera::~VideoStreamCamera() {
    stop_streaming();
    
    // Clean up MJPEG encoder
    if (yuvj_frame_) av_frame_free(&yuvj_frame_);
    if (sws_to_yuvj_ctx_) sws_freeContext(sws_to_yuvj_ctx_);
    if (mjpeg_enc_ctx_) avcodec_free_context(&mjpeg_enc_ctx_);
    
#ifdef USE_VIDEOTOOLBOX
    if (hw_device_ctx_) av_buffer_unref(&hw_device_ctx_);
#endif
    
    // Clean up decoder
    if (codec_ctx_) avcodec_free_context(&codec_ctx_);
    if (format_ctx_) avformat_close_input(&format_ctx_);
}

void VideoStreamCamera::reconfigure(const sdk::Dependencies& deps, const sdk::ResourceConfig& cfg) {
    stop_streaming();
    
    auto attrs = cfg.attributes();
    
    // Get video path from config
    auto video_path_attr = attrs.find("video_path");
    if (video_path_attr != attrs.end()) {
        auto* str_val = video_path_attr->second.get<std::string>();
        if (str_val) {
            video_path_ = *str_val;
        }
    } else {
        throw sdk::Exception("video_path attribute is required");
    }
    
    // Optional attributes
    auto loop_attr = attrs.find("loop");
    if (loop_attr != attrs.end()) {
        auto* bool_val = loop_attr->second.get<bool>();
        if (bool_val) {
            loop_playback_ = *bool_val;
        }
    }
    
    auto fps_attr = attrs.find("target_fps");
    if (fps_attr != attrs.end()) {
        auto* double_val = fps_attr->second.get<double>();
        if (double_val) {
            target_fps_ = static_cast<int>(*double_val);
        }
    }
    
    std::cout << "Opening video file: " << video_path_ << std::endl;
    std::cout << "Loop playback: " << loop_playback_ << std::endl;
    std::cout << "Target FPS: " << target_fps_ << std::endl;
    
    if (!open_video_file(video_path_)) {
        throw sdk::Exception("Failed to open video file: " + video_path_);
    }
    
    start_streaming();
}

bool VideoStreamCamera::open_video_file(const std::string& path) {
    // Open video file
    if (avformat_open_input(&format_ctx_, path.c_str(), nullptr, nullptr) < 0) {
        std::cerr << "Failed to open video file: " << path << std::endl;
        return false;
    }
    
    if (avformat_find_stream_info(format_ctx_, nullptr) < 0) {
        std::cerr << "Failed to find stream info" << std::endl;
        return false;
    }
    
    // Find video stream
    for (unsigned int i = 0; i < format_ctx_->nb_streams; i++) {
        if (format_ctx_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_index_ = i;
            break;
        }
    }
    
    if (video_stream_index_ == -1) {
        std::cerr << "No video stream found" << std::endl;
        return false;
    }
    
    AVStream* video_stream = format_ctx_->streams[video_stream_index_];
    source_fps_ = av_q2d(video_stream->r_frame_rate);
    
    // Setup decoder
    codec_ = avcodec_find_decoder(video_stream->codecpar->codec_id);
    if (!codec_) {
        std::cerr << "Codec not found" << std::endl;
        return false;
    }
    
    codec_ctx_ = avcodec_alloc_context3(codec_);
    avcodec_parameters_to_context(codec_ctx_, video_stream->codecpar);
    
#ifdef USE_VIDEOTOOLBOX
    // Enable VideoToolbox hardware acceleration
    codec_ctx_->get_format = &VideoStreamCamera::get_hw_format;
    init_hw_device(); // ignore failure; we can fall back to software
#endif
    
    // Enable multi-threading for blazing performance
    codec_ctx_->thread_count = std::max(1u, std::thread::hardware_concurrency());
    codec_ctx_->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    
    if (avcodec_open2(codec_ctx_, codec_, nullptr) < 0) {
        std::cerr << "Failed to open codec" << std::endl;
        return false;
    }
    
    // Initialize MJPEG encoder for real JPEG output
    if (!init_mjpeg_encoder(codec_ctx_->width, codec_ctx_->height)) {
        std::cerr << "Failed to init MJPEG encoder" << std::endl;
        return false;
    }
    
    // Calculate frame duration
    double fps = (target_fps_ > 0) ? target_fps_ : source_fps_;
    frame_duration_ = std::chrono::microseconds(static_cast<int64_t>(1000000.0 / fps));
    
    std::cout << "Video opened: " << codec_ctx_->width << "x" << codec_ctx_->height 
              << " @ " << fps << " FPS" << std::endl;
    std::cout << "Hardware acceleration: " << (codec_ctx_->hw_device_ctx ? "VideoToolbox" : "Software") << std::endl;
    
    return true;
}

bool VideoStreamCamera::init_mjpeg_encoder(int w, int h) {
    mjpeg_enc_ = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if (!mjpeg_enc_) {
        std::cerr << "MJPEG encoder not found" << std::endl;
        return false;
    }

    mjpeg_enc_ctx_ = avcodec_alloc_context3(mjpeg_enc_);
    mjpeg_enc_ctx_->codec_type = AVMEDIA_TYPE_VIDEO;
    mjpeg_enc_ctx_->codec_id = AV_CODEC_ID_MJPEG;
    mjpeg_enc_ctx_->pix_fmt = AV_PIX_FMT_YUVJ420P; // JPEG standard colorspace
    mjpeg_enc_ctx_->width = w;
    mjpeg_enc_ctx_->height = h;
    mjpeg_enc_ctx_->time_base = AVRational{1, (target_fps_ > 0) ? target_fps_ : static_cast<int>(source_fps_)};
    
    // High quality JPEG (lower qscale = higher quality)
    av_opt_set_int(mjpeg_enc_ctx_, "qscale", 3, 0);

    if (avcodec_open2(mjpeg_enc_ctx_, mjpeg_enc_, nullptr) < 0) {
        std::cerr << "Failed to open MJPEG encoder" << std::endl;
        return false;
    }

    // Allocate reusable YUVJ frame
    yuvj_frame_ = av_frame_alloc();
    yuvj_frame_->format = mjpeg_enc_ctx_->pix_fmt;
    yuvj_frame_->width = w;
    yuvj_frame_->height = h;
    if (av_frame_get_buffer(yuvj_frame_, 32) < 0) {
        std::cerr << "Failed to allocate YUVJ frame buffer" << std::endl;
        return false;
    }

    std::cout << "MJPEG encoder initialized: " << w << "x" << h << std::endl;
    return true;
}

bool VideoStreamCamera::ensure_yuvj_frame_from_current(AVFrame* src, AVFrame** out) {
    if (!yuvj_frame_ || !src) return false;

    if (!sws_to_yuvj_ctx_) {
        sws_to_yuvj_ctx_ = sws_getContext(
            src->width, src->height, static_cast<AVPixelFormat>(src->format),
            yuvj_frame_->width, yuvj_frame_->height, AV_PIX_FMT_YUVJ420P,
            SWS_FAST_BILINEAR, nullptr, nullptr, nullptr
        );
        if (!sws_to_yuvj_ctx_) {
            std::cerr << "Failed to create swscale context for YUVJ conversion" << std::endl;
            return false;
        }
    }

    // Convert to YUVJ420P (JPEG colorspace)
    if (sws_scale(
        sws_to_yuvj_ctx_,
        src->data, src->linesize,
        0, src->height,
        yuvj_frame_->data, yuvj_frame_->linesize) <= 0) {
        return false;
    }
    
    *out = yuvj_frame_;
    return true;
}

bool VideoStreamCamera::encode_frame_to_jpeg(AVFrame* src, std::vector<uint8_t>& out_jpeg) {
    // Convert to YUVJ420P if necessary
    AVFrame* yuvj_out = nullptr;
    if (!ensure_yuvj_frame_from_current(src, &yuvj_out)) {
        return false;
    }

    // Encode to MJPEG
    AVPacket* pkt = av_packet_alloc();
    int ret = avcodec_send_frame(mjpeg_enc_ctx_, yuvj_out);
    if (ret < 0) {
        av_packet_free(&pkt);
        return false;
    }
    
    ret = avcodec_receive_packet(mjpeg_enc_ctx_, pkt);
    if (ret < 0) {
        av_packet_free(&pkt);
        return false;
    }

    // Copy JPEG data
    out_jpeg.resize(pkt->size);
    std::memcpy(out_jpeg.data(), pkt->data, pkt->size);
    
    av_packet_free(&pkt);
    frames_encoded_++;
    return true;
}

#ifdef USE_VIDEOTOOLBOX
enum AVPixelFormat VideoStreamCamera::get_hw_format(AVCodecContext* ctx, const enum AVPixelFormat* pix_fmts) {
    for (const enum AVPixelFormat* p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == AV_PIX_FMT_VIDEOTOOLBOX) {
            std::cout << "Using VideoToolbox hardware acceleration" << std::endl;
            return *p;
        }
    }
    std::cout << "VideoToolbox not available, using software decoding" << std::endl;
    return pix_fmts[0];
}

bool VideoStreamCamera::init_hw_device() {
    if (av_hwdevice_ctx_create(&hw_device_ctx_, AV_HWDEVICE_TYPE_VIDEOTOOLBOX, nullptr, nullptr, 0) < 0) {
        std::cerr << "VideoToolbox hwdevice init failed; falling back to software" << std::endl;
        return false;
    }
    codec_ctx_->hw_device_ctx = av_buffer_ref(hw_device_ctx_);
    std::cout << "VideoToolbox hardware device initialized" << std::endl;
    return true;
}

bool VideoStreamCamera::transfer_hwframe_if_needed(AVFrame* src, AVFrame* dst) {
    if (src->format == AV_PIX_FMT_VIDEOTOOLBOX) {
        if (av_hwframe_transfer_data(dst, src, 0) < 0) {
            return false;
        }
        dst->width = src->width;
        dst->height = src->height;
        return true;
    }
    return false;
}
#endif

void VideoStreamCamera::start_streaming() {
    running_ = true;
    start_time_ = std::chrono::high_resolution_clock::now();
    last_frame_time_ = start_time_;
    decode_thread_ = std::thread(&VideoStreamCamera::decode_thread, this);
}

void VideoStreamCamera::stop_streaming() {
    running_ = false;
    frame_cv_.notify_all();
    if (decode_thread_.joinable()) {
        decode_thread_.join();
    }
}

void VideoStreamCamera::decode_thread() {
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    AVFrame* sw_frame = av_frame_alloc(); // for hw->sw transfer
    
    std::cout << "Decode thread started" << std::endl;
    
    // Reset timing for clean start
    auto thread_start = std::chrono::high_resolution_clock::now();
    last_frame_time_ = thread_start;
    
    while (running_) {
        // Read packet
        if (av_read_frame(format_ctx_, packet) < 0) {
            if (loop_playback_) {
                std::cout << "Looping video..." << std::endl;
                av_seek_frame(format_ctx_, video_stream_index_, 0, AVSEEK_FLAG_BACKWARD);
                continue;
            } else {
                std::cout << "End of video reached" << std::endl;
                break;
            }
        }
        
        if (packet->stream_index != video_stream_index_) {
            av_packet_unref(packet);
            continue;
        }
        
        // Decode packet
        int ret = avcodec_send_packet(codec_ctx_, packet);
        av_packet_unref(packet);
        if (ret < 0) continue;
        
        while (ret >= 0) {
            ret = avcodec_receive_frame(codec_ctx_, frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            }
            if (ret < 0) break;

#ifdef USE_VIDEOTOOLBOX
            // Handle hardware frames
            AVFrame* src_for_encode = frame;
            av_frame_unref(sw_frame);
            if (frame->format == AV_PIX_FMT_VIDEOTOOLBOX) {
                if (!transfer_hwframe_if_needed(frame, sw_frame)) {
                    continue;
                }
                src_for_encode = sw_frame;
            }
#else
            AVFrame* src_for_encode = frame;
#endif

            // SIMPLIFIED FRAME TIMING - No more aggressive dropping!
            auto now = std::chrono::high_resolution_clock::now();
            auto time_since_last = now - last_frame_time_;
            
            // Only sleep if we're ahead of schedule
            if (time_since_last < frame_duration_) {
                auto sleep_time = frame_duration_ - time_since_last;
                std::this_thread::sleep_for(sleep_time);
            }
            
            // Encode frame to JPEG immediately for blazing fast get_image()
            std::vector<uint8_t> jpeg_data;
            if (encode_frame_to_jpeg(src_for_encode, jpeg_data)) {
                {
                    std::lock_guard<std::mutex> jpeg_lock(jpeg_mutex_);
                    current_jpeg_ = std::move(jpeg_data);
                    jpeg_ready_ = true;
                }
                
                // Also store raw frame for potential other uses
                {
                    std::lock_guard<std::mutex> frame_lock(frame_mutex_);
                    if (current_frame_) {
                        av_frame_free(&current_frame_);
                    }
                    current_frame_ = av_frame_clone(src_for_encode);
                    frames_decoded_++;
                    last_frame_time_ = std::chrono::high_resolution_clock::now();
                }
                
                frame_cv_.notify_one();
            } else {
                // Only count as dropped if encoding actually failed
                frames_dropped_++;
                std::cerr << "Failed to encode frame to JPEG" << std::endl;
            }
        }
    }
    
    av_packet_free(&packet);
    av_frame_free(&frame);
    av_frame_free(&sw_frame);
    std::cout << "Decode thread ended" << std::endl;
}

sdk::Camera::raw_image VideoStreamCamera::get_image(std::string mime_type, const sdk::ProtoStruct& extra) {
    // Blazing fast: return pre-encoded JPEG immediately!
    std::unique_lock<std::mutex> lock(jpeg_mutex_);
    if (!jpeg_ready_ || current_jpeg_.empty()) {
        lock.unlock();
        // Wait for first frame if needed
        std::unique_lock<std::mutex> frame_lock(frame_mutex_);
        if (!frame_cv_.wait_for(frame_lock, std::chrono::milliseconds(100), 
                               [this] { return current_frame_ != nullptr; })) {
            throw sdk::Exception("No frame available");
        }
        frame_lock.unlock();
        lock.lock();
    }
    
    if (!jpeg_ready_ || current_jpeg_.empty()) {
        throw sdk::Exception("No JPEG frame available");
    }
    
    sdk::Camera::raw_image img;
    img.bytes = current_jpeg_; // Copy the pre-encoded JPEG
    img.mime_type = "image/jpeg"; // TRUE JPEG format!
    
    return img;
}

sdk::Camera::image_collection VideoStreamCamera::get_images() {
    // Not implemented for single stream
    return {};
}

sdk::Camera::point_cloud VideoStreamCamera::get_point_cloud(std::string mime_type, const sdk::ProtoStruct& extra) {
    throw sdk::Exception("Point cloud not supported for video stream");
}

sdk::Camera::properties VideoStreamCamera::get_properties() {
    sdk::Camera::properties props;
    props.supports_pcd = false;
    props.frame_rate = (target_fps_ > 0) ? target_fps_ : std::max(1.0, source_fps_);
    
    if (codec_ctx_) {
        props.intrinsic_parameters.width_px = codec_ctx_->width;
        props.intrinsic_parameters.height_px = codec_ctx_->height;
    }
    
    return props;
}

sdk::ProtoStruct VideoStreamCamera::do_command(const sdk::ProtoStruct& command) {
    sdk::ProtoStruct result;
    
    auto cmd = command.find("command");
    if (cmd != command.end()) {
        auto* str_val = cmd->second.get<std::string>();
        if (str_val && *str_val == "get_stats") {
            result["frames_decoded"] = sdk::ProtoValue(static_cast<int>(frames_decoded_.load()));
            result["frames_dropped"] = sdk::ProtoValue(static_cast<int>(frames_dropped_.load()));
            result["frames_encoded"] = sdk::ProtoValue(static_cast<int>(frames_encoded_.load()));
            result["hardware_accel"] = sdk::ProtoValue(codec_ctx_ && codec_ctx_->hw_device_ctx ? true : false);
            
            // Calculate actual FPS
            auto now = std::chrono::high_resolution_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start_time_).count();
            if (elapsed > 0) {
                double actual_fps = static_cast<double>(frames_decoded_.load()) / elapsed;
                result["actual_fps"] = sdk::ProtoValue(actual_fps);
            }
        }
    }
    
    return result;
}

std::vector<sdk::GeometryConfig> VideoStreamCamera::get_geometries(const sdk::ProtoStruct& extra) {
    return {};
}

// Factory method
std::shared_ptr<sdk::Resource> VideoStreamCamera::create(const sdk::Dependencies& deps,
                                                         const sdk::ResourceConfig& cfg) {
    return std::make_shared<VideoStreamCamera>(deps, cfg);
}

} // namespace video_stream
} // namespace viam