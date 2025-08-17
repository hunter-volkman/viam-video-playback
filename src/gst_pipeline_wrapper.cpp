#include "gst_pipeline_wrapper.hpp"

#include <iostream>
#include <sstream>
#include <vector>
#include <cstring>

#include <gst/video/video.h>

GstPipelineWrapper::GstPipelineWrapper() {
    // Safe if called multiple times; GStreamer ignores redundant init
    gst_init(nullptr, nullptr);
    std::cout << "GStreamer initialized (decode-only pipeline)." << std::endl;
}

GstPipelineWrapper::~GstPipelineWrapper() {
    stop();
}

std::string GstPipelineWrapper::make_pipeline_str(const std::string& file_path,
                                                  int max_buffers,
                                                  int desired_w,
                                                  int desired_h,
                                                  int desired_fps) {
    std::stringstream ss;

    // Demux + HW decode + colorspace/scale to I420
    ss << "filesrc location=\"" << file_path << "\" ! ";
    ss << "qtdemux name=demux ";
    ss << "demux.video_0 ! queue max-size-buffers=0 max-size-bytes=0 max-size-time=0 ! ";
    ss << "h264parse config-interval=-1 ! ";
    ss << "nvv4l2decoder enable-max-performance=1 ! ";
    ss << "nvvidconv ! ";

    // Base caps
    ss << "video/x-raw,format=I420";
    if (desired_w > 0 && desired_h > 0) {
        ss << ",width=" << desired_w << ",height=" << desired_h;
    }

    // If caller wants FPS, drop upstream using videorate
    if (desired_fps > 0) {
        ss << " ! videorate drop-only=true ! video/x-raw,format=I420,framerate=" << desired_fps << "/1";
        if (desired_w > 0 && desired_h > 0) {
            ss << ",width=" << desired_w << ",height=" << desired_h;
        }
    }

    // appsink: emit signals, drop on pressure, bounded queue
    ss << " ! appsink name=mysink emit-signals=true ";
    ss << "max-buffers=" << max_buffers << " drop=true sync=false";
    return ss.str();
}

bool GstPipelineWrapper::build_and_start_pipeline(int max_buffers, std::string* err_out) {
    std::string pipeline_str = make_pipeline_str(file_path_, max_buffers, desired_w_, desired_h_, desired_fps_);
    std::cout << "Creating GStreamer pipeline: " << pipeline_str << std::endl;

    GError* err = nullptr;
    pipeline_ = gst_parse_launch(pipeline_str.c_str(), &err);
    if (!pipeline_) {
        if (err_out) *err_out = err ? std::string(err->message) : "unknown parse error";
        if (err) g_error_free(err);
        return false;
    }

    // Get appsink
    appsink_ = gst_bin_get_by_name(GST_BIN(pipeline_), "mysink");
    if (!appsink_) {
        if (err_out) *err_out = "failed to retrieve appsink from pipeline";
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
        return false;
    }

    // Configure appsink
    gst_app_sink_set_emit_signals(GST_APP_SINK(appsink_), TRUE);
    gst_app_sink_set_drop(GST_APP_SINK(appsink_), TRUE);
    gst_app_sink_set_max_buffers(GST_APP_SINK(appsink_), max_buffers);
    g_signal_connect(appsink_, "new-sample", G_CALLBACK(&GstPipelineWrapper::on_new_sample), this);

    // Add bus watch
    GstBus* bus = gst_element_get_bus(pipeline_);
    bus_watch_id_ = gst_bus_add_watch(bus, &GstPipelineWrapper::bus_callback, this);
    gst_object_unref(bus);

    // Start
    std::cout << "Opening in BLOCKING MODE" << std::endl;
    GstStateChangeReturn sret = gst_element_set_state(pipeline_, GST_STATE_PLAYING);
    if (sret == GST_STATE_CHANGE_FAILURE) {
        // Decode detailed error
        GstBus* e_bus = gst_element_get_bus(pipeline_);
        if (GstMessage* msg = gst_bus_pop_filtered(e_bus, GST_MESSAGE_ERROR)) {
            GError* e = nullptr; gchar* debug = nullptr;
            gst_message_parse_error(msg, &e, &debug);
            std::string emsg = e ? e->message : "unknown error";
            if (err_out) *err_out = "PLAYING state failed: " + emsg;
            if (debug) g_free(debug);
            if (e) g_error_free(e);
            gst_message_unref(msg);
        } else if (err_out) {
            *err_out = "PLAYING state failed (no bus message)";
        }
        gst_object_unref(e_bus);

        cleanup_pipeline();
        return false;
    }

    running_.store(true);
    std::cout << "✓ GStreamer decode-only pipeline started successfully" << std::endl;
    std::cout << "  Using: nvv4l2decoder (hardware) → nvvidconv (I420"
              << (desired_w_ > 0 && desired_h_ > 0 ? (", " + std::to_string(desired_w_) + "x" + std::to_string(desired_h_)) : "")
              << ") → appsink" << std::endl;
    std::cout << "  Loop enabled: " << (loop_enabled_ ? "yes" : "no") << std::endl;
    return true;
}

bool GstPipelineWrapper::start(const std::string& file_path,
                               FrameCallback cb,
                               bool loop,
                               int  max_buffers,
                               int  desired_w,
                               int  desired_h,
                               int  desired_fps) {
    std::lock_guard<std::mutex> lk(lifecycle_mtx_);
    if (running_.load()) {
        std::cerr << "GStreamer pipeline already running" << std::endl;
        return false;
    }
    frame_cb_         = std::move(cb);
    loop_enabled_     = loop;
    file_path_        = file_path;
    frames_processed_ = 0;
    width_  = 0;
    height_ = 0;
    desired_w_   = desired_w;
    desired_h_   = desired_h;
    desired_fps_ = desired_fps;

    std::string err;
    if (!build_and_start_pipeline(max_buffers, &err)) {
        std::cerr << "Pipeline start failed: " << err << std::endl;
        return false;
    }
    return true;
}

void GstPipelineWrapper::stop() {
    std::lock_guard<std::mutex> lk(lifecycle_mtx_);
    if (!running_.load() && !pipeline_) return;
    running_.store(false);
    cleanup_pipeline();
    frame_cb_ = nullptr;
}

void GstPipelineWrapper::cleanup_pipeline() {
    if (bus_watch_id_ > 0) {
        g_source_remove(bus_watch_id_);
        bus_watch_id_ = 0;
    }
    if (pipeline_) {
        gst_element_set_state(pipeline_, GST_STATE_NULL);
        GstState cur = GST_STATE_NULL, pending = GST_STATE_NULL;
        gst_element_get_state(pipeline_, &cur, &pending, 1 * GST_SECOND);
    }
    if (appsink_) {
        gst_object_unref(appsink_);
        appsink_ = nullptr;
    }
    if (pipeline_) {
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
    }
}

bool GstPipelineWrapper::restart_pipeline() {
    std::cout << "Restarting pipeline for loop..." << std::endl;
    cleanup_pipeline();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    std::string err;
    if (!build_and_start_pipeline(/*max_buffers=*/24, &err)) {
        std::cerr << "Pipeline restart failed: " << err << std::endl;
        running_.store(false);
        return false;
    }
    return true;
}

bool GstPipelineWrapper::try_seek_to_start() {
    if (!pipeline_) return false;

    gst_element_set_state(pipeline_, GST_STATE_PAUSED);
    GstState cur = GST_STATE_PAUSED, pending = GST_STATE_VOID_PENDING;
    gst_element_get_state(pipeline_, &cur, &pending, 500 * GST_MSECOND);

    gboolean ok = gst_element_seek(
        pipeline_,
        1.0,  // rate
        GST_FORMAT_TIME,
        GstSeekFlags(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
        GST_SEEK_TYPE_SET, 0,
        GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE
    );
    if (!ok) return false;

    gst_element_set_state(pipeline_, GST_STATE_PLAYING);
    gst_element_get_state(pipeline_, &cur, &pending, 500 * GST_MSECOND);
    return true;
}

void GstPipelineWrapper::handle_eos() {
    std::cout << "End of stream reached (frame " << frames_processed_.load() << ")" << std::endl;

    if (!loop_enabled_ || !running_.load()) {
        std::cout << "Loop disabled; stopping." << std::endl;
        running_.store(false);
        return;
    }
    std::cout << "Attempting to loop..." << std::endl;
    if (try_seek_to_start()) {
        std::cout << "✓ Successfully seeked to beginning" << std::endl;
        return;
    }
    std::thread([this]() {
        std::lock_guard<std::mutex> lk(this->lifecycle_mtx_);
        if (!this->running_.load()) return;
        if (this->restart_pipeline()) {
            std::cout << "✓ Pipeline restarted successfully" << std::endl;
        } else {
            std::cerr << "ERROR: Failed to restart pipeline!" << std::endl;
        }
    }).detach();
}

void GstPipelineWrapper::handle_error(GError* err, gchar* debug) {
    std::cerr << "GStreamer ERROR: " << (err ? err->message : "unknown") << std::endl;
    if (debug) {
        std::cerr << "Debug info: " << debug << std::endl;
        g_free(debug);
    }
    if (err) g_error_free(err);
    running_.store(false);
}

GstFlowReturn GstPipelineWrapper::on_new_sample(GstAppSink* sink, gpointer user_data) {
    auto* self = static_cast<GstPipelineWrapper*>(user_data);
    if (!self || !self->frame_cb_ || !self->running_.load()) return GST_FLOW_OK;

    GstSample* sample = gst_app_sink_pull_sample(sink);
    if (!sample) return GST_FLOW_OK;

    // Capture caps → width/height
    GstCaps* caps = gst_sample_get_caps(sample);
    if ((self->width_ == 0 || self->height_ == 0) && caps) {
        if (GstStructure* st = gst_caps_get_structure(caps, 0)) {
            gst_structure_get_int(st, "width",  &self->width_);
            gst_structure_get_int(st, "height", &self->height_);
        }
    }
    const int w = self->width_;
    const int h = self->height_;

    GstBuffer* buf = gst_sample_get_buffer(sample);
    if (!buf) { gst_sample_unref(sample); return GST_FLOW_OK; }

    // Map with video API to honor strides, repack to tight I420
    GstVideoInfo vinfo;
    bool have_vinfo = caps && gst_video_info_from_caps(&vinfo, caps);
    if (have_vinfo) {
        GstVideoFrame vframe;
        if (gst_video_frame_map(&vframe, &vinfo, buf, GST_MAP_READ)) {
            const size_t y_sz  = static_cast<size_t>(w) * h;
            const int    uv_w  = w / 2;
            const int    uv_h  = h / 2;
            const size_t uv_sz = static_cast<size_t>(uv_w) * uv_h;
            std::vector<uint8_t> tight;
            tight.resize(y_sz + 2 * uv_sz);

            uint8_t* dstY = tight.data();
            uint8_t* dstU = dstY + y_sz;
            uint8_t* dstV = dstU + uv_sz;

            const uint8_t* srcY = GST_VIDEO_FRAME_COMP_DATA(&vframe, 0);
            const uint8_t* srcU = GST_VIDEO_FRAME_COMP_DATA(&vframe, 1);
            const uint8_t* srcV = GST_VIDEO_FRAME_COMP_DATA(&vframe, 2);

            const int srcStrideY = GST_VIDEO_FRAME_COMP_STRIDE(&vframe, 0);
            const int srcStrideU = GST_VIDEO_FRAME_COMP_STRIDE(&vframe, 1);
            const int srcStrideV = GST_VIDEO_FRAME_COMP_STRIDE(&vframe, 2);

            for (int row = 0; row < h; ++row) {
                std::memcpy(dstY + static_cast<size_t>(row) * w,
                            srcY + static_cast<size_t>(row) * srcStrideY,
                            static_cast<size_t>(w));
            }
            for (int row = 0; row < uv_h; ++row) {
                std::memcpy(dstU + static_cast<size_t>(row) * uv_w,
                            srcU + static_cast<size_t>(row) * srcStrideU,
                            static_cast<size_t>(uv_w));
                std::memcpy(dstV + static_cast<size_t>(row) * uv_w,
                            srcV + static_cast<size_t>(row) * srcStrideV,
                            static_cast<size_t>(uv_w));
            }

            try {
                self->frame_cb_(tight.data(), tight.size(), w, h); // consumer copies immediately
                self->frames_processed_.fetch_add(1);
            } catch (...) {}

            gst_video_frame_unmap(&vframe);
            gst_sample_unref(sample);
            return GST_FLOW_OK;
        }
    }

    // Fallback: raw map (rare)
    GstMapInfo info;
    if (gst_buffer_map(buf, &info, GST_MAP_READ)) {
        try {
            self->frame_cb_(static_cast<const uint8_t*>(info.data), info.size, w, h);
            self->frames_processed_.fetch_add(1);
        } catch (...) {}
        gst_buffer_unmap(buf, &info);
    }
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

gboolean GstPipelineWrapper::bus_callback(GstBus* /*bus*/, GstMessage* msg, gpointer user_data) {
    auto* self = static_cast<GstPipelineWrapper*>(user_data);
    if (!self) return TRUE;

    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_EOS:
            self->handle_eos();
            break;

        case GST_MESSAGE_ERROR: {
            GError* err = nullptr; gchar* debug = nullptr;
            gst_message_parse_error(msg, &err, &debug);
            self->handle_error(err, debug);
            break;
        }

        case GST_MESSAGE_WARNING: {
            GError* err = nullptr; gchar* debug = nullptr;
            gst_message_parse_warning(msg, &err, &debug);
            std::cerr << "GStreamer warning: " << (err ? err->message : "unknown") << std::endl;
            if (debug) g_free(debug);
            if (err)   g_error_free(err);
            break;
        }

        case GST_MESSAGE_STATE_CHANGED: {
            if (GST_MESSAGE_SRC(msg) == GST_OBJECT(self->pipeline_)) {
                GstState old_state, new_state, pending_state;
                gst_message_parse_state_changed(msg, &old_state, &new_state, &pending_state);
                if (new_state == GST_STATE_PLAYING || new_state == GST_STATE_PAUSED) {
                    std::cout << "Pipeline state: " << gst_element_state_get_name(new_state) << std::endl;
                }
            }
            break;
        }

        default: break;
    }
    return TRUE; // keep watching
}
