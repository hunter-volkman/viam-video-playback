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
    file_path_ = file_path;  // Store for potential restart

    // Build pipeline optimized for Jetson
    // Using software jpegenc to avoid libjpeg ABI mismatch
    std::stringstream pipeline_ss;
    pipeline_ss << "filesrc location=\"" << file_path << "\" ! ";
    pipeline_ss << "qtdemux ! queue ! ";
    pipeline_ss << "h264parse ! ";
    pipeline_ss << "nvv4l2decoder enable-max-performance=1 ! ";
    pipeline_ss << "nvvidconv ! video/x-raw,format=I420 ! ";
    pipeline_ss << "jpegenc quality=85 ! ";
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
    bus_watch_id_ = gst_bus_add_watch(bus, &GstPipelineWrapper::bus_callback, this);
    gst_object_unref(bus);

    // Start pipeline
    GstStateChangeReturn sret = gst_element_set_state(pipeline_, GST_STATE_PLAYING);
    if (sret == GST_STATE_CHANGE_FAILURE) {
        std::cerr << "Failed to set pipeline to PLAYING state" << std::endl;
        cleanup_pipeline();
        return false;
    }

    running_.store(true);
    frames_processed_ = 0;
    
    std::cout << "✓ GStreamer pipeline started successfully" << std::endl;
    std::cout << "  Using: nvv4l2decoder (hardware) → jpegenc (software)" << std::endl;
    std::cout << "  Loop enabled: " << (loop_enabled_ ? "yes" : "no") << std::endl;
    
    return true;
}

void GstPipelineWrapper::stop() {
    if (!running_.load()) return;
    
    running_.store(false);
    
    std::cout << "Stopping GStreamer pipeline (processed " 
              << frames_processed_ << " frames)" << std::endl;

    cleanup_pipeline();
    frame_cb_ = nullptr;
}

void GstPipelineWrapper::cleanup_pipeline() {
    // Remove bus watch
    if (bus_watch_id_ > 0) {
        g_source_remove(bus_watch_id_);
        bus_watch_id_ = 0;
    }

    if (pipeline_) {
        // Send EOS and wait for it to propagate
        gst_element_send_event(pipeline_, gst_event_new_eos());
        
        // Wait up to 500ms for EOS
        GstBus* bus = gst_element_get_bus(pipeline_);
        GstMessage* msg = gst_bus_timed_pop_filtered(bus, 500 * GST_MSECOND,
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
}

bool GstPipelineWrapper::restart_pipeline() {
    std::cout << "Restarting pipeline for loop..." << std::endl;
    
    // Store callback temporarily
    auto temp_cb = frame_cb_;
    bool temp_loop = loop_enabled_;
    std::string temp_path = file_path_;
    
    // Clean up current pipeline
    cleanup_pipeline();
    
    // Small delay to ensure clean state
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Restart with same parameters
    return start(temp_path, temp_cb, temp_loop, 4);
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
        // Log first few frames and periodically
        if (self->frames_processed_ < 5) {
            std::cout << "Frame " << (self->frames_processed_ + 1) 
                      << " - JPEG size: " << info.size << " bytes" << std::endl;
        } else if (self->frames_processed_ % 100 == 0) {
            std::cout << "Processed " << self->frames_processed_ << " frames" << std::endl;
        }
        
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
        case GST_MESSAGE_EOS: {
            std::cout << "End of stream reached (frame " 
                      << self->frames_processed_ << ")" << std::endl;
            
            if (self->loop_enabled_ && self->running_.load()) {
                std::cout << "Attempting to loop..." << std::endl;
                
                // Method 1: Try seeking first (most efficient if it works)
                gboolean seek_success = gst_element_seek_simple(
                    self->pipeline_,
                    GST_FORMAT_TIME,
                    static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
                    0
                );
                
                if (seek_success) {
                    std::cout << "✓ Successfully seeked to beginning" << std::endl;
                    // Reset frame counter for new loop
                    // Note: Don't reset frames_processed_ if you want cumulative count
                } else {
                    std::cout << "Seek failed, performing full pipeline restart..." << std::endl;
                    
                    // Method 2: Full restart (bulletproof but has a brief pause)
                    // We need to do this in a separate thread to avoid deadlock
                    std::thread restart_thread([self]() {
                        if (self->restart_pipeline()) {
                            std::cout << "✓ Pipeline restarted successfully" << std::endl;
                        } else {
                            std::cerr << "ERROR: Failed to restart pipeline!" << std::endl;
                            self->running_.store(false);
                        }
                    });
                    restart_thread.detach();
                }
            } else {
                std::cout << "End of stream, stopping (loop disabled)" << std::endl;
                self->running_.store(false);
            }
            break;
        }
            
        case GST_MESSAGE_ERROR: {
            GError* err;
            gchar* debug_info;
            gst_message_parse_error(msg, &err, &debug_info);
            std::cerr << "GStreamer ERROR: " << err->message << std::endl;
            if (debug_info) {
                std::cerr << "Debug info: " << debug_info << std::endl;
                g_free(debug_info);
            }
            g_clear_error(&err);
            
            // Stop the pipeline on error
            self->running_.store(false);
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
        
        case GST_MESSAGE_STATE_CHANGED: {
            // Only log state changes from the pipeline itself
            if (GST_MESSAGE_SRC(msg) == GST_OBJECT(self->pipeline_)) {
                GstState old_state, new_state, pending_state;
                gst_message_parse_state_changed(msg, &old_state, &new_state, &pending_state);
                
                // Only log significant state changes
                if (new_state == GST_STATE_PLAYING || new_state == GST_STATE_PAUSED) {
                    std::cout << "Pipeline state: " 
                              << gst_element_state_get_name(new_state) << std::endl;
                }
            }
            break;
        }
        
        default:
            break;
    }
    
    return TRUE;  // Keep watching
}