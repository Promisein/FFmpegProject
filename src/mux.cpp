//
// Created by Jianing on 2025/12/22.
//
#include "mux.h"
#include <iostream>
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
}

void mux_thread(const std::string& output_file,
                AVCodecParameters* video_enc_par,
                AVCodecParameters* /*audio_enc_par*/)
{
    std::cout << "[Mux] 开始创建输出文件: " << output_file << "\n";

    AVFormatContext* out_fmt_ctx = nullptr;

    // 创建输出格式上下文 - 显式指定MP4格式
    int ret = avformat_alloc_output_context2(
            &out_fmt_ctx, nullptr, "mp4", output_file.c_str());
    if (ret < 0 || !out_fmt_ctx) {
        char err_buf[1024];
        av_strerror(ret, err_buf, sizeof(err_buf));
        std::cerr << "[Mux Error] 分配输出上下文失败: " << err_buf << "\n";
        return;
    }

    // 创建视频流
    AVStream* video_stream = avformat_new_stream(out_fmt_ctx, nullptr);
    if (!video_stream) {
        std::cerr << "[Mux Error] 创建视频流失败\n";
        avformat_free_context(out_fmt_ctx);
        return;
    }

    // 复制视频编码参数
    ret = avcodec_parameters_copy(video_stream->codecpar, video_enc_par);
    if (ret < 0) {
        char err_buf[1024];
        av_strerror(ret, err_buf, sizeof(err_buf));
        std::cerr << "[Mux Error] 复制视频编码参数失败: " << err_buf << "\n";
        avformat_free_context(out_fmt_ctx);
        return;
    }

    // 对于MP4容器，必须正确设置codec_tag
    // MPEG4在MP4容器中的标准codec_tag是'mp4v' (0x7634706d)
    // 确保codec_tag正确设置
    if (video_stream->codecpar->codec_tag == 0) {
        video_stream->codecpar->codec_tag = 0x7634706d; // 'mp4v'
    }

    // 设置视频流时间基
    video_stream->time_base = (AVRational){1, 25};

    std::cout << "[Mux Info] 视频流配置: 分辨率="
              << video_stream->codecpar->width << "x" << video_stream->codecpar->height
              << ", 编码器ID=" << video_stream->codecpar->codec_id
              << ", codec_tag=0x" << std::hex << video_stream->codecpar->codec_tag << std::dec
              << ", 时间基=" << video_stream->time_base.num << "/" << video_stream->time_base.den << "\n";

    // 打印格式信息（调试用）
    av_dump_format(out_fmt_ctx, 0, output_file.c_str(), 1);

    // 打开输出文件
    if (!(out_fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&out_fmt_ctx->pb, output_file.c_str(), AVIO_FLAG_WRITE);
        if (ret < 0) {
            char err_buf[1024];
            av_strerror(ret, err_buf, sizeof(err_buf));
            std::cerr << "[Mux Error] 打开输出文件失败: " << err_buf << "\n";
            avformat_free_context(out_fmt_ctx);
            return;
        }
    }

    // 写入文件头
    ret = avformat_write_header(out_fmt_ctx, nullptr);
    if (ret < 0) {
        char err_buf[1024];
        av_strerror(ret, err_buf, sizeof(err_buf));
        std::cerr << "[Mux Error] 写入文件头失败: " << err_buf << "\n";
        if (!(out_fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&out_fmt_ctx->pb);
        }
        avformat_free_context(out_fmt_ctx);
        return;
    }

    std::cout << "[Mux] 开始写入视频数据包...\n";

    AVPacket pkt;
    int packet_count = 0;

    // 循环读取视频包
    while (true) {
        // 从队列获取packet
        bool success = g_en_video_pkt_queue.pop(pkt);

        if (!success) {
            std::cout << "[Mux] 视频包队列已空，停止接收\n";
            break;
        }

        packet_count++;

        // 设置流索引
        pkt.stream_index = video_stream->index;

        // 时间基转换：从packet的时间基到视频流时间基
        // 假设pkt的时间基是1/25（来自编码器输出）
        av_packet_rescale_ts(&pkt, (AVRational){1, 25}, video_stream->time_base);

        // 写入数据包
        ret = av_interleaved_write_frame(out_fmt_ctx, &pkt);
        if (ret < 0) {
            char err_buf[1024];
            av_strerror(ret, err_buf, sizeof(err_buf));
            std::cerr << "[Mux Error] 写入视频包失败: " << err_buf
                      << " (pts=" << pkt.pts << ", size=" << pkt.size
                      << ")\n";
        }

        // 每10个包输出一次信息
        if (packet_count % 10 == 0) {
            std::cout << "[Mux] 已写入 " << packet_count << " 个视频包\n";
        }

        av_packet_unref(&pkt);
    }

    // 写入文件尾
    std::cout << "[Mux] 写入文件尾...\n";
    ret = av_write_trailer(out_fmt_ctx);
    if (ret < 0) {
        char err_buf[1024];
        av_strerror(ret, err_buf, sizeof(err_buf));
        std::cerr << "[Mux Warn] 写入文件尾失败: " << err_buf << "\n";
    }

    // 关闭输出文件
    if (!(out_fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        avio_closep(&out_fmt_ctx->pb);
    }

    avformat_free_context(out_fmt_ctx);

    std::cout << "[Mux] 完成！文件: " << output_file
              << "，共写入 " << packet_count << " 个视频包\n";
}