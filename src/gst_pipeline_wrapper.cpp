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

    // Build pipeline optimized for Jetson
    // IMPORTANT: Using software jpegenc to avoid libjpeg ABI mismatch with nvjpegenc
    // The hardware decoder (nvv4l2decoder) still provides acceleration
    std::stringstream pipeline_ss;
    pipeline_ss << "filesrc location=\"" << file_path << "\" ! ";
    pipeline_ss << "qtdemux ! queue ! ";
    pipeline_ss << "h264parse ! ";
    
    // Use hardware decoder
    pipeline_ss << "nvv4l2decoder enable-max-performance=1 ! ";
    
    // Convert to CPU memory with I420 format for jpegenc
    // This avoids the libjpeg mismatch that nvjpegenc causes
    pipeline_ss << "nvvidconv ! video/x-raw,format=I420 ! ";
    
    // Software JPEG encoder - more stable and avoids ABI issues
    pipeline_ss << "jpegenc quality=85 ! ";
    
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
    
    std::cout << "✓ GStreamer pipeline started successfully" << std::endl;
    std::cout << "  Using: nvv4l2decoder (hardware) → jpegenc (software)" << std::endl;
    std::cout << "  This avoids libjpeg ABI mismatch issues" << std::endl;
    
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
        // Log first few frames for debugging
        if (self->frames_processed_ < 5) {
            std::cout << "Frame " << (self->frames_processed_ + 1) 
                      << " - JPEG size: " << info.size << " bytes" << std::endl;
        }
        
        // Deliver JPEG frame to callback
        try {
            self->frame_cb_(static_cast<const uint8_t*>(info.data), info.size);
            self->frames_processed_++;
            
            // Log progress periodically
            if (self->frames_processed_ % 100 == 0) {
                std::cout << "Processed " << self->frames_processed_ << " frames" << std::endl;
            }
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
            std::cout << "End of stream reached" << std::endl;
            if (self->loop_enabled_ && self->running_.load()) {
                // Seek back to beginning for loop
                if (!gst_element_seek_simple(self->pipeline_, GST_FORMAT_TIME,
                    static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT), 0)) {
                    std::cerr << "Failed to seek to beginning for loop" << std::endl;
                } else {
                    std::cout << "Looped back to beginning" << std::endl;
                }
            }
            break;
            
        case GST_MESSAGE_ERROR: {
            GError* err;
            gchar* debug_info;
            gst_message_parse_error(msg, &err, &debug_info);
            std::cerr << "GStreamer error: " << err->message << std::endl;
            if (debug_info) {
                std::cerr << "Debug info: " << debug_info << std::endl;
                g_free(debug_info);
            }
            g_clear_error(&err);
            break;
        }
        
        case GST_MESSAGE_WARNING: {
            GError* err;
            gchar* debug_info;
            gst_message_parse_warning(msg, &err, &debug_info);
            std::cerr << "GStreamer warning: " << err->message << std::endl;
            if (debug_info) {
                g_free(debug_info);
            }
            g_clear_error(&err);
            break;
        }
        
        default:
            break;
    }
    
    return TRUE;  // Keep watching
}