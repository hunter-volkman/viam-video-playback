#include <viam/sdk/common/instance.hpp>
#include <viam/sdk/components/camera.hpp>
#include <viam/sdk/module/service.hpp>
#include <viam/sdk/resource/resource.hpp>
#include <iostream>
#include "video_stream_camera.hpp"

using namespace viam::sdk;

int main(int argc, char **argv) {
    std::cout << "Video Stream Module starting..." << std::endl;
    
    // CRITICAL: Every Viam C++ SDK program must have one Instance object
    // This must be created before any other SDK objects!
    Instance viam_instance;
    
    std::cout << "Viam Instance created" << std::endl;
    
    // Create model registration
    API camera_api = API("rdk", "component", "camera");
    
    std::shared_ptr<ModelRegistration> mr = std::make_shared<ModelRegistration>(
        camera_api,
        viam::video_stream::VideoStreamCamera::model(),
        [](Dependencies deps, ResourceConfig cfg) -> std::shared_ptr<Resource> { 
            return viam::video_stream::VideoStreamCamera::create(deps, cfg); 
        }
    );

    std::cout << "Model registration created" << std::endl;

    // Create and start the module service
    auto service = std::make_shared<ModuleService>(
        argc, argv, 
        std::vector<std::shared_ptr<ModelRegistration>>{mr}
    );
    
    std::cout << "Starting module service..." << std::endl;
    service->serve();
    
    return 0;
}
