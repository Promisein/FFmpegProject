//
// Created by Jianing on 2025/12/22.
//
#include "videodecoder.h"
#include "ring_buffer.h"
#include <iostream>
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
}

void video_decode_thread(AVCodecParameters* codec_par) {
    std::cout << "start videoDecode!\n";
    const AVCodec* codec = avcodec_find_decoder(codec_par->codec_id);
    if (!codec) {
        std::cerr << "[Error] 找不到视频解码器" << std::endl;
        return;
    }

    AVCodecContext* codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) {
        std::cerr << "[Error] 分配视频解码器上下文失败" << std::endl;
        return;
    }
    if (avcodec_parameters_to_context(codec_ctx, codec_par) < 0) {
        std::cerr << "[Error] 复制视频流参数失败" << std::endl;
        avcodec_free_context(&codec_ctx);
        return;
    }

    if (avcodec_open2(codec_ctx, codec, nullptr) < 0) {
        std::cerr << "[Error] 打开视频解码器失败" << std::endl;
        avcodec_free_context(&codec_ctx);
        return;
    }

    AVPacket pkt;
    AVFrame* frame = av_frame_alloc();
    while (g_video_pkt_queue.pop(pkt)) {
        if (!pkt.data) { // 空Packet：解码结束
            avcodec_send_packet(codec_ctx, nullptr);
            break;
        }

        if (avcodec_send_packet(codec_ctx, &pkt) < 0) {
            std::cerr << "[Warn] 视频Packet发送失败" << std::endl;
            av_packet_unref(&pkt);
            continue;
        }

        // 接收解码帧 → 推入环形缓冲区
        while (avcodec_receive_frame(codec_ctx, frame) >= 0) {
            std::cout << "[Video] 解码YUV帧: pts=" << frame->pts << " → 推入环形缓冲区" << std::endl;
            // 推入视频Frame环形缓冲区
            g_video_frame_ringbuf.push(frame);
            av_frame_unref(frame);
        }
        av_packet_unref(&pkt);
    }

    // 解码结束：发送刷新信号给编码线程
    g_video_frame_ringbuf.flush();
    av_frame_free(&frame);
    avcodec_free_context(&codec_ctx);
}