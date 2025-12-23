//
// Created by Jianing on 2025/12/22.
//
#include "demux.h"
#include <iostream>
using namespace std;
// 引入FFmpeg头文件（仅实现文件包含）
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
}

// 解封装线程实现
void demux_thread(AVFormatContext* fmt_ctx, int video_stream_idx, int audio_stream_idx) {
    AVPacket pkt;
    cout << "start demux!" << endl;
    // 循环读取媒体包
    while (av_read_frame(fmt_ctx, &pkt) >= 0) {
        if (pkt.stream_index == video_stream_idx) {
            AVPacket video_pkt;
            av_packet_ref(&video_pkt, &pkt);
            g_video_pkt_queue.push(video_pkt);
        } else if (pkt.stream_index == audio_stream_idx) {
            AVPacket audio_pkt;
            av_packet_ref(&audio_pkt, &pkt);
            g_audio_pkt_queue.push(audio_pkt);
        }
        av_packet_unref(&pkt);
    }

    // 推送空Packet标记结束（替换废弃的av_init_packet）
    AVPacket flush_pkt = {0};
    flush_pkt.data = nullptr;
    flush_pkt.size = 0;
    g_video_pkt_queue.push(flush_pkt);
    g_audio_pkt_queue.push(flush_pkt);
    cout << "demux over!" << endl;
}
