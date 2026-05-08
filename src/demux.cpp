//
// Created by Jianing on 2025/12/22.
//
#include "demux.h"
#include "pipeline.h"
#include <iostream>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
}

// 解封装线程实现
void demux_thread(AVFormatContext* fmt_ctx, int video_stream_idx, int audio_stream_idx,
                  PacketQueue<AVPacket>& video_q, PacketQueue<AVPacket>& audio_q,
                  Pipeline* pipeline) {
    AVPacket pkt;

    while (av_read_frame(fmt_ctx, &pkt) >= 0) {
        if (pkt.stream_index == video_stream_idx) {
            AVPacket video_pkt;
            av_packet_ref(&video_pkt, &pkt);
            video_q.push(video_pkt);
        } else if (pkt.stream_index == audio_stream_idx) {
            AVPacket audio_pkt;
            av_packet_ref(&audio_pkt, &pkt);
            audio_q.push(audio_pkt);
        }
        av_packet_unref(&pkt);
    }

    // 推送空Packet标记结束
    AVPacket flush_pkt = {0};
    flush_pkt.data = nullptr;
    flush_pkt.size = 0;
    video_q.push(flush_pkt);
    audio_q.push(flush_pkt);
}
