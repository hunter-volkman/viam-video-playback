# Video Stream Module
A Viam module for replaying a local video file as a high-performance live stream (for tests).

## Model viam:video-stream:replay
This model implements the `rdk:component:camera` API by decoding and streaming a video file.

### Configuration

```json
{
  "video_path": "test-video.mp4",
  "loop": true,
  "target_fps": 25
}
```

#### Required Attributes

| Name | Type | Description |
|------|------|-----------|
| `video_path` | string | The absolute file path to the video file to be streamed |

#### Optional Attributes

| Name | Type | Default | Description |
|------|------|-----------|-------------|
| `loop` | boolean | true | Automatically loop the video when it ends |
| `target_fps` | integer | Source FPS | Desired frame rate for the stream |
| `jpeg_quality_level` | integer | 15 | JPEG quality from 1 (best) to 31 (fastest) |
| `output_width` | integer | Source width | Width in pixels to resize the output stream to |
| `output_height` | integer | Source height | Height in pixels to resize the output stream to |


### DoCommand

The camera supports the following commands via the `do_command` method:

#### get_stats
Check the real-time performance and status of the video stream pipeline.

```json
{
  "command": "get_stats"
}
```

**Response:**
```json
{
  "actual_fps": 25.03,
  "encoder_queue_size": 0,
  "encoder_threads": 4,
  "frames_decoded": 20956,
  "frames_encoded": 20954,
  "frames_dropped_consumer": 0,
  "frames_dropped_producer": 2
}
```