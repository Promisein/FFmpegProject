//
// Created by Jianing on 2025/12/22.
//
#include "demux.h"
#include "pipeline.h"
#include "logger.h"
#include <cmath>

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

    // 计算总时长用于进度上报
    double total_duration_sec = 0.0;
    int64_t file_size = 0;
    if (fmt_ctx) {
        if (fmt_ctx->duration > 0)
            total_duration_sec = fmt_ctx->duration / (double)AV_TIME_BASE;
        if (fmt_ctx->pb)
            file_size = avio_size(fmt_ctx->pb);
    }

    int last_progress_pct = -1;

    while (av_read_frame(fmt_ctx, &pkt) >= 0) {
        // FPS counter + progress
        if (pipeline) {
            if (pkt.stream_index == video_stream_idx)
                pipeline->demux_video_packets++;
            else if (pkt.stream_index == audio_stream_idx)
                pipeline->demux_audio_packets++;

            // 进度计算 (0.0 ~ 1.0)
            double progress_val = 0.0;
            AVStream* stream = fmt_ctx->streams[pkt.stream_index];
            if (pkt.pts != AV_NOPTS_VALUE && stream && total_duration_sec > 0) {
                double pkt_sec = pkt.pts * av_q2d(stream->time_base);
                if (pkt_sec > 0 && pkt_sec < total_duration_sec)
                    progress_val = pkt_sec / total_duration_sec;
            }
            if (progress_val <= 0.0 && file_size > 0) {
                progress_val = (double)avio_tell(fmt_ctx->pb) / file_size;
            }
            if (progress_val > 1.0) progress_val = 1.0;
            if (progress_val < 0.0) progress_val = 0.0;
            pipeline->progress.store(progress_val, std::memory_order_relaxed);

            // 每 10% 上报
            int pct = static_cast<int>(progress_val * 100);
            if (pct >= last_progress_pct + 10) {
                last_progress_pct = (pct / 10) * 10;
                Logger::info("Demux", std::string("进度: ") + std::to_string(last_progress_pct) + "%");
            }
        }

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

    // EOF: 100%
    if (pipeline) {
        pipeline->progress.store(1.0, std::memory_order_relaxed);
        Logger::info("Demux", "进度: 100%");
    }

    // 推送空Packet标记结束
    AVPacket flush_pkt = {0};
    flush_pkt.data = nullptr;
    flush_pkt.size = 0;
    video_q.push(flush_pkt);
    audio_q.push(flush_pkt);
}
