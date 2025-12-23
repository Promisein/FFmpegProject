//
// Created by Jianing on 2025/12/22.
//

#ifndef FFMPEGPROJECT_AUDIOENCODER_H
#define FFMPEGPROJECT_AUDIOENCODER_H

#include "ring_buffer.h"
struct AVCodecParameters;

// 音频编码线程（入参：原音频流参数、输出时间基）
void audio_encode_thread(AVCodecParameters* src_codec_par, AVRational output_time_base);


#endif //FFMPEGPROJECT_AUDIOENCODER_H
