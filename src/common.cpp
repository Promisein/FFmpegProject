//
// Created by Jianing on 2025/12/22.
//
#include "common.h"

// 全局队列的唯一定义（避免重复定义）
PacketQueue<AVPacket> g_video_pkt_queue;
PacketQueue<AVPacket> g_audio_pkt_queue;
