//
// Created by Jianing on 2025/12/22.
//

#ifndef FFMPEGPROJECT_AUDIODECODER_H
#define FFMPEGPROJECT_AUDIODECODER_H

#include "common.h"
#include "ring_buffer.h"

// 音频解码线程函数声明
void audio_decode_thread(AVCodecParameters* codec_par,
                         PacketQueue<AVPacket>& in_queue,
                         RingBuffer<AVFrame*>& out_rb,
                         Pipeline* pipeline);

#endif //FFMPEGPROJECT_AUDIODECODER_H
