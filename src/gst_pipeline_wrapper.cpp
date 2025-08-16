#include "gst_pipeline_wrapper.hpp"
#include <iostream>
#include <sstream>

GstPipelineWrapper::GstPipelineWrapper() {
    // Initialize GStreamer once per process
    if (!gst_is_initialized()) {
        gst_init(nullptr, nullptr);
        std::cout << "GStreamer initialized: " << gst_version_string() << std::endl;
    }
}

GstPipelineWrapper::~GstPipelineWrapper() {
    stop();
}

bool GstPipelineWrapper::start(const std::string& file_path, FrameCallback cb, bool loop, int max_buffers) {
    if (running_.load()) {
        std::cerr << "GStreamer pipeline already running" << std::endl;
        return false;
    }
    
    frame_cb_ = std::move(cb);
    loop_enabled_ = loop;

    // Build optimized pipeline for Jetson hardware acceleration
    // This pipeline uses NVIDIA's hardware decoders and JPEG encoder
    std::stringstream pipeline_ss;
    pipeline_ss << "filesrc location=\"" << file_path << "\" ! ";
    
    // Use qtdemux for MP4/MOV files, adapt as needed for other containers
    pipeline_ss << "qtdemux name=demux ! queue ! ";
    
    // Parse and decode H.264/H.265 using NVIDIA hardware
    pipeline_ss << "h264parse ! nvv4l2decoder enable-max-performance=1 ! ";
    
    // Convert to NVMM memory for efficient GPU processing
    pipeline_ss << "nvvidconv ! video/x-raw(memory:NVMM),format=NV12 ! ";
    
    // Hardware JPEG encoding
    pipeline_ss << "nvjpegenc quality=85 ! ";
    
    // Output to application
    pipeline_ss << "appsink name=mysink emit-signals=true "
                << "max-buffers=" << max_buffers << " drop=true sync=false";

    std::string pipeline_str = pipeline_ss.str();
    std::cout << "Creating GStreamer pipeline: " << pipeline_str << std::endl;

    // Parse pipeline
    GError* err = nullptr;
    pipeline_ = gst_parse_launch(pipeline_str.c_str(), &err);
    if (!pipeline_) {
        if (err) {
            std::cerr << "GStreamer pipeline parse error: " << err->message << std::endl;
            g_error_free(err);
        } else {
            std::cerr << "GStreamer pipeline parse failed (unknown error)" << std::endl;
        }
        return false;
    }

    // Get appsink element
    appsink_ = gst_bin_get_by_name(GST_BIN(pipeline_), "mysink");
    if (!appsink_) {
        std::cerr << "Failed to get appsink from pipeline" << std::endl;
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
        return false;
    }

    // Configure appsink
    gst_app_sink_set_emit_signals(GST_APP_SINK(appsink_), TRUE);
    gst_app_sink_set_max_buffers(GST_APP_SINK(appsink_), max_buffers);
    gst_app_sink_set_drop(GST_APP_SINK(appsink_), TRUE);

    // Connect signal handlers
    g_signal_connect(appsink_, "new-sample", G_CALLBACK(&GstPipelineWrapper::on_new_sample), this);
    
    // Add bus watch for error handling and EOS
    GstBus* bus = gst_element_get_bus(pipeline_);
    gst_bus_add_watch(bus, &GstPipelineWrapper::bus_callback, this);
    gst_object_unref(bus);

    // Start pipeline
    GstStateChangeReturn sret = gst_element_set_state(pipeline_, GST_STATE_PLAYING);
    if (sret == GST_STATE_CHANGE_FAILURE) {
        std::cerr << "Failed to set pipeline to PLAYING state" << std::endl;
        gst_object_unref(appsink_);
        gst_object_unref(pipeline_);
        appsink_ = nullptr;
        pipeline_ = nullptr;
        return false;
    }

    running_.store(true);
    frames_processed_ = 0;
    
    std::cout << "GStreamer pipeline started successfully" << std::endl;
    return true;
}

void GstPipelineWrapper::stop() {
    if (!running_.load()) return;
    
    running_.store(false);
    
    std::cout << "Stopping GStreamer pipeline (processed " 
              << frames_processed_ << " frames)" << std::endl;

    if (pipeline_) {
        // Send EOS and wait for it to propagate
        gst_element_send_event(pipeline_, gst_event_new_eos());
        
        // Wait up to 1 second for EOS
        GstBus* bus = gst_element_get_bus(pipeline_);
        GstMessage* msg = gst_bus_timed_pop_filtered(bus, GST_SECOND,
            static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
        
        if (msg) {
            gst_message_unref(msg);
        }
        gst_object_unref(bus);
        
        // Stop pipeline
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
    if (!self || !self->frame_cb_ || !self->running_.load()) {
        return GST_FLOW_OK;
    }

    GstSample* sample = gst_app_sink_pull_sample(sink);
    if (!sample) {
        return GST_FLOW_OK;
    }

    GstBuffer* buf = gst_sample_get_buffer(sample);
    if (!buf) {
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    GstMapInfo info;
    if (gst_buffer_map(buf, &info, GST_MAP_READ)) {
        // Deliver JPEG frame to callback
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

gboolean GstPipelineWrapper::bus_callback(GstBus* bus, GstMessage* msg, gpointer user_data) {
    auto* self = static_cast<GstPipelineWrapper*>(user_data);
    
    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_EOS:
            std::cout << "GStreamer: End of stream reached" << std::endl;
            if (self->loop_enabled_ && self->running_.load()) {
                // Seek back to beginning for loop
                gst_element_seek_simple(self->pipeline_, GST_FORMAT_TIME,
                    static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT), 0);
            }
            break;
            
        case GST_MESSAGE_ERROR: {
            GError* err;
            gchar* debug_info;
            gst_message_parse_error(msg, &err, &debug_info);
            std::cerr << "GStreamer error: " << err->message << std::endl;
            std::cerr << "Debug info: " << (debug_info ? debug_info : "none") << std::endl;
            g_clear_error(&err);
            g_free(debug_info);
            break;
        }
        
        case GST_MESSAGE_WARNING: {
            GError* err;
            gchar* debug_info;
            gst_message_parse_warning(msg, &err, &debug_info);
            std::cerr << "GStreamer warning: " << err->message << std::endl;
            g_clear_error(&err);
            g_free(debug_info);
            break;
        }
        
        default:
            break;
    }
    
    return TRUE;  // Keep watching
}