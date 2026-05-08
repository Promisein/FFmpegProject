# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & Run

```bash
cd build
cmake .. -G "MinGW Makefiles"
cmake --build .

# DLLs must be on PATH at runtime:
PATH="C:/Users/Jianing/Desktop/C++Project/FFmpegProject/ffmpeg-4.4-full_build-shared/bin:$PATH" \
  ./FFmpegProject.exe
```

The project requires FFmpeg 4.4 shared libraries. Update `FFMPEG_ROOT` in `CMakeLists.txt:7` if the path differs in your environment.

No tests or linter are configured.

## Architecture: 6-thread transcode pipeline

```
Input → [Demux] → PacketQueue → [Decoder ×2] → RingBuffer → [Encoder ×2] → DeepCopyPacketQueue → [Mux] → Output MP4
```

Each stage is a `std::thread` launched from `main.cpp`. Threads communicate exclusively through queues owned by a **`Pipeline`** object — there are no global queues.

### Pipeline class (`include/pipeline.h`, `src/pipeline.cpp`)

`Pipeline` owns all 6 inter-thread queues and provides unified error propagation (`report_error()` / `has_error()` / `get_error()`). Thread functions receive only the specific queue references they need plus a `Pipeline*` for error reporting.

### RAII wrappers (`include/ffmpeg_raii.h`)

All FFmpeg C resources are managed via `std::unique_ptr` with custom deleters:

| Alias | Wraps | Deleter |
|-------|-------|---------|
| `CodecContextPtr` | `AVCodecContext*` | `avcodec_free_context` |
| `InputFormatContextPtr` | `AVFormatContext*` | `avformat_close_input` |
| `FramePtr` | `AVFrame*` | `av_frame_free` |
| `PacketPtr` | `AVPacket*` | `av_packet_free` |
| `CodecParametersPtr` | `AVCodecParameters*` | `avcodec_parameters_free` |

Error paths in thread functions are simple `return` — RAII handles all cleanup automatically, eliminating the manual `goto`-style resource freeing scattered across the old code.

### Queue implementations

- **`PacketQueue<T>`** (`include/packet_queue.h`) — Simple `std::queue` + mutex + CV. `pop()` blocks until data is available.
- **`RingBuffer<T>`** (`include/ring_buffer.h`) — Fixed-capacity (30) circular buffer. Pre-allocates all AVFrame/AVPacket pointers. Uses `av_frame_ref`/`av_packet_ref` for copies. Blocks producers when full, consumers when empty. `flush()` wakes all waiters for clean shutdown.
- **`DeepCopyPacketQueue`** (`include/deep_copy_packey_queue.h`) — Uses `av_packet_clone()` on push, `av_packet_move_ref()` + `av_packet_free()` on pop. `mark_done()` signals end-of-stream.

### Thread termination protocol

1. Demuxer sends flush packet (`.data = nullptr`) to both `PacketQueue`s on EOF.
2. Each decoder treats flush as drain signal, sends `nullptr` to `avcodec_send_packet()`, collects remaining frames, then calls `RingBuffer::flush()`.
3. Each encoder detects empty ring buffer, drains with `nullptr` frame, then calls `DeepCopyPacketQueue::mark_done()`.
4. Muxer exits when both input queues are empty and `done`.

### Codec choices

- **Video**: MPEG4 (`AV_CODEC_ID_MPEG4`, codec_tag `0x7634706d`), YUV420P, 1 Mbps, GOP=10, zero B-frames.
- **Audio**: AC3 (`AV_CODEC_ID_AC3`), FLTP, 128 kbps, fixed 1536-sample frames.
- **Output**: MP4 container.

### A/V sync (muxer)

The muxer recalculates audio PTS from an accumulated sample counter (does not trust encoder PTS) and uses a min-heap (`std::priority_queue` + `PacketComparator`) to interleave packets in correct presentation order.

### Video rotate module (not linked)

`src/video_rotate.cpp` implements YUV420P 90/180/270-degree rotation. Commented out in `CMakeLists.txt`. Not part of the active build.

## Common pitfalls

- Audio encoder assumes input is FLTP. If the decoder outputs a different sample format, encoding fails — no `swresample` conversion.
- Input/output paths are hardcoded in `main.cpp` as `../input3.mp4` / `../output.mp4` (relative to build directory).
