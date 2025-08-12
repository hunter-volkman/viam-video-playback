#include <viam/sdk/common/instance.hpp>
#include <viam/sdk/components/camera.hpp>
#include <viam/sdk/module/service.hpp>
#include <viam/sdk/resource/resource.hpp>
#include <iostream>
#include "video_playback_camera.hpp"

using namespace viam::sdk;

int main(int argc, char **argv) {
    std::cout << "Video Playback Module starting..." << std::endl;
    
    Instance viam_instance;
    
    std::cout << "Viam Instance created" << std::endl;
    
    API camera_api = API("rdk", "component", "camera");
    
    std::shared_ptr<ModelRegistration> mr = std::make_shared<ModelRegistration>(
        camera_api,
        hunter::video_playback::VideoPlaybackCamera::model(),
        [](Dependencies deps, ResourceConfig cfg) -> std::shared_ptr<Resource> { 
            return hunter::video_playback::VideoPlaybackCamera::create(deps, cfg); 
        }
    );

    std::cout << "Model registration created" << std::endl;

    auto service = std::make_shared<ModuleService>(
        argc, argv, 
        std::vector<std::shared_ptr<ModelRegistration>>{mr}
    );
    
    std::cout << "Starting module service..." << std::endl;
    service->serve();
    
    return 0;
}
