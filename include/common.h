//
// Created by Jianing on 2025/12/22.
//

#ifndef FFMPEGPROJECT_COMMON_H
#define FFMPEGPROJECT_COMMON_H

// 包含队列类声明
#include "packet_queue.h"
#include "deep_copy_packey_queue.h"
// FFmpeg结构体前置声明（减少头文件依赖）
struct AVFormatContext;
struct AVCodecParameters;
struct AVFrame;

// 全局队列声明（仅声明，定义在common.cpp）
extern PacketQueue<AVPacket> g_video_pkt_queue;
extern PacketQueue<AVPacket> g_audio_pkt_queue;
extern DeepCopyPacketQueue g_en_video_pkt_queue;
extern DeepCopyPacketQueue g_en_audio_pkt_queue;
#endif //FFMPEGPROJECT_COMMON_H
