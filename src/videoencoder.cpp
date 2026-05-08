//
// Created by Jianing on 2025/12/22.
//
#include "videoencoder.h"
#include "pipeline.h"
#include "ffmpeg_raii.h"
#include <iostream>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/error.h>
}

void video_encode_thread(AVCodecParameters* src_codec_par, AVRational output_time_base,
                         RingBuffer<AVFrame*>& in_rb, DeepCopyPacketQueue& out_q,
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

    CodecContextPtr enc_ctx(avcodec_alloc_context3(encoder));
    if (!enc_ctx) {
        if (pipeline) pipeline->report_error("[VideoEncoder] 分配视频编码器上下文失败");
        return;
    }

    enc_ctx->codec_id = AV_CODEC_ID_MPEG4;
    enc_ctx->width = src_codec_par->width;
    enc_ctx->height = src_codec_par->height;
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

    std::cout << "[VideoEncoder Info] MPEG4编码器打开成功（分辨率："
              << enc_ctx->width << "x" << enc_ctx->height
              << ", codec_tag=0x" << std::hex << enc_ctx->codec_tag << std::dec << "）\n";

    FramePtr local_frame(av_frame_alloc());
    PacketPtr pkt(av_packet_alloc());
    if (!local_frame || !pkt) {
        if (pipeline) pipeline->report_error("[VideoEncoder] 分配Frame/Packet失败");
        return;
    }

    int frame_count = 0;

    while (true) {
        AVFrame* raw_frame = local_frame.get();
        bool success = in_rb.pop(raw_frame);
        if (!success) {
            std::cout << "[VideoEncoder Info] 环形缓冲区已空，停止接收帧\n";
            break;
        }

        frame_count++;

        if (!local_frame->data[0]) {
            std::cerr << "[VideoEncoder Warn] 无效视频Frame（第" << frame_count << "帧），跳过\n";
            av_frame_unref(local_frame.get());
            continue;
        }

        local_frame->pts = frame_count - 1;

        ret = avcodec_send_frame(enc_ctx.get(), local_frame.get());
        if (ret < 0) {
            av_strerror(ret, err_buf, sizeof(err_buf));
            std::cerr << "[VideoEncoder Warn] 第" << frame_count << "帧编码发送失败：" << err_buf << "\n";
            av_frame_unref(local_frame.get());
            continue;
        }

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

            if ((pkt->flags & AV_PKT_FLAG_KEY) && (frame_count % 10 == 0)) {
                std::cout << "[VideoEncoder Info] 关键帧 packet size=" << pkt->size << "\n";
            }
            if (frame_count % 10 == 0) {
                std::cout << "[VideoEncoder Info] 编码MPEG4 Packet: pts=" << pkt->pts
                          << " size=" << pkt->size << "（第" << frame_count << "帧）\n";
            }

            out_q.push(*pkt);
            av_packet_unref(pkt.get());
        }

        av_frame_unref(local_frame.get());
    }

    std::cout << "[VideoEncoder Info] 开始刷新编码器剩余数据（共处理" << frame_count << "帧）\n";
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
    std::cout << "[VideoEncoder Info] 视频编码线程退出，共处理" << frame_count << "帧\n";
    // RAII: enc_ctx, local_frame, pkt 自动释放
}
