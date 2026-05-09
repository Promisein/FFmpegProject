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
| **音频重采样 (libswresample)** | 自动检测解码器输出格式，`SwrContext` 转换为 AC3 编码器所需的 FLTP，支持任意输入音频格式 |
| **视频像素格式转换 (libswscale)** | 自动检测解码器输出像素格式，`SwsContext` 转换为 MPEG4 编码器所需的 YUV420P，支持任意输入像素格式 |
| **结构化日志系统** | 线程安全 Logger，带时间戳 + 日志级别 + 线程标签，支持控制台 + 文件双输出 |

### 项目架构

```
╔═══════════════════════════════════════════════════════════════════════════════════════╗
║                           FFmpegProject 整体架构 (两层级线程模型)                        ║
╚═══════════════════════════════════════════════════════════════════════════════════════╝

┌──────────────────────────────────────────────────┐     ┌──────────────────────────┐
│                   main.cpp                       │     │   ProcessingConfig        │
│                                                  │     │   ┌─────────────────┐    │
│  ① 解析 CLI 参数                                  │     │   │ rotate: 90|180|270│   │
│     -rotate / -speed / -threads                  │◀────│   │ speed:  0.5~4    │    │
│                                                  │     │   └─────────────────┘    │
│  ② 扫描 ../input/*.mp4 → 生成文件列表              │     └──────────────────────────┘
│                                                  │
│  ③ 创建 ThreadPool(pool_size)                    │
│     │                                            │
│     │  for each file: pool.submit(task)          │
│     │                                            │
│     │  pool.wait_all() → 打印结果汇总              │
│     └────────────────┬───────────────────────────┘
│                      │
│                      │ submit N 个转码任务
│                      ▼
│  ┌───────────────────────────────────────────────────────────────────────────────┐
│  │                         ThreadPool (上层 — 任务级并发)                          │
│  │                                                                               │
│  │  std::vector<thread> workers_        ┌──────────┐                              │
│  │  std::queue<function> tasks_         │ Worker 1 │──▶ transcode_single(A.mp4)  │
│  │  mutex + condition_variable          ├──────────┤                              │
│  │  atomic<size_t> active_count_        │ Worker 2 │──▶ transcode_single(B.mp4)  │
│  │  atomic<size_t> total_submitted_     ├──────────┤                              │
│  │  atomic<size_t> total_completed_     │ Worker 3 │──▶ transcode_single(C.mp4)  │
│  │                                       └──────────┘                              │
│  │  每个 Worker 循环: wait task → execute → notify done_cv_                        │
│  └───────────────────────────────────────────────────────────────────────────────┘
│                      │
│                      │ 每个 Worker 内部启动独立的 6 线程管道
│                      ▼
│  ┌───────────────────────────────────────────────────────────────────────────────┐
│  │              transcode_single() (下层 — 单文件 6 线程转码管线)                    │
│  │                                                                               │
│  │  ◆ 每个调用创建独立的 Pipeline 实例 (所有队列 + 错误传播)                          │
│  │  ◆ 全部 FFmpeg 资源由 RAII 包装, 异常安全                                        │
│  │                                                                               │
│  │  ┌─────────────────────────────────────────────────────────────────────────┐  │
│  │  │                         Pipeline (队列容器 + 错误广播)                      │  │
│  │  │                                                                         │  │
│  │  │  ┌──────────────────┐  ┌──────────────────┐  ┌─────────────────────┐   │  │
│  │  │  │ video_pkt_queue  │  │ audio_pkt_queue  │  │ error_flag_ (atomic)│   │  │
│  │  │  │ PacketQueue      │  │ PacketQueue      │  │ error_msg_ (mutex)  │   │  │
│  │  │  └────────┬─────────┘  └────────┬─────────┘  └─────────────────────┘   │  │
│  │  │           │                     │                                       │  │
│  │  │  ┌────────▼─────────────────────▼──────────┐                            │  │
│  │  │  │ video_frame_ringbuf │ audio_frame_ringbuf│  RingBuffer<AVFrame*>(30) │  │
│  │  │  └────────┬────────────────────┬───────────┘                            │  │
│  │  │           │                    │                                        │  │
│  │  │  ┌────────▼────────────────────▼───────────┐                            │  │
│  │  │  │ en_video_pkt_queue │ en_audio_pkt_queue │  DeepCopyPacketQueue      │  │
│  │  │  └─────────────────────────────────────────┘                            │  │
│  │  └─────────────────────────────────────────────────────────────────────────┘  │
│  │                                                                               │
│  │                        ┌─ 6 线程数据流 ─┐                                       │
│  │                                                                               │
│  │  ┌──────────┐  视频 PacketQueue    ┌──────────────┐                             │
│  │  │ ① Demux  │─────────────────────▶│ ② Video Dec  │──┐                          │
│  │  │          │                      │              │  │ 视频 RingBuffer(30)      │
│  │  │ av_read  │  音频 PacketQueue    └──────────────┘  │                          │
│  │  │ _frame() │──┐                                      │                          │
│  │  └──────────┘  │                ┌──────────────┐      │                          │
│  │                └───────────────▶│ ③ Audio Dec  │──┐   │                          │
│  │                                 │              │  │   │ 音频 RingBuffer(30)      │
│  │                                 └──────────────┘  │   │                          │
│  │                                                    │   │                          │
│  │  ┌─────────────────────────────────────────────────▼───▼──────────────────────┐ │
│  │  │                      ④ Video Encoder (编码线程内处理)                        │ │
│  │  │                                                                           │ │
│  │  │   RingBuffer.pop()                                                        │ │
│  │  │        │                                                                  │ │
│  │  │        ▼                                                                  │ │
│  │  │   ┌──────────────┐                                                        │ │
│  │  │   │ VideoRotate  │  YUV420P 像素级旋转 (90°/180°/270°)                     │ │
│  │  │   │ Processor    │  90°/270° 时交换编码器宽高                               │ │
│  │  │   └──────┬───────┘                                                        │ │
│  │  │          ▼                                                                │ │
│  │  │   ┌──────────────┐                                                        │ │
│  │  │   │ Speed Change │  >1x: 帧丢弃    <1x: 帧复制                             │ │
│  │  │   │ (drop/dup)   │  PTS 连续自增, 无需封装层额外调整                        │ │
│  │  │   └──────┬───────┘                                                        │ │
│  │  │          ▼                                                                │ │
│  │  │   avcodec_send_frame() ──▶ avcodec_receive_packet()                       │ │
│  │  │          │                        │                                       │ │
│  │  │          │           ┌────────────▼──────────────┐                        │ │
│  │  │          │           │ DeepCopyPacketQueue (视频) │                        │ │
│  │  │          │           │ av_packet_clone 深拷贝     │                        │ │
│  │  │          │           └────────────┬──────────────┘                        │ │
│  │  └──────────│────────────────────────│───────────────────────────────────────┘ │
│  │             │                        │                                          │
│  │  ┌──────────▼────────────────────────▼───────────────────────────────────────┐ │
│  │  │                      ⑤ Audio Encoder (编码线程内处理)                       │ │
│  │  │                                                                           │ │
│  │  │   RingBuffer.pop()                                                        │ │
│  │  │        │                                                                  │ │
│  │  │        ▼                                                                  │ │
│  │  │   ┌──────────────┐                                                        │ │
│  │  │   │ Speed Change │  >1x: 帧丢弃    <1x: 帧复制                             │ │
│  │  │   │ (drop/dup)   │  AC3 固定 1536 采样/帧                                  │ │
│  │  │   └──────┬───────┘                                                        │ │
│  │  │          ▼                                                                │ │
│  │  │   avcodec_send_frame() ──▶ avcodec_receive_packet()                       │ │
│  │  │          │                        │                                       │ │
│  │  │          │           ┌────────────▼──────────────┐                        │ │
│  │  │          │           │ DeepCopyPacketQueue (音频) │                        │ │
│  │  │          │           │ av_packet_clone 深拷贝     │                        │ │
│  │  │          │           └────────────┬──────────────┘                        │ │
│  │  └──────────│────────────────────────│───────────────────────────────────────┘ │
│  │             │                        │                                          │
│  │  ┌──────────▼────────────────────────▼───────────────────────────────────────┐ │
│  │  │                           ⑥ Muxer (封装)                                   │ │
│  │  │                                                                           │ │
│  │  │   ┌──────────────────────────────────────────────────────────┐            │ │
│  │  │   │  min-heap (priority_queue + PacketComparator)            │            │ │
│  │  │   │                                                        │            │ │
│  │  │   │  视频 PTS: output_frame_count  (编码器顺序递增)          │            │ │
│  │  │   │  音频 PTS: accumulated_samples / sample_rate (独立计算)  │            │ │
│  │  │   │                                                        │            │ │
│  │  │   │  ┌──┐ ┌──┐ ┌──┐ ┌──┐    按 PTS 弹出交错写入            │            │ │
│  │  │   │  │A │ │V │ │A │ │V │ → av_interleaved_write_frame()    │            │ │
│  │  │   │  └──┘ └──┘ └──┘ └──┘                                    │            │ │
│  │  │   └──────────────────────────────────────────────────────────┘            │ │
│  │  │                                    │                                       │ │
│  │  └────────────────────────────────────│───────────────────────────────────────┘ │
│  │                                       ▼                                          │
│  │                          ../output/<filename>/output.mp4                         │
│  └───────────────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────────────────────┐
│                              线程终止协议 (链式传播 EOF)                                │
│                                                                                     │
│  ①Demux                                        ⑥Mux                                  │
│     │ 发送 flush pkt                               ▲                                 │
│     │ (data=nullptr)                               │ 检查: 两路 done && 队列空        │
│     ▼                                              │ ↓                               │
│  ②/③Decoder                                        │ av_write_trailer()              │
│     │ 收到 flush → send_packet(nullptr)            │ ↓                               │
│     │ 收集编码器残留帧                               │ 关闭输出文件 → return            │
│     │ RingBuffer::flush() ─────────────────────────▶│                                 │
│     ▼                                              │                                 │
│  ④/⑤Encoder                                        │                                 │
│     │ 检测 ring buffer 空 + flush 信号              │                                 │
│     │ send_frame(nullptr) 冲刷编码器                 │                                 │
│     │ DeepCopyPacketQueue::mark_done() ────────────▶│                                 │
│                                                                                     │
└─────────────────────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────────────────────┐
│                              RAII 资源管理与错误传播                                    │
│                                                                                     │
│  ┌────────────────────┐  ┌──────────────────┐  ┌─────────────────────┐              │
│  │ InputFormatContext  │  │ CodecContextPtr  │  │ FramePtr            │              │
│  │ Ptr                 │  │ → avcodec_free_  │  │ → av_frame_free     │              │
│  │ → avformat_close_   │  │   context        │  │                     │              │
│  │   input             │  └──────────────────┘  └─────────────────────┘              │
│  └────────────────────┘                                                             │
│  ┌────────────────────┐  ┌──────────────────────┐  错误路径只需 return;               │
│  │ PacketPtr           │  │ CodecParametersPtr   │  RAII 自动释放所有资源              │
│  │ → av_packet_free    │  │ → avcodec_parameters_│                                     │
│  └────────────────────┘  │   free               │  pipeline.report_error(msg)         │
│                          └──────────────────────┘  → atomic 广播 → join 后统一检查     │
│                                                                                     │
└─────────────────────────────────────────────────────────────────────────────────────┘
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
│   ├── logger.h                    # 结构化日志系统
│   ├── packet_queue.h              # 无界线程安全队列
│   ├── ring_buffer.h               # 定长环形缓冲
│   └── deep_copy_packey_queue.h    # 深拷贝队列 + EOF 信号
├── src/                            # 各模块实现
│   ├── logger.cpp                  # 日志系统实现
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
- **媒体:** FFmpeg 4.4 C API (libavformat, libavcodec, libavutil, libswresample, libswscale)
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
| **Audio Resampling (libswresample)** | Auto-detect decoder output format, `SwrContext` converts to FLTP required by AC3 encoder, supports any input audio format |
| **Video Pixel Conversion (libswscale)** | Auto-detect decoder output pixel format, `SwsContext` converts to YUV420P required by MPEG4 encoder, supports any input pixel format |
| **Structured Logging** | Thread-safe Logger with timestamp + log level + thread tag, dual output to console + file |

### Architecture

```
╔═══════════════════════════════════════════════════════════════════════════════════════╗
║                      FFmpegProject Architecture (Two-Tier Thread Model)                 ║
╚═══════════════════════════════════════════════════════════════════════════════════════╝

┌──────────────────────────────────────────────────┐     ┌──────────────────────────┐
│                   main.cpp                       │     │   ProcessingConfig        │
│                                                  │     │   ┌─────────────────┐    │
│  1. Parse CLI arguments                          │     │   │ rotate: 90|180|270│   │
│     -rotate / -speed / -threads                  │◀────│   │ speed:  0.5~4    │    │
│                                                  │     │   └─────────────────┘    │
│  2. Scan ../input/*.mp4 -> build file list       │     └──────────────────────────┘
│                                                  │
│  3. Create ThreadPool(pool_size)                 │
│     │                                            │
│     │  for each file: pool.submit(task)          │
│     │                                            │
│     │  pool.wait_all() -> print result summary   │
│     └────────────────┬───────────────────────────┘
│                      │
│                      │ submit N transcode tasks
│                      ▼
│  ┌───────────────────────────────────────────────────────────────────────────────┐
│  │                       ThreadPool (Upper — Task-Level Concurrency)               │
│  │                                                                               │
│  │  std::vector<thread> workers_        ┌──────────┐                              │
│  │  std::queue<function> tasks_         │ Worker 1 │──▶ transcode_single(A.mp4)  │
│  │  mutex + condition_variable          ├──────────┤                              │
│  │  atomic<size_t> active_count_        │ Worker 2 │──▶ transcode_single(B.mp4)  │
│  │  atomic<size_t> total_submitted_     ├──────────┤                              │
│  │  atomic<size_t> total_completed_     │ Worker 3 │──▶ transcode_single(C.mp4)  │
│  │                                       └──────────┘                              │
│  │  Worker loop: wait task -> execute -> notify done_cv_                           │
│  └───────────────────────────────────────────────────────────────────────────────┘
│                      │
│                      │ Each Worker internally launches an independent 6-thread pipeline
│                      ▼
│  ┌───────────────────────────────────────────────────────────────────────────────┐
│  │            transcode_single() (Lower — 6-Thread Transcode Pipeline)             │
│  │                                                                               │
│  │  ◆ Each call creates an independent Pipeline instance (all queues + errors)    │
│  │  ◆ All FFmpeg resources managed by RAII, exception-safe                        │
│  │                                                                               │
│  │  ┌─────────────────────────────────────────────────────────────────────────┐  │
│  │  │                   Pipeline (Queue Container + Error Broadcast)            │  │
│  │  │                                                                         │  │
│  │  │  ┌──────────────────┐  ┌──────────────────┐  ┌─────────────────────┐   │  │
│  │  │  │ video_pkt_queue  │  │ audio_pkt_queue  │  │ error_flag_ (atomic)│   │  │
│  │  │  │ PacketQueue      │  │ PacketQueue      │  │ error_msg_ (mutex)  │   │  │
│  │  │  └────────┬─────────┘  └────────┬─────────┘  └─────────────────────┘   │  │
│  │  │           │                     │                                       │  │
│  │  │  ┌────────▼─────────────────────▼──────────┐                            │  │
│  │  │  │ video_frame_ringbuf │ audio_frame_ringbuf│  RingBuffer<AVFrame*>(30) │  │
│  │  │  └────────┬────────────────────┬───────────┘                            │  │
│  │  │           │                    │                                        │  │
│  │  │  ┌────────▼────────────────────▼───────────┐                            │  │
│  │  │  │ en_video_pkt_queue │ en_audio_pkt_queue │  DeepCopyPacketQueue      │  │
│  │  │  └─────────────────────────────────────────┘                            │  │
│  │  └─────────────────────────────────────────────────────────────────────────┘  │
│  │                                                                               │
│  │                        ┌─ 6-Thread Data Flow ─┐                                │
│  │                                                                               │
│  │  ┌──────────┐  Video PacketQueue    ┌──────────────┐                             │
│  │  │ 1. Demux │──────────────────────▶│ 2. Video Dec │──┐                          │
│  │  │          │                      │              │  │ Video RingBuffer(30)     │
│  │  │ av_read  │  Audio PacketQueue   └──────────────┘  │                          │
│  │  │ _frame() │──┐                                      │                          │
│  │  └──────────┘  │                ┌──────────────┐      │                          │
│  │                └───────────────▶│ 3. Audio Dec │──┐   │                          │
│  │                                 │              │  │   │ Audio RingBuffer(30)     │
│  │                                 └──────────────┘  │   │                          │
│  │                                                    │   │                          │
│  │  ┌─────────────────────────────────────────────────▼───▼──────────────────────┐ │
│  │  │                  4. Video Encoder (In-Encoder Processing)                    │ │
│  │  │                                                                           │ │
│  │  │   RingBuffer.pop()                                                        │ │
│  │  │        │                                                                  │ │
│  │  │        ▼                                                                  │ │
│  │  │   ┌──────────────┐                                                        │ │
│  │  │   │ VideoRotate  │  YUV420P pixel rotation (90/180/270)                   │ │
│  │  │   │ Processor    │  90/270 swaps encoder width/height                     │ │
│  │  │   └──────┬───────┘                                                        │ │
│  │  │          ▼                                                                │ │
│  │  │   ┌──────────────┐                                                        │ │
│  │  │   │ Speed Change │  >1x: frame drop    <1x: frame dup                     │ │
│  │  │   │ (drop/dup)   │  Consecutive PTS, no muxer-level adjustment needed     │ │
│  │  │   └──────┬───────┘                                                        │ │
│  │  │          ▼                                                                │ │
│  │  │   avcodec_send_frame() ──▶ avcodec_receive_packet()                       │ │
│  │  │          │                        │                                       │ │
│  │  │          │           ┌────────────▼──────────────┐                        │ │
│  │  │          │           │ DeepCopyPacketQueue (video)│                        │ │
│  │  │          │           │ av_packet_clone deep copy  │                        │ │
│  │  │          │           └────────────┬──────────────┘                        │ │
│  │  └──────────│────────────────────────│───────────────────────────────────────┘ │
│  │             │                        │                                          │
│  │  ┌──────────▼────────────────────────▼───────────────────────────────────────┐ │
│  │  │                  5. Audio Encoder (In-Encoder Processing)                   │ │
│  │  │                                                                           │ │
│  │  │   RingBuffer.pop()                                                        │ │
│  │  │        │                                                                  │ │
│  │  │        ▼                                                                  │ │
│  │  │   ┌──────────────┐                                                        │ │
│  │  │   │ Speed Change │  >1x: frame drop    <1x: frame dup                     │ │
│  │  │   │ (drop/dup)   │  AC3 fixed 1536 samples/frame                          │ │
│  │  │   └──────┬───────┘                                                        │ │
│  │  │          ▼                                                                │ │
│  │  │   avcodec_send_frame() ──▶ avcodec_receive_packet()                       │ │
│  │  │          │                        │                                       │ │
│  │  │          │           ┌────────────▼──────────────┐                        │ │
│  │  │          │           │ DeepCopyPacketQueue (audio)│                        │ │
│  │  │          │           │ av_packet_clone deep copy  │                        │ │
│  │  │          │           └────────────┬──────────────┘                        │ │
│  │  └──────────│────────────────────────│───────────────────────────────────────┘ │
│  │             │                        │                                          │
│  │  ┌──────────▼────────────────────────▼───────────────────────────────────────┐ │
│  │  │                           6. Muxer                                         │ │
│  │  │                                                                           │ │
│  │  │   ┌──────────────────────────────────────────────────────────┐            │ │
│  │  │   │  min-heap (priority_queue + PacketComparator)            │            │ │
│  │  │   │                                                        │            │ │
│  │  │   │  Video PTS: output_frame_count  (encoder sequential)    │            │ │
│  │  │   │  Audio PTS: accumulated_samples / sample_rate            │            │ │
│  │  │   │                                                        │            │ │
│  │  │   │  ┌──┐ ┌──┐ ┌──┐ ┌──┐    pop by PTS, interleaved write  │            │ │
│  │  │   │  │A │ │V │ │A │ │V │ -> av_interleaved_write_frame()   │            │ │
│  │  │   │  └──┘ └──┘ └──┘ └──┘                                    │            │ │
│  │  │   └──────────────────────────────────────────────────────────┘            │ │
│  │  │                                    │                                       │ │
│  │  └────────────────────────────────────│───────────────────────────────────────┘ │
│  │                                       ▼                                          │
│  │                          ../output/<filename>/output.mp4                         │
│  └───────────────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────────────────────┐
│                       Thread Termination Protocol (Chain-Propagated EOF)              │
│                                                                                     │
│  1.Demux                                        6.Mux                                  │
│     │ send flush pkt                                ▲                                 │
│     │ (data=nullptr)                                │ check: both done && queue empty │
│     ▼                                              │ ↓                               │
│  2/3.Decoder                                        │ av_write_trailer()              │
│     │ receive flush -> send_packet(nullptr)         │ ↓                               │
│     │ collect residual frames                       │ close output file -> return      │
│     │ RingBuffer::flush() ─────────────────────────▶│                                 │
│     ▼                                              │                                 │
│  4/5.Encoder                                        │                                 │
│     │ detect empty ring buffer + flush signal       │                                 │
│     │ send_frame(nullptr) drain encoder             │                                 │
│     │ DeepCopyPacketQueue::mark_done() ────────────▶│                                 │
│                                                                                     │
└─────────────────────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────────────────────┐
│                              RAII Resource Management & Error Propagation              │
│                                                                                     │
│  ┌────────────────────┐  ┌──────────────────┐  ┌─────────────────────┐              │
│  │ InputFormatContext  │  │ CodecContextPtr  │  │ FramePtr            │              │
│  │ Ptr                 │  │ -> avcodec_free_  │  │ -> av_frame_free     │              │
│  │ -> avformat_close_  │  │   context        │  │                     │              │
│  │   input             │  └──────────────────┘  └─────────────────────┘              │
│  └────────────────────┘                                                             │
│  ┌────────────────────┐  ┌──────────────────────┐  Error paths: just return;         │
│  │ PacketPtr           │  │ CodecParametersPtr   │  RAII auto-releases all resources  │
│  │ -> av_packet_free   │  │ -> avcodec_parameters_│                                     │
│  └────────────────────┘  │   free               │  pipeline.report_error(msg)         │
│                          └──────────────────────┘  -> atomic broadcast -> check after  │
│                                                                  join                │
└─────────────────────────────────────────────────────────────────────────────────────┘
```

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
│   ├── logger.h                    # Structured logging system
│   ├── packet_queue.h              # Unbounded thread-safe queue
│   ├── ring_buffer.h               # Fixed-size ring buffer
│   └── deep_copy_packey_queue.h    # Deep copy queue + EOF signal
├── src/                            # Module implementations
│   ├── logger.cpp                  # Logger implementation
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
- **Media:** FFmpeg 4.4 C API (libavformat, libavcodec, libavutil, libswresample, libswscale)
- **Build:** CMake 3.27 + MinGW
- **Platform:** Windows (portable to Linux)

---

## 更新日志 / Changelog

### 2026-05-09 — v0.3.0 (P0 基础能力补齐)

| 模块 | 内容 |
|------|------|
| **音频重采样** | `src/audioencoder.cpp` — 插入 `SwrContext`（RAII 封装），自动将解码器输出的任意采样格式/采样率/声道布局转换为 AC3 编码器所需的 FLTP，支持 FLT / S16 / S16P / S32 等常见格式 |
| **视频像素格式转换** | `src/videoencoder.cpp` — 插入 `SwsContext`（RAII 封装），自动将解码器输出的任意像素格式（NV12 / YUVJ420P / YUV422P 等）转换为 MPEG4 编码器所需的 YUV420P |
| **结构化日志系统** | `include/logger.h` + `src/logger.cpp` — 线程安全 Logger 类，`[HH:MM:SS.mmm] [LEVEL] [TAG] msg` 格式，支持 `DEBUG/INFO/WARN/ERROR` 四级，控制台 + 文件双输出。所有模块的 `std::cout`/`std::cerr` 已全部替换 |

### 2026-05-08 — v0.2.0 (批处理 + 线程池)

| 模块 | 内容 |
|------|------|
| **自定义线程池** | `include/thread_pool.h` + `src/thread_pool.cpp` — 手写固定大小线程池，`std::queue` + mutex + CV + atomic 计数器，两层级并发（池内 Worker 执行 transcode_single，每个任务内部 6 线程管道） |
| **批量自动处理** | 自动扫描 `../input/*.mp4`，输出到 `../output/<文件名>/output.mp4`，支持 `-threads <n>` 控制并发数 |

### 2026-05-07 — v0.1.0 (初始架构)

| 模块 | 内容 |
|------|------|
| **6 线程 Pipeline** | Demux → Video/Audio Dec → Video/Audio Enc (旋转+倍速处理) → Mux |
| **Pipeline + RAII** | 消除全局变量，5 种 `std::unique_ptr` 封装 FFmpeg 资源，统一错误传播 |
| **旋转 + 倍速** | `-rotate 90|180|270` YUV420P 像素旋转，`-speed 0.5~4` 帧丢弃/复制倍速 |
| **A/V 同步** | Muxer min-heap PTS 交错 + 音频累计采样独立计算 |
