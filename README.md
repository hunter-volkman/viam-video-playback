# Video Playback Module
A Viam `camera` component for local video file playback (with hardware acceleration support).

## Model `hunter:video-playback:camera`
This model implements the `rdk:component:camera` API by decoding and streaming video files at consistent FPS for testing ML vision services with repeatable input.

### Configuration

```json
{
  "video_path": "/path/to/video.mp4",
  "loop": true,
  "target_fps": 25,
  "use_hardware_acceleration": true
}
```

#### Attributes

The following attributes are available for this model:

| Name          | Type   | Inclusion | Description                |
|---------------|--------|-----------|----------------------------|
| `video_path` | string  | Required  | The absolute file path to the video file to be streamed (e.g., "/home/user/videos/test.mp4"). |
| `loop` | boolean | Optional  | Loop video playback. Defaults to `true`. |
| `target_fps` | integer | Optional  |  Target frame rate (0 = source FPS). Defaults to 0. |
| `jpeg_quality_level` | integer | Optional  | JPEG quality 2-31 (lower = better quality). Defaults to 15. |
| `max_resolution` | integer | Optional  | Max width/height (0 = no scaling). Defaults to 0. |
| `use_hardware_acceleration` | boolean | Optional  | Enable hardware acceleration (Jetson NVDEC). Defaults to `false`. |

#### Example Configuration

```json
{
  "video_path": "/home/jetson/Videos/test.mp4",
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
  "decoder_name": "h264_nvv4l2dec",
  "hardware_accel": true
}
```

### Supported Platforms
* **macOS** (Apple Silicon): Software decoding with multi-threading
* **Jetson** (ARM64): Hardware H.264 decoding with NVIDIA NVDEC