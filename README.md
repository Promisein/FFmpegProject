# FFmpegProject — 多线程视频转码管线 / Multithreaded Video Transcoding Pipeline

**语言 / Language:** [中文](#中文) | [English](#english)

---

## 中文

### 简介

基于 FFmpeg C API 构建的高性能视频批量转码工具。采用 **自定义无锁队列 + 两层级线程池架构**，每个文件内部以 **6 线程流水线** 并行解封装、解码、编码、封装，文件之间通过 **手写固定大小线程池** 实现多任务并发。支持运行时 YUV420P 像素级旋转、帧丢弃/复制倍速播放，全程 A/V 同步。

### 项目亮点

| 亮点 | 说明 |
|------|------|
| **两层级线程模型** | 上层：自研 `ThreadPool` 管理任务并发（3 个文件同时转码）；下层：每个任务内部 6 线程流水线（Demux → Video/Audio Dec → Video/Audio Enc → Mux） |
| **全链路 RAII 资源管理** | 5 种 `std::unique_ptr` + 自定义 Deleter 封装所有 FFmpeg C 资源（`AVFormatContext`、`AVCodecContext`、`AVFrame`、`AVPacket`、`AVCodecParameters`），零手动释放，异常安全 |
| **自定义线程安全队列** | 3 种队列实现：`PacketQueue<T>`（无界）、`RingBuffer<T>`（定长 30 环形缓冲，预分配 + `av_frame_ref` 零拷贝）、`DeepCopyPacketQueue`（深拷贝 + EOF 信号） |
| **Pipeline 集中错误传播** | 所有队列由 `Pipeline` 对象持有，任一线程出错通过 `atomic<bool>` 广播，`join` 后统一检查 |
| **编码器内处理链** | 旋转 & 倍速处理嵌入编码线程：`pop frame → 旋转 → 倍速 drop/dup → encode`，零额外内存分配 |
| **min-heap A/V 交错** | 封装器使用 `std::priority_queue` 按 PTS 排序输出，音频 PTS 从累计采样数独立计算，不依赖编码器 PTS |
| **优雅关闭协议** | 链式传播 EOF：Demux flush pkt → Decoder drain → RingBuffer::flush() → Encoder drain → mark_done() → Muxer write trailer |

### 项目架构

```
                        ┌──────────────────────────┐
                        │       main.cpp            │
                        │  解析 CLI · 扫描 input/   │
                        │  创建 ThreadPool · 汇总   │
                        └──────────┬───────────────┘
                                   │ submit N tasks
                                   ▼
                ┌──────────────────────────────────────┐
                │         ThreadPool (上层并发)         │
                │   pool_size = 3, 手写 std::queue +    │
                │   mutex + CV + atomic 计数器          │
                ├──────┬──────┬──────┬──────────────────┤
                │ Wkr1 │ Wkr2 │ Wkr3 │  ...             │
                └──┬───┴──┬───┴──┬───┘                  │
                   │      │      │                      │
                   ▼      ▼      ▼                      │
             transcode_single() × N                     │
        (每个任务内部 6 线程独立 Pipeline)                │
└──────────────────────────────────────────────────────┘

        ┌─────────────────────────────────────────┐
        │  transcode_single() — 6 线程转码管线     │
        │                                         │
        │  ┌────────┐   PacketQueue   ┌─────────┐ │
        │  │①Demux  │────────────────▶│②Vid Dec │──┐
        │  │ 解封装 │                 │ H.264→  │  │
        │  └───┬────┘                 │ YUV420P │  │  RingBuffer(30)
        │      │                      └─────────┘  │
        │      │          ┌─────────┐              │
        │      │          │③Aud Dec │              │
        │      └─────────▶│ AAC→FLTP│              │
        │    PacketQueue   └────┬────┘              │
        │                      │ RingBuffer(30)    │
        │                      │                   │
        │  ┌────────────────────▼─────────────────┐│
        │  │           ④ Video Encoder            ││
        │  │  pop → [VideoRotate 90/180/270]     ││
        │  │      → [Speed drop/dup ×0.5~×4]     ││
        │  │      → avcodec_send_frame (MPEG4)   ││
        │  └────────────────────┬─────────────────┘│
        │                       │ DeepCopyPktQueue │
        │  ┌────────────────────▼─────────────────┐│
        │  │           ⑤ Audio Encoder            ││
        │  │  pop → [Speed drop/dup ×0.5~×4]     ││
        │  │      → avcodec_send_frame (AC3)     ││
        │  └────────────────────┬─────────────────┘│
        │                       │ DeepCopyPktQueue │
        │                       ▼                  │
        │  ┌──────────────────────────────────────┐│
        │  │            ⑥ Muxer                    ││
        │  │  min-heap PTS interleave             ││
        │  │  → av_interleaved_write_frame()      ││
        │  └──────────────────┬───────────────────┘│
        │                     ▼                    │
        │              ../output/<name>/output.mp4  │
        └─────────────────────────────────────────┘
```

### RAII 资源封装

| 类型别名 | 包装资源 | 自定义 Deleter |
|----------|----------|----------------|
| `InputFormatContextPtr` | `AVFormatContext*` | `avformat_close_input` |
| `CodecContextPtr` | `AVCodecContext*` | `avcodec_free_context` |
| `FramePtr` | `AVFrame*` | `av_frame_free` |
| `PacketPtr` | `AVPacket*` | `av_packet_free` |
| `CodecParametersPtr` | `AVCodecParameters*` | `avcodec_parameters_free` |

### 线程安全队列

| 队列 | 文件 | 特点 |
|------|------|------|
| `PacketQueue<T>` | `include/packet_queue.h` | 无界 `std::queue` + mutex + CV，`pop()` 阻塞等待 |
| `RingBuffer<T>` | `include/ring_buffer.h` | 定长 30，预分配指针，`av_frame_ref` 引用传递，满则阻塞生产者，空则阻塞消费者，`flush()` 唤醒全部线程 |
| `DeepCopyPacketQueue` | `include/deep_copy_packey_queue.h` | `push` 调用 `av_packet_clone` 深拷贝，`pop` 用 `av_packet_move_ref` 转移所有权，`mark_done()` 标记流结束 |

### 线程终止协议

```
①Demux EOF → 发送 flush 包 (data=nullptr)
    │
    ▼
②/③Decoder → 收到 flush → avcodec_send_packet(nullptr) 冲刷
    │         收集残留帧 → RingBuffer::flush() 唤醒全部
    ▼
④/⑤Encoder → 检测 ring buffer 空 → send_frame(nullptr) 冲刷
    │         → mark_done()
    ▼
⑥Muxer → 等待两路 done && 队列空 → 写 trailer → 退出
```

### A/V 同步机制

音频 PTS 由封装器从累计采样数独立计算（`audio_accumulated_samples / sample_rate`），不信任编码器输出的 PTS。视频和音频编码包通过 min-heap（`std::priority_queue` + PTS 比较器）交错写入，保证输出文件播放同步。

### 编解码器选择

| 类型 | 编码 | 像素/采样格式 | 码率 | 其他 |
|------|------|---------------|------|------|
| 视频 | **MPEG4** (`AV_CODEC_ID_MPEG4`) | YUV420P | 1 Mbps | GOP=10, B-frames=0, `codec_tag=0x7634706d` |
| 音频 | **AC3** (`AV_CODEC_ID_AC3`) | FLTP | 128 kbps | 固定 1536 采样/帧 |
| 容器 | **MP4** | - | - | - |

### 目录结构

```
FFmpegProject/
├── main.cpp                        # 入口：CLI 解析、目录扫描、线程池调度
├── CMakeLists.txt                  # CMake 构建配置 (MinGW)
├── include/
│   ├── config.h                    # ProcessingConfig 跨线程配置传递
│   ├── common.h                    # 公共类型前向声明
│   ├── thread_pool.h               # 自定义固定大小线程池
│   ├── pipeline.h                  # Pipeline：持有 6 个队列 + 错误传播
│   ├── ffmpeg_raii.h               # RAII 封装 (5 种 unique_ptr)
│   ├── demux.h / mux.h             # 解封装 / 封装
│   ├── videodecoder.h / audiodecoder.h   # 视频 / 音频解码
│   ├── videoencoder.h / audioencoder.h   # 视频 / 音频编码
│   ├── video_rotate.h              # YUV420P 像素旋转
│   ├── packet_queue.h              # 无界线程安全队列
│   ├── ring_buffer.h               # 定长环形缓冲
│   └── deep_copy_packey_queue.h    # 深拷贝队列 + EOF 信号
├── src/                            # 各模块实现
├── input/                          # 输入文件目录 (扫描 *.mp4)
├── output/<filename>/              # 输出目录 (每个文件独立子目录)
├── ffmpeg-4.4-full_build-shared/   # FFmpeg 4.4 共享库
└── README.md
```

### 构建 & 运行

**依赖：** FFmpeg 4.4 共享库、CMake 3.27+、MinGW (Windows)

```bash
cd build
cmake .. -G "MinGW Makefiles"
cmake --build .
```

**运行：**

```bash
# DLL 路径需要加到 PATH
PATH="<ffmpeg>/bin:$PATH" ./FFmpegProject -rotate 90 -speed 1.5 -threads 3
```

### CLI 用法

```
FFmpegProject [-rotate <angle>] [-speed <ratio>] [-threads <n>]
  -rotate <angle>  视频旋转: 90, 180, 270
  -speed  <ratio>  播放速度: 0.5, 0.75, 1.25, 1.5, 2, 4
  -threads <n>     线程池并发数 (默认 3)

自动扫描 ../input/*.mp4，输出到 ../output/<文件名>/output.mp4

示例:
  ./FFmpegProject                                    # 默认参数，单文件
  ./FFmpegProject -rotate 90 -speed 1.5              # 旋转90° + 1.5倍速
  ./FFmpegProject -rotate 180 -speed 0.5 -threads 4  # 旋转180° + 0.5倍速 + 4并发
```

### 技术栈

- **语言:** C++17 (std::thread, std::atomic, std::filesystem, std::priority_queue)
- **媒体:** FFmpeg 4.4 C API (libavformat, libavcodec, libavutil)
- **构建:** CMake 3.27 + MinGW
- **平台:** Windows (可移植至 Linux)

---

## English

### Overview

A high-performance batch video transcoding tool built on the FFmpeg C API. It features a **custom lock-free queue + two-tier thread pool architecture**: each file is processed through a **6-thread internal pipeline** (demux, decode, encode, mux in parallel), while multiple files are processed concurrently via a **hand-written fixed-size thread pool**. Supports runtime YUV420P pixel-level rotation and frame drop/duplication speed change with full A/V sync.

### Highlights

| Highlight | Description |
|-----------|-------------|
| **Two-Tier Threading** | Upper: custom `ThreadPool` for task concurrency (3 files in parallel). Lower: 6-thread pipeline per task (Demux → Video/Audio Dec → Video/Audio Enc → Mux) |
| **Full RAII Resource Management** | 5 `std::unique_ptr` aliases with custom deleters wrapping all FFmpeg C resources. Zero manual `_free` calls, exception-safe |
| **Custom Thread-Safe Queues** | 3 implementations: `PacketQueue<T>` (unbounded), `RingBuffer<T>` (fixed 30, pre-allocated + `av_frame_ref` zero-copy), `DeepCopyPacketQueue` (deep copy + EOF signal) |
| **Pipeline Centralized Error Propagation** | All queues owned by `Pipeline`. Any thread error broadcast via `atomic<bool>`, checked after `join` |
| **In-Encoder Processing Chain** | Rotation & speed change embedded in encoder thread: `pop frame → rotate → speed drop/dup → encode`, zero extra allocation |
| **Min-Heap A/V Interleaving** | Muxer uses `std::priority_queue` for PTS-ordered output. Audio PTS independently recalculated from accumulated sample count |
| **Graceful Shutdown Protocol** | Chain-propagated EOF: Demux flush pkt → Decoder drain → `RingBuffer::flush()` → Encoder drain → `mark_done()` → Muxer write trailer |

### Architecture

See the Chinese section above for the ASCII architecture diagram.

### RAII Resource Wrappers

| Type Alias | Wraps | Custom Deleter |
|------------|-------|----------------|
| `InputFormatContextPtr` | `AVFormatContext*` | `avformat_close_input` |
| `CodecContextPtr` | `AVCodecContext*` | `avcodec_free_context` |
| `FramePtr` | `AVFrame*` | `av_frame_free` |
| `PacketPtr` | `AVPacket*` | `av_packet_free` |
| `CodecParametersPtr` | `AVCodecParameters*` | `avcodec_parameters_free` |

### Queue Implementations

| Queue | Header | Features |
|-------|--------|----------|
| `PacketQueue<T>` | `include/packet_queue.h` | Unbounded `std::queue` + mutex + CV, blocking `pop()` |
| `RingBuffer<T>` | `include/ring_buffer.h` | Fixed 30, pre-allocated, `av_frame_ref` reference passing, blocks producer when full / consumer when empty, `flush()` wakes all |
| `DeepCopyPacketQueue` | `include/deep_copy_packey_queue.h` | `push` deep-copies via `av_packet_clone`, `pop` transfers ownership via `av_packet_move_ref`, `mark_done()` signals EOS |

### Thread Termination Protocol

```
①Demux EOF → send flush packet (data=nullptr)
    │
    ▼
②/③Decoder → receive flush → avcodec_send_packet(nullptr) drain
    │         collect residual frames → RingBuffer::flush()
    ▼
④/⑤Encoder → detect empty ring buffer → send_frame(nullptr) drain
    │         → mark_done()
    ▼
⑥Muxer → wait both done && queues empty → write trailer → exit
```

### A/V Sync

Audio PTS is independently calculated from accumulated sample count (`audio_accumulated_samples / sample_rate`), not trusting encoder output PTS. Video and audio encoded packets are interleaved via a min-heap (`std::priority_queue` + PTS comparator) for correct playback order.

### Codec Choices

| Type | Codec | Format | Bitrate | Notes |
|------|-------|--------|---------|-------|
| Video | **MPEG4** (`AV_CODEC_ID_MPEG4`) | YUV420P | 1 Mbps | GOP=10, B-frames=0, `codec_tag=0x7634706d` |
| Audio | **AC3** (`AV_CODEC_ID_AC3`) | FLTP | 128 kbps | Fixed 1536 samples/frame |
| Container | **MP4** | - | - | - |

### Directory Structure

```
FFmpegProject/
├── main.cpp                        # Entry: CLI parsing, directory scan, thread pool scheduling
├── CMakeLists.txt                  # CMake build config (MinGW)
├── include/
│   ├── config.h                    # ProcessingConfig cross-thread config
│   ├── common.h                    # Common forward declarations
│   ├── thread_pool.h               # Custom fixed-size thread pool
│   ├── pipeline.h                  # Pipeline: owns 6 queues + error propagation
│   ├── ffmpeg_raii.h               # RAII wrappers (5 unique_ptr types)
│   ├── demux.h / mux.h             # Demux / Mux
│   ├── videodecoder.h / audiodecoder.h   # Video / Audio decode
│   ├── videoencoder.h / audioencoder.h   # Video / Audio encode
│   ├── video_rotate.h              # YUV420P pixel rotation
│   ├── packet_queue.h              # Unbounded thread-safe queue
│   ├── ring_buffer.h               # Fixed-size ring buffer
│   └── deep_copy_packey_queue.h    # Deep copy queue + EOF signal
├── src/                            # Module implementations
├── input/                          # Input files directory (scans *.mp4)
├── output/<filename>/              # Output directory (per-file subdir)
├── ffmpeg-4.4-full_build-shared/   # FFmpeg 4.4 shared libraries
└── README.md
```

### Build & Run

**Dependencies:** FFmpeg 4.4 shared libraries, CMake 3.27+, MinGW (Windows)

```bash
cd build
cmake .. -G "MinGW Makefiles"
cmake --build .
```

```bash
# FFmpeg DLLs must be on PATH at runtime
PATH="<ffmpeg>/bin:$PATH" ./FFmpegProject -rotate 90 -speed 1.5 -threads 3
```

### CLI Usage

```
FFmpegProject [-rotate <angle>] [-speed <ratio>] [-threads <n>]
  -rotate <angle>  Video rotation: 90, 180, 270
  -speed  <ratio>  Playback speed: 0.5, 0.75, 1.25, 1.5, 2, 4
  -threads <n>     Thread pool concurrency (default: 3)

Auto-scans ../input/*.mp4, outputs to ../output/<filename>/output.mp4

Examples:
  ./FFmpegProject                                    # Default, single file
  ./FFmpegProject -rotate 90 -speed 1.5              # 90° rotate + 1.5x speed
  ./FFmpegProject -rotate 180 -speed 0.5 -threads 4  # 180° rotate + 0.5x speed + 4 workers
```

### Tech Stack

- **Language:** C++17 (std::thread, std::atomic, std::filesystem, std::priority_queue)
- **Media:** FFmpeg 4.4 C API (libavformat, libavcodec, libavutil)
- **Build:** CMake 3.27 + MinGW
- **Platform:** Windows (portable to Linux)
