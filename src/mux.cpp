//
// Created by Jianing on 2025/12/22.
//
#include "mux.h"
#include "ring_buffer.h"
#include <iostream>
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
}

void mux_thread(const std::string& output_file,
                AVCodecParameters* video_enc_par,
                AVCodecParameters* audio_enc_par) {
    // 1. 创建输出格式上下文
    AVFormatContext* out_fmt_ctx = nullptr;
    if (avformat_alloc_output_context2(&out_fmt_ctx, nullptr, nullptr, output_file.c_str()) < 0) {
        std::cerr << "[Error] 创建输出格式上下文失败" << std::endl;
        return;
    }

    // 2. 添加视频流
    AVStream* video_stream = avformat_new_stream(out_fmt_ctx, nullptr);
    if (!video_stream) {
        std::cerr << "[Error] 创建视频流失败" << std::endl;
        avformat_free_context(out_fmt_ctx);
        return;
    }
    if (avcodec_parameters_copy(video_stream->codecpar, video_enc_par) < 0) {
        std::cerr << "[Error] 复制视频编码参数失败" << std::endl;
        avformat_free_context(out_fmt_ctx);
        return;
    }
    video_stream->time_base = out_fmt_ctx->streams[0]->time_base;

    // 3. 添加音频流
    AVStream* audio_stream = avformat_new_stream(out_fmt_ctx, nullptr);
    if (!audio_stream) {
        std::cerr << "[Error] 创建音频流失败" << std::endl;
        avformat_free_context(out_fmt_ctx);
        return;
    }
    if (avcodec_parameters_copy(audio_stream->codecpar, audio_enc_par) < 0) {
        std::cerr << "[Error] 复制音频编码参数失败" << std::endl;
        avformat_free_context(out_fmt_ctx);
        return;
    }
    audio_stream->time_base = out_fmt_ctx->streams[1]->time_base;

    // 4. 打开输出文件
    if (!(out_fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&out_fmt_ctx->pb, output_file.c_str(), AVIO_FLAG_WRITE) < 0) {
            std::cerr << "[Error] 打开输出文件失败: " << output_file << std::endl;
            avformat_free_context(out_fmt_ctx);
            return;
        }
    }

    // 5. 写入文件头
    if (avformat_write_header(out_fmt_ctx, nullptr) < 0) {
        std::cerr << "[Error] 写入文件头失败" << std::endl;
        avio_closep(&out_fmt_ctx->pb);
        avformat_free_context(out_fmt_ctx);
        return;
    }

    // 6. 循环读取编码后的Packet，写入文件
    AVPacket* video_pkt = av_packet_alloc();
    AVPacket* audio_pkt = av_packet_alloc();
    bool video_done = false, audio_done = false;

    while (!video_done || !audio_done) {
        // 读取视频Packet
        if (!video_done) {
            if (g_video_pkt_ringbuf.pop(video_pkt)) {
                // 写入视频Packet
                if (av_interleaved_write_frame(out_fmt_ctx, video_pkt) < 0) {
                    std::cerr << "[Warn] 写入视频Packet失败" << std::endl;
                }
                av_packet_unref(video_pkt);
            } else {
                video_done = true;
            }
        }

        // 读取音频Packet
        if (!audio_done) {
            if (g_audio_pkt_ringbuf.pop(audio_pkt)) {
                // 写入音频Packet
                if (av_interleaved_write_frame(out_fmt_ctx, audio_pkt) < 0) {
                    std::cerr << "[Warn] 写入音频Packet失败" << std::endl;
                }
                av_packet_unref(audio_pkt);
            } else {
                audio_done = true;
            }
        }
    }

    // 7. 写入文件尾
    av_write_trailer(out_fmt_ctx);

    // 8. 释放资源
    if (!(out_fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        avio_closep(&out_fmt_ctx->pb);
    }
    av_packet_free(&video_pkt);
    av_packet_free(&audio_pkt);
    avformat_free_context(out_fmt_ctx);

    std::cout << "[Mux] 转码完成！输出文件: " << output_file << std::endl;
}
