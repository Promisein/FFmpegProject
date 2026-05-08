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
struct AVRational;
class Pipeline;
#endif //FFMPEGPROJECT_COMMON_H
