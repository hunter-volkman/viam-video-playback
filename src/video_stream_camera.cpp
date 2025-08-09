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
    
    if (sws_ctx_) sws_freeContext(sws_ctx_);
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
    
    // Enable multi-threading
    codec_ctx_->thread_count = std::thread::hardware_concurrency();
    codec_ctx_->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    
    if (avcodec_open2(codec_ctx_, codec_, nullptr) < 0) {
        std::cerr << "Failed to open codec" << std::endl;
        return false;
    }
    
    // Calculate frame duration
    double fps = (target_fps_ > 0) ? target_fps_ : source_fps_;
    frame_duration_ = std::chrono::microseconds(static_cast<int64_t>(1000000.0 / fps));
    
    std::cout << "Video opened: " << codec_ctx_->width << "x" << codec_ctx_->height 
              << " @ " << fps << " FPS" << std::endl;
    
    return true;
}

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
    
    while (running_) {
        // Read packet
        if (av_read_frame(format_ctx_, packet) < 0) {
            if (loop_playback_) {
                av_seek_frame(format_ctx_, video_stream_index_, 0, AVSEEK_FLAG_BACKWARD);
                continue;
            } else {
                break;
            }
        }
        
        if (packet->stream_index != video_stream_index_) {
            av_packet_unref(packet);
            continue;
        }
        
        // Decode packet
        int ret = avcodec_send_packet(codec_ctx_, packet);
        if (ret < 0) {
            av_packet_unref(packet);
            continue;
        }
        
        while (ret >= 0) {
            ret = avcodec_receive_frame(codec_ctx_, frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            }
            
            // Frame timing
            auto now = std::chrono::high_resolution_clock::now();
            auto next_frame_time = last_frame_time_ + frame_duration_;
            
            if (now < next_frame_time) {
                std::this_thread::sleep_until(next_frame_time);
            }
            
            // Store frame
            {
                std::lock_guard<std::mutex> lock(frame_mutex_);
                if (current_frame_) {
                    av_frame_unref(current_frame_);
                }
                current_frame_ = av_frame_clone(frame);
                frames_decoded_++;
                last_frame_time_ = std::chrono::high_resolution_clock::now();
            }
            frame_cv_.notify_one();
        }
        
        av_packet_unref(packet);
    }
    
    av_packet_free(&packet);
    av_frame_free(&frame);
}

sdk::Camera::raw_image VideoStreamCamera::get_image(std::string mime_type, const sdk::ProtoStruct& extra) {
    std::unique_lock<std::mutex> lock(frame_mutex_);
    
    // Wait for frame with timeout
    if (!frame_cv_.wait_for(lock, std::chrono::milliseconds(100),
                            [this] { return current_frame_ != nullptr; })) {
        throw sdk::Exception("No frame available");
    }
    
    if (!current_frame_) {
        throw sdk::Exception("No frame available");
    }
    
    // Convert frame to RGB
    int width = current_frame_->width;
    int height = current_frame_->height;
    
    std::vector<uint8_t> rgb_buffer(width * height * 3);
    
    if (current_frame_->format != AV_PIX_FMT_RGB24) {
        if (!sws_ctx_) {
            sws_ctx_ = sws_getContext(width, height, 
                                      (AVPixelFormat)current_frame_->format,
                                      width, height, AV_PIX_FMT_RGB24,
                                      SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
        }
        
        uint8_t* dst_data[1] = { rgb_buffer.data() };
        int dst_linesize[1] = { width * 3 };
        
        sws_scale(sws_ctx_, current_frame_->data, current_frame_->linesize,
                 0, height, dst_data, dst_linesize);
    } else {
        memcpy(rgb_buffer.data(), current_frame_->data[0], rgb_buffer.size());
    }
    
    sdk::Camera::raw_image img;
    // Copy bytes into the vector
    img.bytes.assign(rgb_buffer.begin(), rgb_buffer.end());
    img.mime_type = "image/jpeg";  // We'll return as JPEG mime type
    
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
    props.frame_rate = (target_fps_ > 0) ? target_fps_ : source_fps_;
    
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
            // Use int instead of int64_t to avoid ambiguity
            result["frames_decoded"] = sdk::ProtoValue(static_cast<int>(frames_decoded_.load()));
            result["frames_dropped"] = sdk::ProtoValue(static_cast<int>(frames_dropped_.load()));
        }
    }
    
    return result;
}

std::vector<sdk::GeometryConfig> VideoStreamCamera::get_geometries(const sdk::ProtoStruct& extra) {
    return {};
}

// Factory method - returns Resource pointer
std::shared_ptr<sdk::Resource> VideoStreamCamera::create(const sdk::Dependencies& deps,
                                                         const sdk::ResourceConfig& cfg) {
    return std::make_shared<VideoStreamCamera>(deps, cfg);
}

} // namespace video_stream
} // namespace viam
