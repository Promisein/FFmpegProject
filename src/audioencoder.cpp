#include "audioencoder.h"
#include <iostream>
#include <cstring>
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
}

// AC3编码器固定要求：输入PCM帧样本数（48kHz→1536，44.1kHz→1411，32kHz→1024，根据实际采样率调整）
#define AC3_REQUIRED_NB_SAMPLES 1536

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

    // 3. 配置AC3编码参数（严格匹配AC3要求）
    enc_ctx->codec_id = AV_CODEC_ID_AC3;
    enc_ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;       // AC3标准 planar 格式
    enc_ctx->sample_rate = src_codec_par->sample_rate; // 复用原采样率（需为AC3兼容值：32k/44.1k/48k）
    enc_ctx->channel_layout = av_get_default_channel_layout(src_codec_par->channels);
    enc_ctx->channels = src_codec_par->channels;    // 复用原声道数（最多5.1）
    enc_ctx->bit_rate = 128000;                     // 标准码率
    enc_ctx->time_base = output_time_base;          // 输出时间基（匹配复用器）
    enc_ctx->frame_size = AC3_REQUIRED_NB_SAMPLES;  // 强制输入帧样本数（核心）

    // 4. 打开编码器
    int ret = avcodec_open2(enc_ctx, encoder, nullptr);
    if (ret < 0) {
        char err_buf[1024];
        av_strerror(ret, err_buf, sizeof(err_buf));
        std::cerr << "[Error] 打开AC3编码器失败：" << err_buf << std::endl;
        avcodec_free_context(&enc_ctx);
        return;
    }
    std::cout << "[AudioEncoder Info] AC3编码器打开成功（采样率：" << enc_ctx->sample_rate << "，样本数要求：" << AC3_REQUIRED_NB_SAMPLES << "）" << std::endl;

    // 5. 初始化资源
    AVFrame* input_frame = av_frame_alloc();  // 从环形缓冲区取出的解码后PCM帧
    AVFrame* encode_frame = av_frame_alloc(); // 适配AC3的编码输入帧
    AVPacket* pkt = av_packet_alloc();
    if (!input_frame || !encode_frame || !pkt) {
        std::cerr << "[Error] 分配编码资源失败" << std::endl;
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
        std::cerr << "[Error] 分配编码帧缓冲区失败：" << err_buf << std::endl;
        return;
    }

    // 6. 取帧→适配→编码
    while (g_audio_frame_ringbuf.pop(input_frame)) {
        if (!input_frame->data[0] || input_frame->nb_samples <= 0) {
            std::cerr << "[Warn] 无效PCM帧，跳过" << std::endl;
            av_frame_unref(input_frame);
            continue;
        }

        // 适配样本数：拷贝有效样本，不足填0，多余截断
        int src_nb = input_frame->nb_samples;
        int dst_nb = AC3_REQUIRED_NB_SAMPLES;
        int copy_nb = std::min(src_nb, dst_nb);
        for (int ch = 0; ch < enc_ctx->channels; ch++) {
            // 拷贝PCM数据（planar格式按声道单独拷贝）
            memcpy(encode_frame->data[ch], input_frame->data[ch], copy_nb * av_get_bytes_per_sample(enc_ctx->sample_fmt));
            // 不足部分填充静音（0值）
            if (copy_nb < dst_nb) {
                memset(encode_frame->data[ch] + copy_nb * av_get_bytes_per_sample(enc_ctx->sample_fmt),
                       0, (dst_nb - copy_nb) * av_get_bytes_per_sample(enc_ctx->sample_fmt));
            }
        }

        // 关键修复：PTS继承+时间基适配（移除input_frame->time_base引用）
        encode_frame->pts = input_frame->pts;  // 直接继承解码帧的PTS
        // 无需额外转换：解码帧的PTS已适配流时间基，编码器会自动按enc_ctx->time_base处理

        // 7. 发送帧到编码器
        ret = avcodec_send_frame(enc_ctx, encode_frame);
        if (ret < 0) {
            char err_buf[1024];
            av_strerror(ret, err_buf, sizeof(err_buf));
            std::cerr << "[Warn] 编码发送失败：" << err_buf << "（PTS：" << encode_frame->pts << "）" << std::endl;
            av_frame_unref(input_frame);
            continue;
        }

        // 8. 接收编码Packet
        while (true) {
            ret = avcodec_receive_packet(enc_ctx, pkt);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) {
                char err_buf[1024];
                av_strerror(ret, err_buf, sizeof(err_buf));
                std::cerr << "[Error] 接收编码Packet失败：" << err_buf << std::endl;
                break;
            }

            std::cout << "[AudioEncoder] 编码成功：PTS=" << pkt->pts << "，大小=" << pkt->size << std::endl;
            av_packet_rescale_ts(pkt, enc_ctx->time_base, output_time_base);
            pkt->stream_index = 1;
            if (!g_audio_pkt_ringbuf.push(pkt)) {
                std::cerr << "[Warn] Packet推入缓冲区失败" << std::endl;
            }
            av_packet_unref(pkt);
        }

        av_frame_unref(input_frame);
    }

    // 9. 刷新编码器
    std::cout << "[AudioEncoder Info] 刷新编码器剩余数据" << std::endl;
    ret = avcodec_send_frame(enc_ctx, nullptr);
    if (ret < 0) {
        char err_buf[1024];
        av_strerror(ret, err_buf, sizeof(err_buf));
        std::cerr << "[Error] 刷新编码器失败：" << err_buf << std::endl;
    }
    while (ret >= 0) {
        ret = avcodec_receive_packet(enc_ctx, pkt);
        if (ret < 0) break;
        av_packet_rescale_ts(pkt, enc_ctx->time_base, output_time_base);
        pkt->stream_index = 1;
        g_audio_pkt_ringbuf.push(pkt);
        av_packet_unref(pkt);
    }

    // 10. 释放资源
    g_audio_pkt_ringbuf.flush();
    av_frame_free(&input_frame);
    av_frame_free(&encode_frame);
    av_packet_free(&pkt);
    avcodec_free_context(&enc_ctx);
    std::cout << "[AudioEncoder Info] 编码线程退出" << std::endl;
}