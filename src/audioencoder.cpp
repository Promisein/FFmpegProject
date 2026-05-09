//
// Created by Jianing on 2025/12/22.
//
#include "audioencoder.h"
#include "pipeline.h"
#include "ffmpeg_raii.h"
#include "logger.h"
#include <cstring>
#include <cmath>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libavutil/error.h>
#include <libswresample/swresample.h>
}

#define AC3_REQUIRED_NB_SAMPLES 1536

// SwrContext RAII 包装
struct SwrContextDeleter {
    void operator()(SwrContext* ctx) const {
        if (ctx) swr_free(&ctx);
    }
};
using SwrContextPtr = std::unique_ptr<SwrContext, SwrContextDeleter>;

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
        Logger::info("AudioEncoder", std::string("倍速: ") + std::to_string(speed) + "x");
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

    Logger::info("AudioEncoder", std::string("AC3编码器打开成功（采样率：")
                 + std::to_string(enc_ctx->sample_rate)
                 + "，声道数：" + std::to_string(enc_ctx->channels)
                 + "，帧大小：" + std::to_string(enc_ctx->frame_size) + "）");

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

    // 重采样上下文（懒初始化）
    SwrContextPtr swr_ctx;
    bool need_resample = false;
    FramePtr resampled_frame(av_frame_alloc());
    int bytes_per_sample = av_get_bytes_per_sample(static_cast<AVSampleFormat>(enc_ctx->sample_fmt));

    int input_frame_count = 0;
    int output_frame_count = 0;
    int64_t sample_counter = 0;

    while (true) {
        AVFrame* raw_frame = input_frame.get();
        bool success = in_rb.pop(raw_frame);
        if (!success) {
            Logger::info("AudioEncoder", "环形缓冲区已空，停止接收帧");
            break;
        }

        input_frame_count++;

        if (!input_frame->data[0] || input_frame->nb_samples <= 0) {
            Logger::warn("AudioEncoder", std::string("无效音频Frame（第")
                         + std::to_string(input_frame_count) + "帧），跳过");
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
                continue;
            }
        }

        // ========== 音频重采样: 解码格式 → FLTP ==========
        AVFrame* source_frame = input_frame.get();
        bool src_is_fltp = (input_frame->format == AV_SAMPLE_FMT_FLTP);
        bool src_matches_layout = (input_frame->channel_layout == enc_ctx->channel_layout
                                   || input_frame->channels == enc_ctx->channels);
        bool src_matches_rate = (input_frame->sample_rate == enc_ctx->sample_rate);
        need_resample = !src_is_fltp || !src_matches_layout || !src_matches_rate;

        if (need_resample) {
            if (!swr_ctx) {
                int64_t in_ch_layout = input_frame->channel_layout;
                if (in_ch_layout == 0) {
                    in_ch_layout = av_get_default_channel_layout(input_frame->channels);
                }
                swr_ctx.reset(swr_alloc_set_opts(nullptr,
                    enc_ctx->channel_layout, enc_ctx->sample_fmt, enc_ctx->sample_rate,
                    in_ch_layout, static_cast<AVSampleFormat>(input_frame->format), input_frame->sample_rate,
                    0, nullptr));
                if (!swr_ctx) {
                    Logger::error("AudioEncoder", "创建SwrContext失败");
                    av_frame_unref(input_frame.get());
                    continue;
                }
                ret = swr_init(swr_ctx.get());
                if (ret < 0) {
                    av_strerror(ret, err_buf, sizeof(err_buf));
                    Logger::error("AudioEncoder", std::string("初始化SwrContext失败：") + err_buf);
                    av_frame_unref(input_frame.get());
                    continue;
                }
                Logger::info("AudioEncoder",
                    std::string("音频重采样已启用: ") + av_get_sample_fmt_name(static_cast<AVSampleFormat>(input_frame->format))
                    + " → FLTP, " + std::to_string(input_frame->sample_rate) + "Hz"
                    + ", ch_layout=" + std::to_string(in_ch_layout));
            }

            // 计算输出采样数
            int64_t delay = swr_get_delay(swr_ctx.get(), input_frame->sample_rate);
            int dst_nb = static_cast<int>(av_rescale_rnd(
                delay + input_frame->nb_samples,
                enc_ctx->sample_rate, input_frame->sample_rate, AV_ROUND_UP));

            av_frame_unref(resampled_frame.get());
            resampled_frame->format = enc_ctx->sample_fmt;
            resampled_frame->sample_rate = enc_ctx->sample_rate;
            resampled_frame->channel_layout = enc_ctx->channel_layout;
            resampled_frame->channels = enc_ctx->channels;
            resampled_frame->nb_samples = dst_nb;

            ret = av_frame_get_buffer(resampled_frame.get(), 0);
            if (ret < 0) {
                Logger::warn("AudioEncoder", "分配重采样帧缓冲区失败");
                av_frame_unref(input_frame.get());
                continue;
            }

            ret = swr_convert_frame(swr_ctx.get(), resampled_frame.get(), input_frame.get());
            if (ret < 0) {
                av_strerror(ret, err_buf, sizeof(err_buf));
                Logger::warn("AudioEncoder", std::string("音频重采样失败：") + err_buf);
                av_frame_unref(input_frame.get());
                continue;
            }
            source_frame = resampled_frame.get();
        }

        int src_nb = source_frame->nb_samples;
        int dst_nb = AC3_REQUIRED_NB_SAMPLES;
        int copy_nb = (src_nb < dst_nb) ? src_nb : dst_nb;

        // 如果重采样后 bytes_per_sample 可能改变，重新获取
        int actual_bps = av_get_bytes_per_sample(static_cast<AVSampleFormat>(enc_ctx->sample_fmt));

        for (int ch = 0; ch < enc_ctx->channels && ch < source_frame->channels; ch++) {
            if (copy_nb > 0) {
                memcpy(encode_frame->data[ch], source_frame->data[ch], copy_nb * actual_bps);
            }
            if (copy_nb < dst_nb) {
                memset(encode_frame->data[ch] + copy_nb * actual_bps,
                       0, (dst_nb - copy_nb) * actual_bps);
            }
        }

        encode_frame->pts = sample_counter;
        sample_counter += copy_nb;

        ret = avcodec_send_frame(enc_ctx.get(), encode_frame.get());
        if (ret < 0) {
            av_strerror(ret, err_buf, sizeof(err_buf));
            Logger::warn("AudioEncoder", std::string("第") + std::to_string(output_frame_count)
                         + "帧编码发送失败：" + err_buf);
            av_frame_unref(input_frame.get());
            continue;
        }

        output_frame_count++;

        while (true) {
            ret = avcodec_receive_packet(enc_ctx.get(), pkt.get());
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) {
                av_strerror(ret, err_buf, sizeof(err_buf));
                Logger::warn("AudioEncoder", std::string("接收编码包失败：") + err_buf);
                break;
            }

            pkt->stream_index = 1;

            if (output_frame_count % 50 == 0) {
                Logger::debug("AudioEncoder", std::string("编码AC3 Packet: pts=")
                              + std::to_string(pkt->pts) + " size=" + std::to_string(pkt->size)
                              + "（第" + std::to_string(output_frame_count) + "帧）");
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

    Logger::info("AudioEncoder", std::string("开始刷新编码器剩余数据（输入")
                 + std::to_string(input_frame_count) + "帧，输出"
                 + std::to_string(output_frame_count) + "帧）");
    ret = avcodec_send_frame(enc_ctx.get(), nullptr);
    if (ret < 0) {
        av_strerror(ret, err_buf, sizeof(err_buf));
        Logger::warn("AudioEncoder", std::string("刷新编码器失败：") + err_buf);
    }

    while (true) {
        ret = avcodec_receive_packet(enc_ctx.get(), pkt.get());
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) {
            av_strerror(ret, err_buf, sizeof(err_buf));
            Logger::warn("AudioEncoder", std::string("刷新时接收编码包失败：") + err_buf);
            break;
        }

        av_packet_rescale_ts(pkt.get(), enc_ctx->time_base, output_time_base);
        pkt->stream_index = 1;
        out_q.push(*pkt);
        av_packet_unref(pkt.get());
    }

    out_q.mark_done();
    Logger::info("AudioEncoder", std::string("音频编码线程退出，输入")
                 + std::to_string(input_frame_count) + "帧，输出"
                 + std::to_string(output_frame_count) + "帧");
}
