//
// Created by Jianing on 2025/12/22.
//
#include "ring_buffer.h"
extern "C" {
#include <libavutil/frame.h>
#include <libavcodec/packet.h>
}

// 全局环形缓冲区定义（容量30帧，适配音视频实时性）
RingBuffer<AVFrame*> g_video_frame_ringbuf(30);
RingBuffer<AVFrame*> g_audio_frame_ringbuf(30);

// 编码后Packet环形缓冲区（容量50，适配编码后Packet）
RingBuffer<AVPacket*> g_video_pkt_ringbuf(50);
RingBuffer<AVPacket*> g_audio_pkt_ringbuf(50);
