//
// Created by Jianing on 2025/12/22.
//

#ifndef FFMPEGPROJECT_AUDIODECODER_H
#define FFMPEGPROJECT_AUDIODECODER_H

#include "common.h"

// 音频解码线程函数声明
void audio_decode_thread(AVCodecParameters* codec_par);

#endif //FFMPEGPROJECT_AUDIODECODER_H
