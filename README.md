# Video Playback Module
A Viam `camera` component for local video file playback, built with a high-performance, parallelized architecture designed for streaming high-resolution video files.

## Model `hunter:video-playback:camera`
This model implements the `rdk:component:camera` API by decoding and streaming a video file.

### Configuration

```json
{
  "video_path": "/Users/hunter.volkman/Developer/viam-video-playback/test-videos/15099728-uhd_3840_2160_25fps.mp4",
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
  "actual_fps": 25.049261083743843
}
```