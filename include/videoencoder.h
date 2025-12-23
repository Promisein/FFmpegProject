//
// Created by Jianing on 2025/12/22.
//

#ifndef FFMPEGPROJECT_VIDEOENCODER_H
#define FFMPEGPROJECT_VIDEOENCODER_H
#include "ring_buffer.h"
struct AVCodecParameters;

// 视频编码线程（入参：原视频流参数、输出时间基）
void video_encode_thread(AVCodecParameters* src_codec_par, AVRational output_time_base);


#endif //FFMPEGPROJECT_VIDEOENCODER_H
