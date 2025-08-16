#include "gst_pipeline_wrapper.hpp"
#include <iostream>
#include <sstream>

GstPipelineWrapper::GstPipelineWrapper() {
    // Safe to call multiple times; GStreamer ignores redundant init
    gst_init(nullptr, nullptr);
    std::cout << "GStreamer initialized." << std::endl;
}

GstPipelineWrapper::~GstPipelineWrapper() {
    stop();
}

std::string GstPipelineWrapper::make_pipeline_str(const std::string& file_path,
                                                  const std::string& encoder,
                                                  int max_buffers) {
    // NOTE: We explicitly use h264parse + nvv4l2decoder for hardware decode on Jetson.
    // If you need H.265, wire an alternative builder or detect codec upstream.
    std::stringstream ss;
    ss << "filesrc location=\"" << file_path << "\" ! ";
    ss << "qtdemux name=demux ";
    ss << "demux.video_0 ! queue max-size-buffers=0 max-size-bytes=0 max-size-time=0 ! ";
    ss << "h264parse config-interval=-1 ! ";
    ss << "nvv4l2decoder enable-max-performance=1 ! ";
    ss << "nvvidconv ! video/x-raw,format=I420 ! ";
    ss << encoder << " quality=85 ! ";
    ss << "appsink name=mysink emit-signals=true ";
    ss << "max-buffers=" << max_buffers << " drop=true sync=false";
    return ss.str();
}

bool GstPipelineWrapper::build_and_start_pipeline(const std::string& encoder, int max_buffers, std::string* err_out) {
    encoder_name_ = encoder;

    // Build pipeline
    std::string pipeline_str = make_pipeline_str(file_path_, encoder, max_buffers);
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
    gst_app_sink_set_max_buffers(GST_APP_SINK(appsink_), max_buffers);
    gst_app_sink_set_drop(GST_APP_SINK(appsink_), TRUE);

    // Connect sample signal
    g_signal_connect(appsink_, "new-sample", G_CALLBACK(&GstPipelineWrapper::on_new_sample), this);

    // Add bus watch
    GstBus* bus = gst_element_get_bus(pipeline_);
    bus_watch_id_ = gst_bus_add_watch(bus, &GstPipelineWrapper::bus_callback, this);
    gst_object_unref(bus);

    // Start
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
    std::cout << "✓ GStreamer pipeline started successfully" << std::endl;
    std::cout << "  Using: nvv4l2decoder (hardware) → " << encoder_name_ << " → appsink" << std::endl;
    std::cout << "  Loop enabled: " << (loop_enabled_ ? "yes" : "no") << std::endl;
    return true;
}

bool GstPipelineWrapper::start(const std::string& file_path, FrameCallback cb, bool loop, int max_buffers) {
    std::lock_guard<std::mutex> lk(lifecycle_mtx_);
    if (running_.load()) {
        std::cerr << "GStreamer pipeline already running" << std::endl;
        return false;
    }
    frame_cb_      = std::move(cb);
    loop_enabled_  = loop;
    file_path_     = file_path;
    frames_processed_ = 0;
    width_ = height_ = 0;

    // Prefer nvjpegenc; if missing, fall back to jpegenc.
    std::string err;
    if (!build_and_start_pipeline("nvjpegenc", max_buffers, &err)) {
        std::cerr << "nvjpegenc path failed (" << err << "), falling back to jpegenc..." << std::endl;
        err.clear();
        if (!build_and_start_pipeline("jpegenc", max_buffers, &err)) {
            std::cerr << "jpegenc path also failed: " << err << std::endl;
            return false;
        }
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
    // Remove bus watch
    if (bus_watch_id_ > 0) {
        g_source_remove(bus_watch_id_);
        bus_watch_id_ = 0;
    }

    // Drain EOS/error and drive to NULL
    if (pipeline_) {
        // Set to NULL and block for completion to ensure NVDEC fully releases
        gst_element_set_state(pipeline_, GST_STATE_NULL);
        GstState cur = GST_STATE_NULL, pending = GST_STATE_NULL;
        // Wait up to 1s for state change to complete
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
    // Restart with same params (safe teardown + re-build)
    std::cout << "Restarting pipeline for loop..." << std::endl;
    cleanup_pipeline();

    // Small delay to ensure NVDEC userspace driver is fully torn down
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    std::string err;
    if (!build_and_start_pipeline(encoder_name_, /*max_buffers=*/8, &err)) {
        // If our previously chosen encoder fails (e.g., nvjpegenc disappeared), try the other one.
        std::cerr << "Primary restart failed (" << err << "). Trying alternate encoder..." << std::endl;
        const std::string alt = (encoder_name_ == "nvjpegenc") ? "jpegenc" : "nvjpegenc";
        err.clear();
        if (!build_and_start_pipeline(alt, /*max_buffers=*/8, &err)) {
            std::cerr << "Alternate restart also failed: " << err << std::endl;
            running_.store(false);
            return false;
        }
    }
    return true;
}

bool GstPipelineWrapper::try_seek_to_start() {
    // Some demuxers behave better if we pause before seeking
    if (!pipeline_) return false;

    // Pause
    gst_element_set_state(pipeline_, GST_STATE_PAUSED);
    GstState cur = GST_STATE_PAUSED, pending = GST_STATE_VOID_PENDING;
    gst_element_get_state(pipeline_, &cur, &pending, 500 * GST_MSECOND);

    // Seek to 0 (flush + keyunit)
    gboolean ok = gst_element_seek(
        pipeline_,
        1.0,                         // rate
        GST_FORMAT_TIME,
        GstSeekFlags(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
        GST_SEEK_TYPE_SET, 0,        // start at 0
        GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE
    );

    if (!ok) return false;

    // Back to playing
    gst_element_set_state(pipeline_, GST_STATE_PLAYING);
    gst_element_get_state(pipeline_, &cur, &pending, 500 * GST_MSECOND);
    return true;
}

void GstPipelineWrapper::handle_eos() {
    std::cout << "End of stream reached (frame " << frames_processed_ << ")" << std::endl;

    if (!loop_enabled_ || !running_.load()) {
        std::cout << "Loop disabled; stopping." << std::endl;
        running_.store(false);
        return;
    }

    std::cout << "Attempting to loop..." << std::endl;
    // First try the efficient path (seek-to-start)
    if (try_seek_to_start()) {
        std::cout << "✓ Successfully seeked to beginning" << std::endl;
        return;
    }

    // Fallback: full restart. Do it on a separate thread but with lifecycle lock inside.
    std::thread([this]() {
        std::lock_guard<std::mutex> lk(this->lifecycle_mtx_);
        if (!this->running_.load()) return; // aborted meanwhile
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

    // Update width/height lazily from caps (once)
    if (self->width_ == 0 || self->height_ == 0) {
        if (GstCaps* caps = gst_sample_get_caps(sample)) {
            if (GstStructure* st = gst_caps_get_structure(caps, 0)) {
                int w = 0, h = 0;
                if (gst_structure_get_int(st, "width", &w))  self->width_  = w;
                if (gst_structure_get_int(st, "height", &h)) self->height_ = h;
            }
        }
    }

    GstBuffer* buf = gst_sample_get_buffer(sample);
    if (!buf) {
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    GstMapInfo info;
    if (gst_buffer_map(buf, &info, GST_MAP_READ)) {
        // Log first few and then periodic frames
        if (self->frames_processed_ < 5) {
            std::cout << "Frame " << (self->frames_processed_ + 1)
                      << " - JPEG size: " << info.size << " bytes" << std::endl;
        } else if ((self->frames_processed_ % 100) == 0) {
            std::cout << "Processed " << self->frames_processed_ << " frames" << std::endl;
        }

        try {
            self->frame_cb_(static_cast<const uint8_t*>(info.data), info.size);
            self->frames_processed_++;
        } catch (const std::exception& ex) {
            std::cerr << "Frame callback threw exception: " << ex.what() << std::endl;
        }
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
            if (err) g_error_free(err);
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
