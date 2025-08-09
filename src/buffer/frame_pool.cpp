#include "frame_pool.hpp"

namespace viam {
namespace video_stream {

FramePool::FramePool(size_t pool_size) : pool_size_(pool_size) {
    buffers_.reserve(pool_size);
    for (size_t i = 0; i < pool_size; ++i) {
        buffers_.emplace_back();
        available_.push(i);
    }
}

std::vector<uint8_t> FramePool::get_buffer(size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!available_.empty()) {
        size_t idx = available_.front();
        available_.pop();
        
        if (buffers_[idx].size() != size) {
            buffers_[idx].resize(size);
        }
        
        return buffers_[idx];
    }
    
    return std::vector<uint8_t>(size);
}

} // namespace video_stream
} // namespace viam
