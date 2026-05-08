//
// Created by Jianing on 2025/12/22.
//

#ifndef FFMPEGPROJECT_DEMUX_H
#define FFMPEGPROJECT_DEMUX_H

#include "common.h"

// 解封装线程函数声明
void demux_thread(AVFormatContext* fmt_ctx, int video_stream_idx, int audio_stream_idx,
                  PacketQueue<AVPacket>& video_q, PacketQueue<AVPacket>& audio_q,
                  Pipeline* pipeline);

#endif //FFMPEGPROJECT_DEMUX_H
