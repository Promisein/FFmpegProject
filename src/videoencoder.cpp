//
// Created by Jianing on 2025/12/22.
//
#include "videoencoder.h"
#include "pipeline.h"
#include "ffmpeg_raii.h"
#include "video_rotate.h"
#include <iostream>
#include <cmath>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/error.h>
}

void video_encode_thread(AVCodecParameters* src_codec_par, AVRational output_time_base,
                         RingBuffer<AVFrame*>& in_rb, DeepCopyPacketQueue& out_q,
                         const ProcessingConfig& config,
                         Pipeline* pipeline) {
    if (!src_codec_par) {
        if (pipeline) pipeline->report_error("[VideoEncoder] 输入编码器参数为空指针");
        return;
    }

    const AVCodec* encoder = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
    if (!encoder) {
        if (pipeline) pipeline->report_error("[VideoEncoder] 找不到MPEG4编码器");
        return;
    }

    // 旋转处理器
    VideoRotateProcessor rotator;
    bool need_rotate = false;
    if (config.rotate != ROTATE_NONE) {
        rotator = VideoRotateProcessor(config.rotate);
        need_rotate = true;
        std::cout << "[VideoEncoder Info] 旋转已启用: " << config.rotate << "度\n";
    }

    // 确定编码器输出分辨率（90/270旋转时交换宽高）
    int enc_width = src_codec_par->width;
    int enc_height = src_codec_par->height;
    if (config.rotate == ROTATE_90_CW || config.rotate == ROTATE_270_CW) {
        std::swap(enc_width, enc_height);
    }
    std::cout << "[VideoEncoder Info] 编码输出分辨率: " << enc_width << "x" << enc_height << "\n";

    double speed = config.speed_ratio;
    if (speed != 1.0) {
        std::cout << "[VideoEncoder Info] 倍速: " << speed << "x\n";
    }

    CodecContextPtr enc_ctx(avcodec_alloc_context3(encoder));
    if (!enc_ctx) {
        if (pipeline) pipeline->report_error("[VideoEncoder] 分配视频编码器上下文失败");
        return;
    }

    enc_ctx->codec_id = AV_CODEC_ID_MPEG4;
    enc_ctx->width = enc_width;
    enc_ctx->height = enc_height;
    enc_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    enc_ctx->time_base = output_time_base;
    enc_ctx->framerate = av_inv_q(output_time_base);
    enc_ctx->bit_rate = 1000000;
    enc_ctx->gop_size = 10;
    enc_ctx->max_b_frames = 0;
    enc_ctx->codec_tag = 0x7634706d;

    char err_buf[1024];
    int ret = avcodec_open2(enc_ctx.get(), encoder, nullptr);
    if (ret < 0) {
        av_strerror(ret, err_buf, sizeof(err_buf));
        if (pipeline) pipeline->report_error(std::string("[VideoEncoder] 打开MPEG4编码器失败：") + err_buf);
        return;
    }

    std::cout << "[VideoEncoder Info] MPEG4编码器打开成功\n";

    FramePtr local_frame(av_frame_alloc());
    PacketPtr pkt(av_packet_alloc());
    if (!local_frame || !pkt) {
        if (pipeline) pipeline->report_error("[VideoEncoder] 分配Frame/Packet失败");
        return;
    }

    int input_frame_count = 0;   // 从环形缓冲区读取的帧数
    int output_frame_count = 0;  // 实际发送到编码器的帧数

    while (true) {
        AVFrame* raw_frame = local_frame.get();
        bool success = in_rb.pop(raw_frame);
        if (!success) {
            std::cout << "[VideoEncoder Info] 环形缓冲区已空，停止接收帧\n";
            break;
        }

        input_frame_count++;

        if (!local_frame->data[0]) {
            std::cerr << "[VideoEncoder Warn] 无效视频Frame（第" << input_frame_count << "帧），跳过\n";
            av_frame_unref(local_frame.get());
            continue;
        }

        // ========== 步骤1: 旋转 ==========
        AVFrame* frame_to_encode = local_frame.get();
        FramePtr rotated_frame;
        if (need_rotate) {
            rotated_frame.reset(rotator.rotateFrame(local_frame.get()));
            if (rotated_frame) {
                frame_to_encode = rotated_frame.get();
            } else {
                std::cerr << "[VideoEncoder Warn] 旋转失败，使用原始帧\n";
            }
        }

        // ========== 步骤2: 倍速处理 ==========
        if (speed > 1.0) {
            // 快放：跳帧
            // 2x: 每隔1帧取1帧 (encode frame 0,2,4,...)
            // 1.5x: 每3帧取2帧 (encode frame 0,1,3,4,6,7,...)
            // 4x: 每4帧取1帧 (encode frame 0,4,8,...)
            int input_idx = input_frame_count - 1; // 0-based
            double expected_output = input_idx / speed;
            int should_encode = static_cast<int>(expected_output);
            if (should_encode < output_frame_count) {
                // 跳帧：不编码此帧
                av_frame_unref(local_frame.get());
                continue;
            }
        }

        // 设置PTS（基于输出帧计数）
        frame_to_encode->pts = output_frame_count;

        // 发送到编码器
        ret = avcodec_send_frame(enc_ctx.get(), frame_to_encode);
        if (ret < 0) {
            av_strerror(ret, err_buf, sizeof(err_buf));
            std::cerr << "[VideoEncoder Warn] 第" << output_frame_count << "帧编码发送失败：" << err_buf << "\n";
            av_frame_unref(local_frame.get());
            continue;
        }

        output_frame_count++;

        // 接收编码包
        while (true) {
            ret = avcodec_receive_packet(enc_ctx.get(), pkt.get());
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) {
                av_strerror(ret, err_buf, sizeof(err_buf));
                std::cerr << "[VideoEncoder Warn] 接收编码包失败：" << err_buf << "\n";
                break;
            }

            pkt->stream_index = 0;
            av_packet_rescale_ts(pkt.get(), enc_ctx->time_base, output_time_base);

            if (output_frame_count % 10 == 0) {
                std::cout << "[VideoEncoder Info] 编码MPEG4 Packet: pts=" << pkt->pts
                          << " size=" << pkt->size << "（第" << output_frame_count << "帧）\n";
            }

            out_q.push(*pkt);
            av_packet_unref(pkt.get());
        }

        // ========== 慢放: 复制帧 ==========
        if (speed < 1.0) {
            double repeats = 1.0 / speed; // 0.5x → 2 repeats, 0.75x → 1.333 repeats
            int repeat_count = static_cast<int>(std::round(repeats));
            if (repeat_count < 1) repeat_count = 1;

            for (int r = 1; r < repeat_count; r++) {
                frame_to_encode->pts = output_frame_count;

                ret = avcodec_send_frame(enc_ctx.get(), frame_to_encode);
                if (ret < 0) continue;

                output_frame_count++;

                while (true) {
                    ret = avcodec_receive_packet(enc_ctx.get(), pkt.get());
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                    if (ret < 0) break;

                    pkt->stream_index = 0;
                    av_packet_rescale_ts(pkt.get(), enc_ctx->time_base, output_time_base);
                    out_q.push(*pkt);
                    av_packet_unref(pkt.get());
                }
            }
        }

        av_frame_unref(local_frame.get());
    }

    std::cout << "[VideoEncoder Info] 开始刷新编码器剩余数据（输入" << input_frame_count
              << "帧，输出" << output_frame_count << "帧）\n";
    ret = avcodec_send_frame(enc_ctx.get(), nullptr);
    if (ret < 0) {
        av_strerror(ret, err_buf, sizeof(err_buf));
        std::cerr << "[VideoEncoder Warn] 刷新编码器失败：" << err_buf << "\n";
    }

    while (true) {
        ret = avcodec_receive_packet(enc_ctx.get(), pkt.get());
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) {
            av_strerror(ret, err_buf, sizeof(err_buf));
            std::cerr << "[VideoEncoder Warn] 刷新时接收编码包失败：" << err_buf << "\n";
            break;
        }

        av_packet_rescale_ts(pkt.get(), enc_ctx->time_base, output_time_base);
        pkt->stream_index = 0;
        out_q.push(*pkt);
        av_packet_unref(pkt.get());
    }

    out_q.mark_done();
    std::cout << "[VideoEncoder Info] 视频编码线程退出，输入" << input_frame_count
              << "帧，输出" << output_frame_count << "帧\n";
}
