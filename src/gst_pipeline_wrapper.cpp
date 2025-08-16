#include "gst_pipeline_wrapper.hpp"
#include <iostream>

GstPipelineWrapper::GstPipelineWrapper() {
    // Initialize GStreamer for the process. gst_init is safe to call multiple times in many builds,
    // but we call it here once when wrapper is constructed.
    gst_init(nullptr, nullptr);
}

GstPipelineWrapper::~GstPipelineWrapper() {
    stop();
}

bool GstPipelineWrapper::start(const std::string& file_path, FrameCallback cb, bool loop, int max_buffers) {
    if (running_.load()) return false;
    frame_cb_ = std::move(cb);

    // Build a pipeline string optimized for Jetson:
    // filesrc -> qtdemux -> h264parse -> nvv4l2decoder -> nvvidconv -> video/x-raw(memory:NVMM),format=NV12 -> nvjpegenc -> appsink
    // Keep NVMM buffers until nvjpegenc; appsink receives encoded JPEG bytes.
    // If your content is h265 or not mp4, you'll need to tweak the parse/demux elements accordingly.
    std::string pipeline_str =
        "filesrc location=\"" + file_path + "\" ! qtdemux name=demux "
        "demux.video_0 ! queue ! h264parse ! nvv4l2decoder enable-max-performance=1 ! "
        "nvvidconv ! video/x-raw(memory:NVMM),format=NV12 ! nvjpegenc ! "
        "appsink name=mysink emit-signals=true max-buffers=" + std::to_string(max_buffers) + " drop=true sync=false";

    GError* err = nullptr;
    pipeline_ = gst_parse_launch(pipeline_str.c_str(), &err);
    if (!pipeline_) {
        if (err) {
            std::cerr << "GStreamer pipeline parse error: " << err->message << std::endl;
            g_error_free(err);
        } else {
            std::cerr << "GStreamer pipeline parse failed (unknown error)." << std::endl;
        }
        return false;
    }

    appsink_ = gst_bin_get_by_name(GST_BIN(pipeline_), "mysink");
    if (!appsink_) {
        std::cerr << "GStreamer: failed to get appsink from pipeline." << std::endl;
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
        return false;
    }

    // Ensure appsink properties (safety)
    gst_app_sink_set_emit_signals(GST_APP_SINK(appsink_), TRUE);
    gst_app_sink_set_max_buffers(GST_APP_SINK(appsink_), max_buffers);
    gst_app_sink_set_drop(GST_APP_SINK(appsink_), TRUE);

    // Connect new-sample signal to our static handler
    g_signal_connect(appsink_, "new-sample", G_CALLBACK(&GstPipelineWrapper::on_new_sample), this);

    // Set pipeline to PLAYING
    GstStateChangeReturn sret = gst_element_set_state(pipeline_, GST_STATE_PLAYING);
    if (sret == GST_STATE_CHANGE_FAILURE) {
        std::cerr << "GStreamer: failed to set pipeline to PLAYING." << std::endl;
        gst_object_unref(appsink_);
        gst_object_unref(pipeline_);
        appsink_ = nullptr;
        pipeline_ = nullptr;
        return false;
    }

    running_.store(true);
    return true;
}

void GstPipelineWrapper::stop() {
    if (!running_.load()) return;
    running_.store(false);

    if (pipeline_) {
        gst_element_set_state(pipeline_, GST_STATE_NULL);
    }
    if (appsink_) {
        gst_object_unref(appsink_);
        appsink_ = nullptr;
    }
    if (pipeline_) {
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
    }
    frame_cb_ = nullptr;
}

GstFlowReturn GstPipelineWrapper::on_new_sample(GstAppSink* sink, gpointer user_data) {
    auto* self = static_cast<GstPipelineWrapper*>(user_data);
    if (!self || !self->frame_cb_) return GST_FLOW_OK;

    GstSample* sample = gst_app_sink_pull_sample(sink);
    if (!sample) return GST_FLOW_OK;

    GstBuffer* buf = gst_sample_get_buffer(sample);
    if (!buf) {
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    GstMapInfo info;
    if (gst_buffer_map(buf, &info, GST_MAP_READ)) {
        // Call user callback with JPEG bytes (nvjpegenc outputs JPEG stream)
        try {
            self->frame_cb_(static_cast<const uint8_t*>(info.data), info.size);
        } catch (const std::exception& ex) {
            std::cerr << "GstPipelineWrapper: frame callback threw: " << ex.what() << std::endl;
        }
        gst_buffer_unmap(buf, &info);
    } else {
        std::cerr << "GstPipelineWrapper: failed to map GstBuffer." << std::endl;
    }

    gst_sample_unref(sample);
    return GST_FLOW_OK;
}
