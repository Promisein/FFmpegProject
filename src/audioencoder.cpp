//
// Created by Jianing on 2025/12/22.
//
#include "audioencoder.h"
#include "pipeline.h"
#include "ffmpeg_raii.h"
#include <iostream>
#include <cstring>
#include <cmath>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libavutil/error.h>
}

#define AC3_REQUIRED_NB_SAMPLES 1536

void audio_encode_thread(AVCodecParameters* src_codec_par, AVRational output_time_base,
                         RingBuffer<AVFrame*>& in_rb, DeepCopyPacketQueue& out_q,
                         const ProcessingConfig& config,
                         Pipeline* pipeline) {
    if (!src_codec_par) {
        if (pipeline) pipeline->report_error("[AudioEncoder] 输入编码器参数为空指针");
        return;
    }

    double speed = config.speed_ratio;
    if (speed != 1.0) {
        std::cout << "[AudioEncoder Info] 倍速: " << speed << "x\n";
    }

    const AVCodec* encoder = avcodec_find_encoder(AV_CODEC_ID_AC3);
    if (!encoder) {
        if (pipeline) pipeline->report_error("[AudioEncoder] 找不到AC3编码器");
        return;
    }

    CodecContextPtr enc_ctx(avcodec_alloc_context3(encoder));
    if (!enc_ctx) {
        if (pipeline) pipeline->report_error("[AudioEncoder] 分配音频编码器上下文失败");
        return;
    }

    enc_ctx->codec_id = AV_CODEC_ID_AC3;
    enc_ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
    enc_ctx->sample_rate = src_codec_par->sample_rate;
    enc_ctx->channel_layout = av_get_default_channel_layout(src_codec_par->channels);
    enc_ctx->channels = src_codec_par->channels;
    enc_ctx->bit_rate = 128000;
    enc_ctx->time_base = (AVRational){1, enc_ctx->sample_rate};
    enc_ctx->frame_size = AC3_REQUIRED_NB_SAMPLES;

    char err_buf[1024];
    int ret = avcodec_open2(enc_ctx.get(), encoder, nullptr);
    if (ret < 0) {
        av_strerror(ret, err_buf, sizeof(err_buf));
        if (pipeline) pipeline->report_error(std::string("[AudioEncoder] 打开AC3编码器失败：") + err_buf);
        return;
    }

    std::cout << "[AudioEncoder Info] AC3编码器打开成功（采样率："
              << enc_ctx->sample_rate << "，声道数：" << enc_ctx->channels
              << "，帧大小：" << enc_ctx->frame_size << "）\n";

    FramePtr input_frame(av_frame_alloc());
    FramePtr encode_frame(av_frame_alloc());
    PacketPtr pkt(av_packet_alloc());

    if (!input_frame || !encode_frame || !pkt) {
        if (pipeline) pipeline->report_error("[AudioEncoder] 分配Frame/Packet失败");
        return;
    }

    encode_frame->format = enc_ctx->sample_fmt;
    encode_frame->sample_rate = enc_ctx->sample_rate;
    encode_frame->channel_layout = enc_ctx->channel_layout;
    encode_frame->channels = enc_ctx->channels;
    encode_frame->nb_samples = AC3_REQUIRED_NB_SAMPLES;

    ret = av_frame_get_buffer(encode_frame.get(), 0);
    if (ret < 0) {
        av_strerror(ret, err_buf, sizeof(err_buf));
        if (pipeline) pipeline->report_error(std::string("[AudioEncoder] 分配编码帧缓冲区失败：") + err_buf);
        return;
    }

    int input_frame_count = 0;   // 从环形缓冲区读取的帧数
    int output_frame_count = 0;  // 发送到编码器的帧数
    int64_t sample_counter = 0;
    int64_t bytes_per_sample = av_get_bytes_per_sample(static_cast<AVSampleFormat>(enc_ctx->sample_fmt));

    while (true) {
        AVFrame* raw_frame = input_frame.get();
        bool success = in_rb.pop(raw_frame);
        if (!success) {
            std::cout << "[AudioEncoder Info] 环形缓冲区已空，停止接收帧\n";
            break;
        }

        input_frame_count++;

        if (!input_frame->data[0] || input_frame->nb_samples <= 0) {
            std::cerr << "[AudioEncoder Warn] 无效音频Frame（第" << input_frame_count << "帧），跳过\n";
            av_frame_unref(input_frame.get());
            continue;
        }

        // ========== 倍速处理: 快放跳帧 ==========
        if (speed > 1.0) {
            int input_idx = input_frame_count - 1;
            double expected_output = input_idx / speed;
            int should_encode = static_cast<int>(expected_output);
            if (should_encode < output_frame_count) {
                av_frame_unref(input_frame.get());
                continue; // 跳过此帧
            }
        }

        int src_nb = input_frame->nb_samples;
        int dst_nb = AC3_REQUIRED_NB_SAMPLES;
        int copy_nb = (src_nb < dst_nb) ? src_nb : dst_nb;

        for (int ch = 0; ch < enc_ctx->channels; ch++) {
            if (copy_nb > 0) {
                memcpy(encode_frame->data[ch], input_frame->data[ch], copy_nb * bytes_per_sample);
            }
            if (copy_nb < dst_nb) {
                memset(encode_frame->data[ch] + copy_nb * bytes_per_sample,
                       0, (dst_nb - copy_nb) * bytes_per_sample);
            }
        }

        encode_frame->pts = sample_counter;
        sample_counter += copy_nb;

        ret = avcodec_send_frame(enc_ctx.get(), encode_frame.get());
        if (ret < 0) {
            av_strerror(ret, err_buf, sizeof(err_buf));
            std::cerr << "[AudioEncoder Warn] 第" << output_frame_count << "帧编码发送失败：" << err_buf << "\n";
            av_frame_unref(input_frame.get());
            continue;
        }

        output_frame_count++;

        while (true) {
            ret = avcodec_receive_packet(enc_ctx.get(), pkt.get());
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) {
                av_strerror(ret, err_buf, sizeof(err_buf));
                std::cerr << "[AudioEncoder Warn] 接收编码包失败：" << err_buf << "\n";
                break;
            }

            pkt->stream_index = 1;


            if (output_frame_count % 50 == 0) {
                std::cout << "[AudioEncoder Info] 编码AC3 Packet: pts=" << pkt->pts
                          << " size=" << pkt->size << "（第" << output_frame_count << "帧）\n";
            }

            out_q.push(*pkt);
            av_packet_unref(pkt.get());
        }

        // ========== 慢放: 复制音频帧 ==========
        if (speed < 1.0) {
            double repeats = 1.0 / speed;
            int repeat_count = static_cast<int>(std::round(repeats));
            if (repeat_count < 1) repeat_count = 1;

            for (int r = 1; r < repeat_count; r++) {
                encode_frame->pts = sample_counter;
                sample_counter += copy_nb;

                ret = avcodec_send_frame(enc_ctx.get(), encode_frame.get());
                if (ret < 0) continue;

                output_frame_count++;

                while (true) {
                    ret = avcodec_receive_packet(enc_ctx.get(), pkt.get());
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                    if (ret < 0) break;

                    pkt->stream_index = 1;
        
                    out_q.push(*pkt);
                    av_packet_unref(pkt.get());
                }
            }
        }

        av_frame_unref(input_frame.get());
    }

    std::cout << "[AudioEncoder Info] 开始刷新编码器剩余数据（输入" << input_frame_count
              << "帧，输出" << output_frame_count << "帧）\n";
    ret = avcodec_send_frame(enc_ctx.get(), nullptr);
    if (ret < 0) {
        av_strerror(ret, err_buf, sizeof(err_buf));
        std::cerr << "[AudioEncoder Warn] 刷新编码器失败：" << err_buf << "\n";
    }

    while (true) {
        ret = avcodec_receive_packet(enc_ctx.get(), pkt.get());
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) {
            av_strerror(ret, err_buf, sizeof(err_buf));
            std::cerr << "[AudioEncoder Warn] 刷新时接收编码包失败：" << err_buf << "\n";
            break;
        }

        av_packet_rescale_ts(pkt.get(), enc_ctx->time_base, output_time_base);
        pkt->stream_index = 1;
        out_q.push(*pkt);
        av_packet_unref(pkt.get());
    }

    out_q.mark_done();
    std::cout << "[AudioEncoder Info] 音频编码线程退出，输入" << input_frame_count
              << "帧，输出" << output_frame_count << "帧\n";
}
