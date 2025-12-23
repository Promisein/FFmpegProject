//
// Created by Jianing on 2025/12/22.
//

#ifndef FFMPEGPROJECT_VIDEODECODER_H
#define FFMPEGPROJECT_VIDEODECODER_H

#include "common.h"

// 视频解码线程函数声明
void video_decode_thread(AVCodecParameters* codec_par);

#endif //FFMPEGPROJECT_VIDEODECODER_H
