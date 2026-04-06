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
#include <libavutil/samplefmt.h>
#include <libavutil/error.h>
}

// AC3编码器固定要求：输入PCM帧样本数
#define AC3_REQUIRED_NB_SAMPLES 1536

void audio_encode_thread(AVCodecParameters* src_codec_par, AVRational output_time_base) {
    if (!src_codec_par) {
        std::cerr << "[AudioEncoder Error] 输入编码器参数为空指针！\n";
        return;
    }

    // 1. 查找AC3编码器
    const AVCodec* encoder = avcodec_find_encoder(AV_CODEC_ID_AC3);
    if (!encoder) {
        std::cerr << "[AudioEncoder Error] 找不到AC3编码器\n";
        return;
    }

    // 2. 初始化编码器上下文
    AVCodecContext* enc_ctx = avcodec_alloc_context3(encoder);
    if (!enc_ctx) {
        std::cerr << "[AudioEncoder Error] 分配音频编码器上下文失败\n";
        return;
    }

    // 3. 配置AC3编码参数
    enc_ctx->codec_id = AV_CODEC_ID_AC3;
    enc_ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;       // AC3标准 planar 格式
    enc_ctx->sample_rate = src_codec_par->sample_rate; // 复用原采样率
    enc_ctx->channel_layout = av_get_default_channel_layout(src_codec_par->channels);
    enc_ctx->channels = src_codec_par->channels;    // 复用原声道数
    enc_ctx->bit_rate = 128000;                     // 标准码率
    enc_ctx->time_base = (AVRational){1, enc_ctx->sample_rate}; // 基于采样率的时间基
    enc_ctx->frame_size = AC3_REQUIRED_NB_SAMPLES;  // 强制输入帧样本数

    // 4. 打开编码器
    int ret = avcodec_open2(enc_ctx, encoder, nullptr);
    if (ret < 0) {
        char err_buf[1024];
        av_strerror(ret, err_buf, sizeof(err_buf));
        std::cerr << "[AudioEncoder Error] 打开AC3编码器失败：" << err_buf << "\n";
        avcodec_free_context(&enc_ctx);
        return;
    }

    // 【一次性信息】保留输出
    std::cout << "[AudioEncoder Info] AC3编码器打开成功（采样率："
              << enc_ctx->sample_rate
              << "，声道数：" << enc_ctx->channels
              << "，帧大小：" << enc_ctx->frame_size
              << "）\n";

    // 5. 初始化资源
    AVFrame* input_frame = av_frame_alloc();   // 从环形缓冲区取出的解码后PCM帧
    AVFrame* encode_frame = av_frame_alloc();  // 适配AC3的编码输入帧
    AVPacket* pkt = av_packet_alloc();

    if (!input_frame || !encode_frame || !pkt) {
        std::cerr << "[AudioEncoder Error] 分配Frame/Packet失败\n";
        avcodec_free_context(&enc_ctx);
        if (input_frame) av_frame_free(&input_frame);
        if (encode_frame) av_frame_free(&encode_frame);
        if (pkt) av_packet_free(&pkt);
        return;
    }

    // 配置编码帧缓冲区
    encode_frame->format = enc_ctx->sample_fmt;
    encode_frame->sample_rate = enc_ctx->sample_rate;
    encode_frame->channel_layout = enc_ctx->channel_layout;
    encode_frame->channels = enc_ctx->channels;
    encode_frame->nb_samples = AC3_REQUIRED_NB_SAMPLES;

    ret = av_frame_get_buffer(encode_frame, 0);
    if (ret < 0) {
        char err_buf[1024];
        av_strerror(ret, err_buf, sizeof(err_buf));
        std::cerr << "[AudioEncoder Error] 分配编码帧缓冲区失败：" << err_buf << "\n";
        avcodec_free_context(&enc_ctx);
        av_frame_free(&input_frame);
        av_frame_free(&encode_frame);
        av_packet_free(&pkt);
        return;
    }

    int frame_count = 0;  // 👈 新增帧计数器
    int64_t sample_counter = 0;  // 样本计数器，用于计算PTS

    while (true) {
        // 从环形缓冲区获取一帧数据
        bool success = g_audio_frame_ringbuf.pop(input_frame);
        if (!success) {
            // 【退出信息】保留输出
            std::cout << "[AudioEncoder Info] 环形缓冲区已空，停止接收帧\n";
            break;
        }

        frame_count++;

        
        // 检查帧的有效性
        if (!input_frame->data[0] || input_frame->nb_samples <= 0) {
            std::cerr << "[AudioEncoder Warn] 无效音频Frame（第" << frame_count << "帧），跳过\n";
            av_frame_unref(input_frame);
            continue;
        }

        // 适配样本数：拷贝有效样本，不足填0，多余截断
        int src_nb = input_frame->nb_samples;
        int dst_nb = AC3_REQUIRED_NB_SAMPLES;
        int copy_nb = (src_nb < dst_nb) ? src_nb : dst_nb;
        int bytes_per_sample = av_get_bytes_per_sample(static_cast<AVSampleFormat>(enc_ctx->sample_fmt));

        for (int ch = 0; ch < enc_ctx->channels; ch++) {
            // 拷贝PCM数据（planar格式按声道单独拷贝）
            if (copy_nb > 0) {
                memcpy(encode_frame->data[ch], input_frame->data[ch], copy_nb * bytes_per_sample);
            }
            // 不足部分填充静音（0值）
            if (copy_nb < dst_nb) {
                memset(encode_frame->data[ch] + copy_nb * bytes_per_sample,
                       0, (dst_nb - copy_nb) * bytes_per_sample);
            }
        }

        // 设置PTS
        encode_frame->pts = sample_counter;
        sample_counter += copy_nb;  // 使用实际的样本数，而不是固定的1536

        // 6. 发送frame到编码器
        ret = avcodec_send_frame(enc_ctx, encode_frame);
        if (ret < 0) {
            char err_buf[1024];
            av_strerror(ret, err_buf, sizeof(err_buf));
            std::cerr << "[AudioEncoder Warn] 第" << frame_count << "帧编码发送失败：" << err_buf << "\n";
            av_frame_unref(input_frame);
            continue;
        }

        // 7. 接收编码后的packet
        while (true) {
            ret = avcodec_receive_packet(enc_ctx, pkt);
            if (ret == AVERROR(EAGAIN)) {
                break;
            } else if (ret == AVERROR_EOF) {
                break;
            } else if (ret < 0) {
                char err_buf[1024];
                av_strerror(ret, err_buf, sizeof(err_buf));
                std::cerr << "[AudioEncoder Warn] 接收编码包失败：" << err_buf << "\n";
                break;
            }

            // 设置流索引和时间戳
            pkt->stream_index = 1;  // 假设音频流索引为1（视频为0）
            av_packet_rescale_ts(pkt, enc_ctx->time_base, output_time_base);

            // 主编码日志：每50帧才输出（音频帧通常比视频帧多）
            if (frame_count % 50 == 0) {
                std::cout << "[AudioEncoder Info] 编码AC3 Packet: pts=" << pkt->pts
                          << " size=" << pkt->size << "（第" << frame_count << "帧）\n";
            }

            // 推送到队列（始终执行）
            g_en_audio_pkt_queue.push(*pkt);
            av_packet_unref(pkt);
        }

        av_frame_unref(input_frame);
    }

    // 刷新编码器（一次性信息，保留）
    std::cout << "[AudioEncoder Info] 开始刷新编码器剩余数据（共处理" << frame_count << "帧）\n";
    ret = avcodec_send_frame(enc_ctx, nullptr);
    if (ret < 0) {
        char err_buf[1024];
        av_strerror(ret, err_buf, sizeof(err_buf));
        std::cerr << "[AudioEncoder Warn] 刷新编码器失败：" << err_buf << "\n";
    }

    while (true) {
        ret = avcodec_receive_packet(enc_ctx, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        } else if (ret < 0) {
            char err_buf[1024];
            av_strerror(ret, err_buf, sizeof(err_buf));
            std::cerr << "[AudioEncoder Warn] 刷新时接收编码包失败：" << err_buf << "\n";
            break;
        }

        av_packet_rescale_ts(pkt, enc_ctx->time_base, output_time_base);
        pkt->stream_index = 1;
        g_en_audio_pkt_queue.push(*pkt);
        av_packet_unref(pkt);
    }

    // 标记队列结束
    g_en_audio_pkt_queue.mark_done();

    // 释放资源
    av_frame_free(&input_frame);
    av_frame_free(&encode_frame);
    av_packet_free(&pkt);
    avcodec_free_context(&enc_ctx);

    // 【退出总结】保留输出
    std::cout << "[AudioEncoder Info] 音频编码线程退出，共处理" << frame_count << "帧\n";
}