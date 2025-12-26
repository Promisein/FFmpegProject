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
#include <libavutil/error.h>
}

void video_encode_thread(AVCodecParameters* src_codec_par, AVRational output_time_base) {
    if (!src_codec_par) {
        std::cerr << "[VideoEncoder Error] 输入编码器参数为空指针！\n";
        return;
    }

    const AVCodec* encoder = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
    if (!encoder) {
        std::cerr << "[VideoEncoder Error] 找不到MPEG4编码器\n";
        return;
    }

    AVCodecContext* enc_ctx = avcodec_alloc_context3(encoder);
    if (!enc_ctx) {
        std::cerr << "[VideoEncoder Error] 分配视频编码器上下文失败\n";
        return;
    }

    // 设置编码参数
    enc_ctx->codec_id = AV_CODEC_ID_MPEG4;
    enc_ctx->width = src_codec_par->width;
    enc_ctx->height = src_codec_par->height;
    enc_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    enc_ctx->time_base = output_time_base;  // 设置为输出时间基1/25
    enc_ctx->framerate = av_inv_q(output_time_base);
    enc_ctx->bit_rate = 1000000;
    enc_ctx->gop_size = 10;
    enc_ctx->max_b_frames = 0;

    // 对于MPEG4，设置正确的codec_tag（mp4v）
    // 0x7634706d = 'mp4v' 的小端表示
    enc_ctx->codec_tag = 0x7634706d;

    int ret = avcodec_open2(enc_ctx, encoder, nullptr);
    if (ret < 0) {
        char err_buf[1024];
        av_strerror(ret, err_buf, sizeof(err_buf));
        std::cerr << "[VideoEncoder Error] 打开MPEG4编码器失败：" << err_buf << "\n";
        avcodec_free_context(&enc_ctx);
        return;
    }

    // 【一次性信息】保留输出
    std::cout << "[VideoEncoder Info] MPEG4编码器打开成功（分辨率："
              << enc_ctx->width << "x" << enc_ctx->height
              << ", codec_tag=0x" << std::hex << enc_ctx->codec_tag << std::dec
              << "）\n";

    AVFrame* local_frame = av_frame_alloc();
    AVPacket* pkt = av_packet_alloc();
    if (!local_frame || !pkt) {
        std::cerr << "[VideoEncoder Error] 分配Frame/Packet失败\n";
        avcodec_free_context(&enc_ctx);
        if (local_frame) av_frame_free(&local_frame);
        if (pkt) av_packet_free(&pkt);
        return;
    }

    int frame_count = 0;

    while (true) {
        // 从环形缓冲区获取一帧数据
        bool success = g_video_frame_ringbuf.pop(local_frame);
        if (!success) {
            // 【退出信息】保留输出
            std::cout << "[VideoEncoder Info] 环形缓冲区已空，停止接收帧\n";
            break;
        }

        frame_count++;

        // 检查帧的有效性
        if (!local_frame->data[0]) {
            std::cerr << "[VideoEncoder Warn] 无效视频Frame（第" << frame_count << "帧），跳过\n";
            av_frame_unref(local_frame);
            continue;
        }

        // 设置时间戳 - 使用简单的递增方式
        local_frame->pts = frame_count - 1;

        // 发送frame到编码器
        ret = avcodec_send_frame(enc_ctx, local_frame);
        if (ret < 0) {
            char err_buf[1024];
            av_strerror(ret, err_buf, sizeof(err_buf));
            std::cerr << "[VideoEncoder Warn] 第" << frame_count << "帧编码发送失败：" << err_buf << "\n";
            av_frame_unref(local_frame);
            continue;
        }

        // 接收编码后的packet
        while (true) {
            ret = avcodec_receive_packet(enc_ctx, pkt);
            if (ret == AVERROR(EAGAIN)) {
                break;
            } else if (ret == AVERROR_EOF) {
                break;
            } else if (ret < 0) {
                char err_buf[1024];
                av_strerror(ret, err_buf, sizeof(err_buf));
                std::cerr << "[VideoEncoder Warn] 接收编码包失败：" << err_buf << "\n";
                break;
            }

            // 设置流索引和时间戳
            pkt->stream_index = 0;
            av_packet_rescale_ts(pkt, enc_ctx->time_base, output_time_base);

            // 关键帧提示：每10帧才输出
            if ((pkt->flags & AV_PKT_FLAG_KEY) && (frame_count % 10 == 0)) {
                std::cout << "[VideoEncoder Info] 关键帧 packet size=" << pkt->size << "\n";
            }

            // 主编码日志：每10帧才输出
            if (frame_count % 10 == 0) {
                std::cout << "[VideoEncoder Info] 编码MPEG4 Packet: pts=" << pkt->pts
                          << " size=" << pkt->size << "（第" << frame_count << "帧）\n";
            }

            // 推送到队列（始终执行）
            g_en_video_pkt_queue.push(*pkt);
            av_packet_unref(pkt);
        }

        av_frame_unref(local_frame);
    }

    // 刷新编码器（一次性信息，保留）
    std::cout << "[VideoEncoder Info] 开始刷新编码器剩余数据（共处理" << frame_count << "帧）\n";
    ret = avcodec_send_frame(enc_ctx, nullptr);
    if (ret < 0) {
        char err_buf[1024];
        av_strerror(ret, err_buf, sizeof(err_buf));
        std::cerr << "[VideoEncoder Warn] 刷新编码器失败：" << err_buf << "\n";
    }

    while (true) {
        ret = avcodec_receive_packet(enc_ctx, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        } else if (ret < 0) {
            char err_buf[1024];
            av_strerror(ret, err_buf, sizeof(err_buf));
            std::cerr << "[VideoEncoder Warn] 刷新时接收编码包失败：" << err_buf << "\n";
            break;
        }

        av_packet_rescale_ts(pkt, enc_ctx->time_base, output_time_base);
        pkt->stream_index = 0;
        g_en_video_pkt_queue.push(*pkt);
        av_packet_unref(pkt);
    }

    // 标记队列结束
    g_en_video_pkt_queue.mark_done();

    // 释放资源
    av_frame_free(&local_frame);
    av_packet_free(&pkt);
    avcodec_free_context(&enc_ctx);

    // 【退出总结】保留输出
    std::cout << "[VideoEncoder Info] 视频编码线程退出，共处理" << frame_count << "帧\n";
}