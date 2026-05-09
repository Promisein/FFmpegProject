//
// Created by Jianing on 2025/12/22.
//
#include "videoencoder.h"
#include "pipeline.h"
#include "ffmpeg_raii.h"
#include "video_rotate.h"
#include "logger.h"
#include <cmath>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

// SwsContext RAII 包装
struct SwsContextDeleter {
    void operator()(SwsContext* ctx) const {
        if (ctx) sws_freeContext(ctx);
    }
};
using SwsContextPtr = std::unique_ptr<SwsContext, SwsContextDeleter>;

void video_encode_thread(AVCodecParameters* src_codec_par, AVRational output_time_base,
                         RingBuffer<AVFrame*>& in_rb, DeepCopyPacketQueue& out_q,
                         const ProcessingConfig& config,
                         Pipeline* pipeline) {
    if (!src_codec_par) {
        if (pipeline) pipeline->report_error("[VideoEncoder] 输入编码器参数为空指针");
        return;
    }

    // 编码器选择
    AVCodecID codec_id;
    const char* codec_name = "";
    switch (config.video_codec) {
        case VIDEO_CODEC_H264:
            codec_id = AV_CODEC_ID_H264;
            codec_name = "H264";
            break;
        case VIDEO_CODEC_MPEG4:
        default:
            codec_id = AV_CODEC_ID_MPEG4;
            codec_name = "MPEG4";
            break;
    }

    const AVCodec* encoder = avcodec_find_encoder(codec_id);
    if (!encoder) {
        if (pipeline) pipeline->report_error(std::string("[VideoEncoder] 找不到") + codec_name + "编码器");
        return;
    }

    // 旋转处理器
    VideoRotateProcessor rotator;
    bool need_rotate = false;
    if (config.rotate != ROTATE_NONE) {
        rotator = VideoRotateProcessor(config.rotate);
        need_rotate = true;
        Logger::info("VideoEncoder", std::string("旋转已启用: ") + std::to_string(config.rotate) + "度");
    }

    // 确定编码器输出分辨率（90/270旋转时交换宽高）
    int enc_width = src_codec_par->width;
    int enc_height = src_codec_par->height;
    if (config.rotate == ROTATE_90_CW || config.rotate == ROTATE_270_CW) {
        std::swap(enc_width, enc_height);
    }
    Logger::info("VideoEncoder", std::string("编码输出分辨率: ")
                 + std::to_string(enc_width) + "x" + std::to_string(enc_height));

    double speed = config.speed_ratio;
    if (speed != 1.0) {
        Logger::info("VideoEncoder", std::string("倍速: ") + std::to_string(speed) + "x");
    }

    CodecContextPtr enc_ctx(avcodec_alloc_context3(encoder));
    if (!enc_ctx) {
        if (pipeline) pipeline->report_error("[VideoEncoder] 分配视频编码器上下文失败");
        return;
    }

    enc_ctx->codec_id = codec_id;
    enc_ctx->width = enc_width;
    enc_ctx->height = enc_height;
    enc_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    enc_ctx->time_base = output_time_base;
    enc_ctx->framerate = av_inv_q(output_time_base);
    enc_ctx->bit_rate = 1000000;
    enc_ctx->gop_size = 10;
    enc_ctx->max_b_frames = 0;

    if (config.video_codec == VIDEO_CODEC_H264) {
        enc_ctx->codec_tag = 0;
        av_opt_set(enc_ctx->priv_data, "preset", "medium", 0);
    } else {
        enc_ctx->codec_tag = 0x7634706d;  // 'mp4v'
    }

    char err_buf[1024];
    int ret = avcodec_open2(enc_ctx.get(), encoder, nullptr);
    if (ret < 0) {
        av_strerror(ret, err_buf, sizeof(err_buf));
        if (pipeline) pipeline->report_error(std::string("[VideoEncoder] 打开MPEG4编码器失败：") + err_buf);
        return;
    }

    Logger::info("VideoEncoder", std::string(codec_name) + "编码器打开成功");

    FramePtr local_frame(av_frame_alloc());
    PacketPtr pkt(av_packet_alloc());
    if (!local_frame || !pkt) {
        if (pipeline) pipeline->report_error("[VideoEncoder] 分配Frame/Packet失败");
        return;
    }

    // 像素格式转换上下文（懒初始化）
    SwsContextPtr sws_ctx;
    FramePtr converted_frame;

    int input_frame_count = 0;
    int output_frame_count = 0;

    while (true) {
        AVFrame* raw_frame = local_frame.get();
        bool success = in_rb.pop(raw_frame);
        if (!success) {
            Logger::info("VideoEncoder", "环形缓冲区已空，停止接收帧");
            break;
        }

        input_frame_count++;

        if (!local_frame->data[0]) {
            Logger::warn("VideoEncoder", std::string("无效视频Frame（第")
                         + std::to_string(input_frame_count) + "帧），跳过");
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
                Logger::warn("VideoEncoder", "旋转失败，使用原始帧");
            }
        }

        // ========== 步骤2: 像素格式转换 (任意格式 → YUV420P) ==========
        bool need_convert = (frame_to_encode->format != AV_PIX_FMT_YUV420P);
        FramePtr converted;
        if (need_convert) {
            if (!sws_ctx) {
                sws_ctx.reset(sws_getContext(
                    frame_to_encode->width, frame_to_encode->height,
                    static_cast<AVPixelFormat>(frame_to_encode->format),
                    enc_width, enc_height, AV_PIX_FMT_YUV420P,
                    SWS_BILINEAR, nullptr, nullptr, nullptr));
                if (!sws_ctx) {
                    Logger::error("VideoEncoder", "创建SwsContext失败");
                    av_frame_unref(local_frame.get());
                    continue;
                }
                Logger::info("VideoEncoder",
                    std::string("像素格式转换已启用: ")
                    + av_get_pix_fmt_name(static_cast<AVPixelFormat>(frame_to_encode->format))
                    + " → YUV420P");
            }

            converted.reset(av_frame_alloc());
            if (!converted) {
                Logger::warn("VideoEncoder", "分配转换帧失败");
                av_frame_unref(local_frame.get());
                continue;
            }
            converted->format = AV_PIX_FMT_YUV420P;
            converted->width = enc_width;
            converted->height = enc_height;
            ret = av_frame_get_buffer(converted.get(), 0);
            if (ret < 0) {
                Logger::warn("VideoEncoder", "分配转换帧缓冲区失败");
                av_frame_unref(local_frame.get());
                continue;
            }

            sws_scale(sws_ctx.get(),
                frame_to_encode->data, frame_to_encode->linesize,
                0, frame_to_encode->height,
                converted->data, converted->linesize);
            // sws_scale returns the output slice height, no error code
            // Copy PTS from source
            converted->pts = frame_to_encode->pts;
            frame_to_encode = converted.get();
            converted_frame = std::move(converted);
        }

        // ========== 步骤3: 倍速处理 ==========
        if (speed > 1.0) {
            int input_idx = input_frame_count - 1;
            double expected_output = input_idx / speed;
            int should_encode = static_cast<int>(expected_output);
            if (should_encode < output_frame_count) {
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
            Logger::warn("VideoEncoder", std::string("第") + std::to_string(output_frame_count)
                         + "帧编码发送失败：" + err_buf);
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
                Logger::warn("VideoEncoder", std::string("接收编码包失败：") + err_buf);
                break;
            }

            pkt->stream_index = 0;
            av_packet_rescale_ts(pkt.get(), enc_ctx->time_base, output_time_base);

            if (output_frame_count % 50 == 0) {
                Logger::debug("VideoEncoder", std::string("编码") + codec_name + " Packet: pts="
                              + std::to_string(pkt->pts) + " size=" + std::to_string(pkt->size)
                              + "（第" + std::to_string(output_frame_count) + "帧）");
            }

            out_q.push(*pkt);
            av_packet_unref(pkt.get());
        }

        // ========== 慢放: 复制帧 ==========
        if (speed < 1.0) {
            double repeats = 1.0 / speed;
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

    Logger::info("VideoEncoder", std::string("开始刷新编码器剩余数据（输入")
                 + std::to_string(input_frame_count) + "帧，输出"
                 + std::to_string(output_frame_count) + "帧）");
    ret = avcodec_send_frame(enc_ctx.get(), nullptr);
    if (ret < 0) {
        av_strerror(ret, err_buf, sizeof(err_buf));
        Logger::warn("VideoEncoder", std::string("刷新编码器失败：") + err_buf);
    }

    while (true) {
        ret = avcodec_receive_packet(enc_ctx.get(), pkt.get());
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) {
            av_strerror(ret, err_buf, sizeof(err_buf));
            Logger::warn("VideoEncoder", std::string("刷新时接收编码包失败：") + err_buf);
            break;
        }

        av_packet_rescale_ts(pkt.get(), enc_ctx->time_base, output_time_base);
        pkt->stream_index = 0;
        out_q.push(*pkt);
        av_packet_unref(pkt.get());
    }

    out_q.mark_done();
    if (pipeline) pipeline->video_encoded_frames.store(output_frame_count, std::memory_order_release);
    Logger::info("VideoEncoder", std::string("视频编码线程退出，输入")
                 + std::to_string(input_frame_count) + "帧，输出"
                 + std::to_string(output_frame_count) + "帧");
}
