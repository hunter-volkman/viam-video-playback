#pragma once

#include <vector>
#include <queue>
#include <mutex>
#include <memory>

namespace viam {
namespace video_stream {

class FramePool {
public:
    explicit FramePool(size_t pool_size);
    ~FramePool() = default;
    
    std::vector<uint8_t> get_buffer(size_t size);
    
private:
    std::vector<std::vector<uint8_t>> buffers_;
    std::queue<size_t> available_;
    std::mutex mutex_;
    size_t pool_size_;
};

} // namespace video_stream
} // namespace viam
