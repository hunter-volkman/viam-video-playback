# Video Playback Module
A Viam `camera` component for local video file playback with hardware acceleration support.

## Model `hunter:video-playback:camera`
This model implements the `rdk:component:camera` API to stream a video file as a camera component, enabling repetable testing of ML vision services with consistent input. Supports hardware-accelerated H.264/HEVC decoding on NVIDIA Jetson platform. Automatic fallback to software decoding.

### Configuration
The following attribute template can be used to configure this model:

```json
{
  "video_path": "<string>",
  "loop": <boolean>,
  "target_fps": <integer>,
  "jpeg_quality_level": <integer>,
  "max_resolution": <integer>,
  "use_hardware_acceleration": <boolean>
}
```

#### Attributes

The following attributes are available for this model:

| Name          | Type   | Inclusion | Description                |
|---------------|--------|-----------|----------------------------|
| `video_path` | string  | Required  | Absolute path to the video file to stream. |
| `loop` | boolean | Optional  | Loop video playback when end is reached. Default: `true`. |
| `target_fps` | integer | Optional  |  Target frame rate. Use 0 for source FPS. Default: `0`. |
| `jpeg_quality_level` | integer | Optional  | JPEG compression quality (2-31, lower = better quality). Default: `15`. |
| `max_resolution` | integer | Optional  | Maximum width/height in pixels. Use 0 for no scaling. Default: `0`. |
| `use_hardware_acceleration` | boolean | Optional  | Enable hardware decoding on supported platforms. Default: `false`. |

#### Example Configuration

```json
{
  "video_path": "/home/user/videos/test.mp4",
  "loop": true,
  "target_fps": 25,
  "jpeg_quality_level": 20,
  "max_resolution": 1920,
  "use_hardware_acceleration": true
}
```

### DoCommand

The camera supports the following commands via the `do_command` method:

#### get_stats
Check the real-time performance and status of the video playback pipeline.

```json
{
  "command": "get_stats"
}
```

**Response:**
```json
{
  "encoder_threads": 4,
  "frames_decoded": 5085,
  "frames_encoded": 5085,
  "frames_dropped_producer": 0,
  "frames_dropped_consumer": 0,
  "encoder_queue_size": 0,
  "actual_fps": 25.049261083743843,
  "decoder_name": "h264",
  "hardware_acceleration": true,
  "output_width": 1920,
  "output_height": 1080
}
```

### Platform Support
* Linux ARM64 (Jetson): Hardware-accelerated H.264/HEVC decoding via NVDEC
* macOS ARM64 (Apple Silicon): Multi-threaded software decoding
* Linux x86_64: Multi-threaded software decoding

See BUILD.md for detailed build instructions.