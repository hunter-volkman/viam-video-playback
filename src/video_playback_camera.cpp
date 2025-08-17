#include "video_playback_camera.hpp"
#include <viam/sdk/common/exception.hpp>
#include <viam/sdk/common/proto_value.hpp>
#include <iostream>
#include <cstring>
#include <algorithm>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
extern "C" {
#include <libavutil/opt.h>
#include <libavutil/hwcontext.h>
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
    
#if defined(__aarch64__) && defined(__linux__)
    // Jetson Orin NX: 6 cores, use 4 for encoding (leave 2 for decode/system)
    num_encoder_threads_ = 4;
    std::cout << "[JETSON] Optimized for Orin NX with " << num_encoder_threads_ << " encoder threads" << std::endl;
#else
    num_encoder_threads_ = std::min(4, static_cast<int>(std::thread::hardware_concurrency() / 2));
#endif
    
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
        throw vs::Exception("`video_path` attribute is required");
    }
    video_path_ = *attrs.at("video_path").get<std::string>();

    // Parse optional attributes with sane defaults
    if (attrs.find("loop") != attrs.end()) {
        loop_playback_ = *attrs.at("loop").get<bool>();
    }
    if (attrs.find("target_fps") != attrs.end()) {
        target_fps_ = static_cast<int>(*attrs.at("target_fps").get<double>());
    }
    if (attrs.find("jpeg_quality_level") != attrs.end()) {
        quality_level_ = std::clamp(static_cast<int>(*attrs.at("jpeg_quality_level").get<double>()), 2, 31);
    }
    if (attrs.find("output_width") != attrs.end()) {
        output_width_ = static_cast<int>(*attrs.at("output_width").get<double>());
    }
    if (attrs.find("output_height") != attrs.end()) {
        output_height_ = static_cast<int>(*attrs.at("output_height").get<double>());
    }

    std::cout << "Configuration:" << std::endl;
    std::cout << "  Path: " << video_path_ << std::endl;
    std::cout << "  Output: " << output_width_ << "x" << output_height_ << std::endl;
    std::cout << "  Target FPS: " << (target_fps_ > 0 ? std::to_string(target_fps_) : "source") << std::endl;
    std::cout << "  JPEG Quality: " << quality_level_ << std::endl;
    std::cout << "  Loop: " << (loop_playback_ ? "yes" : "no") << std::endl;

#if defined(__aarch64__) && defined(__linux__)
    setup_gstreamer_pipeline();
#else
    if (!initialize_decoder(video_path_)) {
        throw vs::Exception("Failed to initialize decoder for: " + video_path_);
    }
    if (!initialize_encoder_pool(decoder_ctx_->width, decoder_ctx_->height)) {
        throw vs::Exception("Failed to initialize encoder pool");
    }
#endif
    
    start_pipeline();
}

#if defined(__aarch64__) && defined(__linux__)
void VideoPlaybackCamera::setup_gstreamer_pipeline() {
    // Initialize GStreamer
    if (!gst_is_initialized()) {
        gst_init(nullptr, nullptr);
    }
    
    // Build optimized pipeline string for Jetson
    std::stringstream pipeline_str;
    pipeline_str << "filesrc location=\"" << video_path_ << "\" ! ";
    pipeline_str << "qtdemux ! h264parse ! ";
    pipeline_str << "nvv4l2decoder enable-max-performance=1 ! ";
    pipeline_str << "nvvidconv ! ";
    pipeline_str << "video/x-raw,format=I420";
    
    if (output_width_ > 0 && output_height_ > 0) {
        pipeline_str << ",width=" << output_width_ << ",height=" << output_height_;
    }
    
    if (target_fps_ > 0) {
        pipeline_str << " ! videorate drop-only=true ! ";
        pipeline_str << "video/x-raw,framerate=" << target_fps_ << "/1";
    }
    
    pipeline_str << " ! appsink name=sink emit-signals=true ";
    pipeline_str << "max-buffers=4 drop=true sync=false";
    
    std::cout << "[GST] Pipeline: " << pipeline_str.str() << std::endl;
    
    // Create pipeline
    GError* error = nullptr;
    pipeline_ = gst_parse_launch(pipeline_str.str().c_str(), &error);
    if (!pipeline_) {
        std::string err_msg = error ? error->message : "unknown error";
        if (error) g_error_free(error);
        throw vs::Exception("Failed to create GStreamer pipeline: " + err_msg);
    }
    
    // Get appsink
    appsink_ = gst_bin_get_by_name(GST_BIN(pipeline_), "sink");
    if (!appsink_) {
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
        throw vs::Exception("Failed to get appsink from pipeline");
    }
    
    // Configure appsink
    g_signal_connect(appsink_, "new-sample", G_CALLBACK(on_new_sample), this);
    
    // Set up bus watch
    GstBus* bus = gst_element_get_bus(pipeline_);
    bus_watch_id_ = gst_bus_add_watch(bus, bus_callback, this);
    gst_object_unref(bus);
    
    // Start pipeline
    if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        throw vs::Exception("Failed to start GStreamer pipeline");
    }
    
    std::cout << "[GST] Pipeline started successfully" << std::endl;
}

GstFlowReturn VideoPlaybackCamera::on_new_sample(GstAppSink* sink, gpointer user_data) {
    auto* self = static_cast<VideoPlaybackCamera*>(user_data);
    if (!self->running_) return GST_FLOW_OK;
    
    GstSample* sample = gst_app_sink_pull_sample(sink);
    if (!sample) return GST_FLOW_OK;
    
    GstBuffer* buffer = gst_sample_get_buffer(sample);
    GstCaps* caps = gst_sample_get_caps(sample);
    
    if (buffer && caps) {
        GstVideoInfo info;
        if (gst_video_info_from_caps(&info, caps)) {
            GstVideoFrame frame;
            if (gst_video_frame_map(&frame, &info, buffer, GST_MAP_READ)) {
                int width = GST_VIDEO_FRAME_WIDTH(&frame);
                int height = GST_VIDEO_FRAME_HEIGHT(&frame);
                
                // Copy I420 data to contiguous buffer
                size_t y_size = width * height;
                size_t uv_size = (width / 2) * (height / 2);
                size_t total_size = y_size + 2 * uv_size;
                
                auto frame_data = std::make_shared<std::vector<uint8_t>>(total_size);
                uint8_t* dst = frame_data->data();
                
                // Copy Y plane
                const uint8_t* y_src = (uint8_t*)GST_VIDEO_FRAME_COMP_DATA(&frame, 0);
                int y_stride = GST_VIDEO_FRAME_COMP_STRIDE(&frame, 0);
                for (int row = 0; row < height; ++row) {
                    std::memcpy(dst + row * width, y_src + row * y_stride, width);
                }
                
                // Copy U plane
                dst += y_size;
                const uint8_t* u_src = (uint8_t*)GST_VIDEO_FRAME_COMP_DATA(&frame, 1);
                int u_stride = GST_VIDEO_FRAME_COMP_STRIDE(&frame, 1);
                for (int row = 0; row < height / 2; ++row) {
                    std::memcpy(dst + row * (width / 2), u_src + row * u_stride, width / 2);
                }
                
                // Copy V plane
                dst += uv_size;
                const uint8_t* v_src = (uint8_t*)GST_VIDEO_FRAME_COMP_DATA(&frame, 2);
                int v_stride = GST_VIDEO_FRAME_COMP_STRIDE(&frame, 2);
                for (int row = 0; row < height / 2; ++row) {
                    std::memcpy(dst + row * (width / 2), v_src + row * v_stride, width / 2);
                }
                
                gst_video_frame_unmap(&frame);
                
                // Initialize encoder pool lazily
                if (self->mjpeg_contexts_.empty()) {
                    self->initialize_encoder_pool(width, height);
                }
                
                // Push to lock-free queue
                FrameData fd{frame_data, width, height, 0};
                if (!self->frame_queue_.try_push(std::move(fd))) {
                    self->frames_dropped_.fetch_add(1);
                } else {
                    self->frames_decoded_.fetch_add(1);
                }
            }
        }
    }
    
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

gboolean VideoPlaybackCamera::bus_callback(GstBus* bus, GstMessage* msg, gpointer user_data) {
    auto* self = static_cast<VideoPlaybackCamera*>(user_data);
    
    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_EOS:
            if (self->loop_playback_) {
                gst_element_seek_simple(self->pipeline_, GST_FORMAT_TIME,
                                       GstSeekFlags(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT), 0);
            } else {
                self->running_ = false;
            }
            break;
            
        case GST_MESSAGE_ERROR: {
            GError* err;
            gchar* debug;
            gst_message_parse_error(msg, &err, &debug);
            std::cerr << "[GST] Error: " << err->message << std::endl;
            g_error_free(err);
            g_free(debug);
            self->running_ = false;
            break;
        }
        
        default:
            break;
    }
    
    return TRUE;
}
#else
bool VideoPlaybackCamera::initialize_decoder(const std::string& path) {
    // Open input file
    if (avformat_open_input(&format_ctx_, path.c_str(), nullptr, nullptr) != 0) {
        std::cerr << "Cannot open video file: " << path << std::endl;
        return false;
    }
    
    if (avformat_find_stream_info(format_ctx_, nullptr) < 0) {
        std::cerr << "Cannot find stream information" << std::endl;
        return false;
    }
    
    // Find video stream
    video_stream_index_ = av_find_best_stream(format_ctx_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video_stream_index_ < 0) {
        std::cerr << "Cannot find video stream" << std::endl;
        return false;
    }
    
    AVStream* video_stream = format_ctx_->streams[video_stream_index_];
    
    // Try hardware decoder first on macOS
#if defined(__APPLE__)
    if (video_stream->codecpar->codec_id == AV_CODEC_ID_H264) {
        decoder_ = avcodec_find_decoder_by_name("h264_videotoolbox");
        if (decoder_) {
            std::cout << "Using VideoToolbox hardware decoder" << std::endl;
        }
    }
#endif
    
    if (!decoder_) {
        decoder_ = avcodec_find_decoder(video_stream->codecpar->codec_id);
        if (!decoder_) {
            std::cerr << "Failed to find decoder" << std::endl;
            return false;
        }
    }
    
    decoder_ctx_ = avcodec_alloc_context3(decoder_);
    if (!decoder_ctx_) {
        std::cerr << "Failed to allocate decoder context" << std::endl;
        return false;
    }
    
    avcodec_parameters_to_context(decoder_ctx_, video_stream->codecpar);
    
    // Multi-threaded decoding for software decoders
    if (std::string(decoder_->name).find("videotoolbox") == std::string::npos) {
        decoder_ctx_->thread_count = std::thread::hardware_concurrency();
        decoder_ctx_->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    }
    
    if (avcodec_open2(decoder_ctx_, decoder_, nullptr) < 0) {
        std::cerr << "Failed to open decoder" << std::endl;
        return false;
    }
    
    std::cout << "Decoder initialized: " << decoder_->name 
              << " (" << decoder_ctx_->width << "x" << decoder_ctx_->height << ")" << std::endl;
    
    return true;
}

void VideoPlaybackCamera::decode_thread_func() {
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    AVFrame* sw_frame = av_frame_alloc();
    
    auto next_frame_time = std::chrono::high_resolution_clock::now();
    const auto frame_duration = target_fps_ > 0 ? 
        std::chrono::microseconds(1000000 / target_fps_) : 
        std::chrono::microseconds(33333); // Default 30fps
    
    while (running_) {
        if (av_read_frame(format_ctx_, packet) < 0) {
            if (loop_playback_) {
                av_seek_frame(format_ctx_, video_stream_index_, 0, AVSEEK_FLAG_BACKWARD);
                avcodec_flush_buffers(decoder_ctx_);
                continue;
            } else {
                break;
            }
        }
        
        if (packet->stream_index == video_stream_index_) {
            if (avcodec_send_packet(decoder_ctx_, packet) >= 0) {
                while (avcodec_receive_frame(decoder_ctx_, frame) >= 0) {
                    // Frame rate limiting
                    if (target_fps_ > 0) {
                        std::this_thread::sleep_until(next_frame_time);
                        next_frame_time += frame_duration;
                    }
                    
                    // Handle hardware frames
                    AVFrame* output_frame = frame;
                    if (frame->format == AV_PIX_FMT_VIDEOTOOLBOX) {
                        if (av_hwframe_transfer_data(sw_frame, frame, 0) < 0) {
                            continue;
                        }
                        output_frame = sw_frame;
                    }
                    
                    // Copy frame data
                    size_t frame_size = av_image_get_buffer_size(
                        (AVPixelFormat)output_frame->format,
                        output_frame->width,
                        output_frame->height,
                        32
                    );
                    
                    auto frame_data = std::make_shared<std::vector<uint8_t>>(frame_size);
                    av_image_copy_to_buffer(
                        frame_data->data(),
                        frame_size,
                        output_frame->data,
                        output_frame->linesize,
                        (AVPixelFormat)output_frame->format,
                        output_frame->width,
                        output_frame->height,
                        32
                    );
                    
                    FrameData fd{frame_data, output_frame->width, output_frame->height, output_frame->pts};
                    if (!frame_queue_.try_push(std::move(fd))) {
                        frames_dropped_.fetch_add(1);
                    } else {
                        frames_decoded_.fetch_add(1);
                    }
                }
            }
        }
        
        av_packet_unref(packet);
    }
    
    av_packet_free(&packet);
    av_frame_free(&frame);
    av_frame_free(&sw_frame);
}
#endif

bool VideoPlaybackCamera::initialize_encoder_pool(int width, int height) {
    const AVCodec* mjpeg_encoder = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if (!mjpeg_encoder) {
        std::cerr << "MJPEG encoder not found" << std::endl;
        return false;
    }
    
    mjpeg_contexts_.resize(num_encoder_threads_);
    conversion_frames_.resize(num_encoder_threads_);
    sws_contexts_.resize(num_encoder_threads_);
    
    for (int i = 0; i < num_encoder_threads_; ++i) {
        // Create encoder context
        mjpeg_contexts_[i] = avcodec_alloc_context3(mjpeg_encoder);
        AVCodecContext* ctx = mjpeg_contexts_[i];
        
        ctx->pix_fmt = AV_PIX_FMT_YUVJ420P;
        ctx->width = width;
        ctx->height = height;
        ctx->time_base = {1, target_fps_ > 0 ? target_fps_ : 30};
        ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
        
        // Open encoder with quality setting
        AVDictionary* opts = nullptr;
        av_dict_set(&opts, "preset", "ultrafast", 0);
        
        if (avcodec_open2(ctx, mjpeg_encoder, &opts) < 0) {
            av_dict_free(&opts);
            std::cerr << "Failed to open MJPEG encoder " << i << std::endl;
            return false;
        }
        av_dict_free(&opts);
        
        // Set quality
        av_opt_set_int(ctx->priv_data, "qscale", quality_level_, 0);
        
        // Allocate conversion frame
        conversion_frames_[i] = av_frame_alloc();
        conversion_frames_[i]->format = AV_PIX_FMT_YUVJ420P;
        conversion_frames_[i]->width = width;
        conversion_frames_[i]->height = height;
        av_frame_get_buffer(conversion_frames_[i], 32);
    }
    
    std::cout << "Initialized " << num_encoder_threads_ << " MJPEG encoders (" 
              << width << "x" << height << ", quality=" << quality_level_ << ")" << std::endl;
    
    return true;
}

void VideoPlaybackCamera::encoder_thread_func(int thread_id) {
    // Set thread priority for real-time performance
#ifdef __linux__
    struct sched_param param;
    param.sched_priority = 10;
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);
#endif
    
    std::vector<uint8_t> jpeg_buffer;
    jpeg_buffer.reserve(1024 * 1024); // 1MB initial reservation
    
    while (running_) {
        FrameData frame;
        
        // Try to get frame from queue
        if (!frame_queue_.try_pop(frame)) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            continue;
        }
        
        // Create AVFrame from raw data
        AVFrame* src_frame = av_frame_alloc();
        src_frame->format = AV_PIX_FMT_YUV420P;
        src_frame->width = frame.width;
        src_frame->height = frame.height;
        
        // Set up pointers to planar data
        size_t y_size = frame.width * frame.height;
        size_t uv_size = (frame.width / 2) * (frame.height / 2);
        
        src_frame->data[0] = frame.data->data();
        src_frame->data[1] = frame.data->data() + y_size;
        src_frame->data[2] = frame.data->data() + y_size + uv_size;
        
        src_frame->linesize[0] = frame.width;
        src_frame->linesize[1] = frame.width / 2;
        src_frame->linesize[2] = frame.width / 2;
        
        // Convert to JPEG colorspace if needed
        if (!sws_contexts_[thread_id]) {
            sws_contexts_[thread_id] = sws_getContext(
                frame.width, frame.height, AV_PIX_FMT_YUV420P,
                frame.width, frame.height, AV_PIX_FMT_YUVJ420P,
                SWS_FAST_BILINEAR, nullptr, nullptr, nullptr
            );
        }
        
        sws_scale(sws_contexts_[thread_id],
                  src_frame->data, src_frame->linesize, 0, frame.height,
                  conversion_frames_[thread_id]->data, conversion_frames_[thread_id]->linesize);
        
        // Encode to JPEG
        AVPacket* pkt = av_packet_alloc();
        if (avcodec_send_frame(mjpeg_contexts_[thread_id], conversion_frames_[thread_id]) >= 0) {
            if (avcodec_receive_packet(mjpeg_contexts_[thread_id], pkt) >= 0) {
                // Update output buffer
                jpeg_buffer.assign(pkt->data, pkt->data + pkt->size);
                
                // Double-buffer swap
                {
                    std::lock_guard<std::mutex> lock(jpeg_mutex_);
                    back_buffer_ = jpeg_buffer;
                    std::swap(front_buffer_, back_buffer_);
                    new_frame_ready_ = true;
                    last_frame_time_ = std::chrono::steady_clock::now();
                }
                
                frames_encoded_.fetch_add(1);
            }
        }
        
        av_packet_free(&pkt);
        av_frame_free(&src_frame);
    }
    
    // Cleanup
    if (sws_contexts_[thread_id]) {
        sws_freeContext(sws_contexts_[thread_id]);
        sws_contexts_[thread_id] = nullptr;
    }
}

void VideoPlaybackCamera::start_pipeline() {
    if (running_) return;
    
    running_ = true;
    start_time_ = std::chrono::high_resolution_clock::now();
    
    // Start encoder threads
    encoder_threads_.reserve(num_encoder_threads_);
    for (int i = 0; i < num_encoder_threads_; ++i) {
        encoder_threads_.emplace_back(&VideoPlaybackCamera::encoder_thread_func, this, i);
    }
    
#if !defined(__aarch64__) || !defined(__linux__)
    // Start decode thread for non-Jetson platforms
    decode_thread_ = std::thread(&VideoPlaybackCamera::decode_thread_func, this);
#endif
    
    std::cout << "Pipeline started with " << num_encoder_threads_ << " encoder threads" << std::endl;
}

void VideoPlaybackCamera::stop_pipeline() {
    if (!running_) return;
    
    running_ = false;
    
    // Wait for threads
    if (decode_thread_.joinable()) {
        decode_thread_.join();
    }
    
    for (auto& t : encoder_threads_) {
        if (t.joinable()) {
            t.join();
        }
    }
    encoder_threads_.clear();
    
#if defined(__aarch64__) && defined(__linux__)
    // Cleanup GStreamer
    if (pipeline_) {
        gst_element_set_state(pipeline_, GST_STATE_NULL);
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
    }
    if (appsink_) {
        gst_object_unref(appsink_);
        appsink_ = nullptr;
    }
    if (bus_watch_id_) {
        g_source_remove(bus_watch_id_);
        bus_watch_id_ = 0;
    }
#else
    // Cleanup FFmpeg
    for (auto& ctx : mjpeg_contexts_) {
        if (ctx) avcodec_free_context(&ctx);
    }
    for (auto& frame : conversion_frames_) {
        if (frame) av_frame_free(&frame);
    }
    for (auto& sws : sws_contexts_) {
        if (sws) sws_freeContext(sws);
    }
    
    if (decoder_ctx_) avcodec_free_context(&decoder_ctx_);
    if (format_ctx_) avformat_close_input(&format_ctx_);
#endif
    
    mjpeg_contexts_.clear();
    conversion_frames_.clear();
    sws_contexts_.clear();
    
    std::cout << "Pipeline stopped" << std::endl;
}

vs::Camera::raw_image VideoPlaybackCamera::get_image(std::string /*mime_type*/,
                                                     const vs::ProtoStruct& /*extra*/) {
    // Fast path - check if frame is ready
    if (new_frame_ready_.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(jpeg_mutex_);
        
        // Check frame age
        auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - last_frame_time_
        );
        
        if (age < std::chrono::milliseconds(100)) { // Fresh frame
            vs::Camera::raw_image img;
            img.bytes = front_buffer_;
            img.mime_type = "image/jpeg";
            return img;
        }
    }
    
    // Wait for new frame with timeout
    auto timeout_start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - timeout_start < std::chrono::milliseconds(200)) {
        if (new_frame_ready_.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lock(jpeg_mutex_);
            vs::Camera::raw_image img;
            img.bytes = front_buffer_;
            img.mime_type = "image/jpeg";
            return img;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    
    // Return last known frame if available
    {
        std::lock_guard<std::mutex> lock(jpeg_mutex_);
        if (!front_buffer_.empty()) {
            vs::Camera::raw_image img;
            img.bytes = front_buffer_;
            img.mime_type = "image/jpeg";
            return img;
        }
    }
    
    throw vs::Exception("Timeout waiting for frame");
}

vs::Camera::image_collection VideoPlaybackCamera::get_images() {
    throw vs::Exception("get_images not implemented");
}

vs::Camera::point_cloud VideoPlaybackCamera::get_point_cloud(std::string /*mime_type*/,
                                                             const vs::ProtoStruct& /*extra*/) {
    throw vs::Exception("get_point_cloud not implemented");
}

vs::Camera::properties VideoPlaybackCamera::get_properties() {
    vs::Camera::properties props;
    props.supports_pcd = false;
    props.frame_rate = target_fps_ > 0 ? target_fps_ : 30.0;
    props.intrinsic_parameters.width_px = output_width_;
    props.intrinsic_parameters.height_px = output_height_;
    return props;
}

vs::ProtoStruct VideoPlaybackCamera::do_command(const vs::ProtoStruct& command) {
    auto cmd_it = command.find("command");
    if (cmd_it == command.end() || *cmd_it->second.get<std::string>() != "get_stats") {
        throw vs::Exception("Unknown command");
    }
    
    vs::ProtoStruct stats;
    stats["frames_decoded"] = vs::ProtoValue(static_cast<int>(frames_decoded_.load()));
    stats["frames_encoded"] = vs::ProtoValue(static_cast<int>(frames_encoded_.load()));
    stats["frames_dropped"] = vs::ProtoValue(static_cast<int>(frames_dropped_.load()));
    stats["queue_size"] = vs::ProtoValue(static_cast<int>(frame_queue_.size()));
    
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::high_resolution_clock::now() - start_time_
    ).count();
    
    if (elapsed > 0) {
        stats["actual_fps"] = vs::ProtoValue(static_cast<double>(frames_encoded_.load()) / elapsed);
    }
    
#if defined(__aarch64__) && defined(__linux__)
    stats["pipeline"] = vs::ProtoValue(std::string("GStreamer HW (nvv4l2decoder)"));
#else
    stats["pipeline"] = vs::ProtoValue(std::string("FFmpeg SW"));
#endif
    
    return stats;
}

std::vector<vs::GeometryConfig> VideoPlaybackCamera::get_geometries(const vs::ProtoStruct& /*extra*/) {
    return {};
}

} // namespace video_playback
} // namespace hunter