//
// Created by Jianing on 2025/12/22.
//
#include "audioencoder.h"
#include <iostream>
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
}

void audio_encode_thread(AVCodecParameters* src_codec_par, AVRational output_time_base) {
    // 1. 查找AC3编码器
    const AVCodec* encoder = avcodec_find_encoder(AV_CODEC_ID_AC3);
    if (!encoder) {
        std::cerr << "[Error] 找不到AC3编码器" << std::endl;
        return;
    }

    // 2. 初始化编码器上下文
    AVCodecContext* enc_ctx = avcodec_alloc_context3(encoder);
    if (!enc_ctx) {
        std::cerr << "[Error] 分配音频编码器上下文失败" << std::endl;
        return;
    }

    // 3. 设置AC3编码参数
    enc_ctx->codec_id = AV_CODEC_ID_AC3;
    enc_ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;       // AC3标准采样格式
    enc_ctx->sample_rate = src_codec_par->sample_rate; // 原音频采样率
    enc_ctx->channel_layout = av_get_default_channel_layout(src_codec_par->channels);
    enc_ctx->channels = src_codec_par->channels;    // 原音频声道数
    enc_ctx->bit_rate = 128000;                     // 码率128Kbps
    enc_ctx->time_base = output_time_base;          // 输出时间基

    // 4. 打开编码器
    if (avcodec_open2(enc_ctx, encoder, nullptr) < 0) {
        std::cerr << "[Error] 打开AC3编码器失败" << std::endl;
        avcodec_free_context(&enc_ctx);
        return;
    }

    // 5. 从环形缓冲区取Frame编码
    AVFrame* frame = av_frame_alloc();
    AVPacket* pkt = av_packet_alloc();
    while (g_audio_frame_ringbuf.pop(frame)) {
        // 设置Frame的编码参数
        frame->format = enc_ctx->sample_fmt;
        frame->sample_rate = enc_ctx->sample_rate;
        frame->channel_layout = enc_ctx->channel_layout;
        frame->channels = enc_ctx->channels;
//        frame->time_base = enc_ctx->time_base;

        // 发送Frame到编码器
        if (avcodec_send_frame(enc_ctx, frame) < 0) {
            std::cerr << "[Warn] 音频Frame编码发送失败" << std::endl;
            av_frame_unref(frame);
            continue;
        }

        // 接收编码后的Packet
        while (avcodec_receive_packet(enc_ctx, pkt) >= 0) {
            std::cout << "[AudioEncoder] 编码AC3 Packet: pts=" << pkt->pts << std::endl;
            // 转换时间基（匹配复用器）
            av_packet_rescale_ts(pkt, enc_ctx->time_base, output_time_base);
            pkt->stream_index = 1; // 音频流索引（复用器中固定为1）
            // 推入编码后Packet环形缓冲区
            g_audio_pkt_ringbuf.push(pkt);
            av_packet_unref(pkt);
        }
        av_frame_unref(frame);
    }

    // 6. 刷新编码器剩余数据
    avcodec_send_frame(enc_ctx, nullptr);
    while (avcodec_receive_packet(enc_ctx, pkt) >= 0) {
        av_packet_rescale_ts(pkt, enc_ctx->time_base, output_time_base);
        pkt->stream_index = 1;
        g_audio_pkt_ringbuf.push(pkt);
        av_packet_unref(pkt);
    }

    // 7. 编码结束：发送刷新信号给复用线程
    g_audio_pkt_ringbuf.flush();
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&enc_ctx);
}
