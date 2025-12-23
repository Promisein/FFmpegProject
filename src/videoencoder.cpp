//
// Created by Jianing on 2025/12/22.
//
#include "videoencoder.h"
#include <iostream>
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
}

void video_encode_thread(AVCodecParameters* src_codec_par, AVRational output_time_base) {
    // 1. 查找MPEG4编码器
    const AVCodec* encoder = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
    if (!encoder) {
        std::cerr << "[Error] 找不到MPEG4编码器" << std::endl;
        return;
    }

    // 2. 初始化编码器上下文
    AVCodecContext* enc_ctx = avcodec_alloc_context3(encoder);
    if (!enc_ctx) {
        std::cerr << "[Error] 分配视频编码器上下文失败" << std::endl;
        return;
    }

    // 3. 设置MPEG4编码参数
    enc_ctx->codec_id = AV_CODEC_ID_MPEG4;
    enc_ctx->width = src_codec_par->width;          // 原视频宽度
    enc_ctx->height = src_codec_par->height;        // 原视频高度
    enc_ctx->pix_fmt = AV_PIX_FMT_YUV420P;          // MPEG4标准YUV格式
    enc_ctx->time_base = output_time_base;          // 输出时间基（与复用器一致）
    enc_ctx->framerate = av_inv_q(output_time_base); // 帧率
    enc_ctx->bit_rate = 1000000;                    // 码率1Mbps
    enc_ctx->gop_size = 10;                         // 关键帧间隔
    enc_ctx->max_b_frames = 1;                      // B帧数量

    // 4. 打开编码器
    if (avcodec_open2(enc_ctx, encoder, nullptr) < 0) {
        std::cerr << "[Error] 打开MPEG4编码器失败" << std::endl;
        avcodec_free_context(&enc_ctx);
        return;
    }

    // 5. 从环形缓冲区取Frame编码
    AVFrame* frame = av_frame_alloc();
    AVPacket* pkt = av_packet_alloc();
    while (g_video_frame_ringbuf.pop(frame)) {
        // 设置Frame的编码参数
        frame->format = enc_ctx->pix_fmt;
        frame->width = enc_ctx->width;
        frame->height = enc_ctx->height;
//        frame->time_base = enc_ctx->time_base;

        // 发送Frame到编码器
        if (avcodec_send_frame(enc_ctx, frame) < 0) {
            std::cerr << "[Warn] 视频Frame编码发送失败" << std::endl;
            av_frame_unref(frame);
            continue;
        }

        // 接收编码后的Packet
        while (avcodec_receive_packet(enc_ctx, pkt) >= 0) {
            std::cout << "[VideoEncoder] 编码MPEG4 Packet: pts=" << pkt->pts << std::endl;
            // 转换时间基（匹配复用器）
            av_packet_rescale_ts(pkt, enc_ctx->time_base, output_time_base);
            pkt->stream_index = 0; // 视频流索引（复用器中固定为0）
            // 推入编码后Packet环形缓冲区
            g_video_pkt_ringbuf.push(pkt);
            av_packet_unref(pkt);
        }
        av_frame_unref(frame);
    }

    // 6. 刷新编码器剩余数据
    avcodec_send_frame(enc_ctx, nullptr);
    while (avcodec_receive_packet(enc_ctx, pkt) >= 0) {
        av_packet_rescale_ts(pkt, enc_ctx->time_base, output_time_base);
        pkt->stream_index = 0;
        g_video_pkt_ringbuf.push(pkt);
        av_packet_unref(pkt);
    }

    // 7. 编码结束：发送刷新信号给复用线程
    g_video_pkt_ringbuf.flush();
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&enc_ctx);
}

