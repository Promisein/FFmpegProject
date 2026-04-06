//
// Created by Jianing on 2025/12/22.
//
#include "mux.h"
#include <iostream>
#include <queue>
#include <functional>
#include <memory>
#include <thread>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
}

// 包比较器（用于排序，按PTS从小到大）
struct PacketComparator {
    bool operator()(const AVPacket* a, const AVPacket* b) const {
        if (!a || !b) return false;
        // 比较pts，如果pts相同，优先视频包
        if (a->pts == b->pts) {
            return a->stream_index > b->stream_index; // 视频索引小，所以当视频索引<音频索引时，视频优先
        }
        return a->pts > b->pts; // 小顶堆，pts小的优先
    }
};

void mux_thread(const std::string& output_file,
                AVCodecParameters* video_enc_par,
                AVCodecParameters* audio_enc_par,
                AVRational output_time_base)
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
    if (video_stream->codecpar->codec_tag == 0) {
        video_stream->codecpar->codec_tag = 0x7634706d; // 'mp4v'
    }

    // 设置视频流时间基
    video_stream->time_base = output_time_base;

    std::cout << "[Mux Info] 视频流配置: 分辨率="
              << video_stream->codecpar->width << "x" << video_stream->codecpar->height
              << ", 编码器ID=" << video_stream->codecpar->codec_id
              << ", codec_tag=0x" << std::hex << video_stream->codecpar->codec_tag << std::dec
              << ", 时间基=" << video_stream->time_base.num << "/" << video_stream->time_base.den << "\n";
    std::cout << "[Mux Info] 输出时间基: " << output_time_base.num << "/" << output_time_base.den << "\n";

    // 创建音频流（如果音频参数有效）
    AVStream* audio_stream = nullptr;
    if (audio_enc_par && audio_enc_par->codec_type == AVMEDIA_TYPE_AUDIO) {
        audio_stream = avformat_new_stream(out_fmt_ctx, nullptr);
        if (audio_stream) {
            ret = avcodec_parameters_copy(audio_stream->codecpar, audio_enc_par);
            if (ret < 0) {
                char err_buf[1024];
                av_strerror(ret, err_buf, sizeof(err_buf));
                std::cerr << "[Mux Warn] 复制音频编码参数失败: " << err_buf << "\n";
                audio_stream = nullptr;
            } else {
                // 设置音频流时间基为1/采样率
                audio_stream->time_base = (AVRational){1, audio_enc_par->sample_rate};
                std::cout << "[Mux Info] 音频流配置: 采样率=" << audio_stream->codecpar->sample_rate
                          << ", 声道数=" << audio_stream->codecpar->channels
                          << ", 编码器ID=" << audio_stream->codecpar->codec_id
                          << ", 时间基=" << audio_stream->time_base.num << "/" << audio_stream->time_base.den << "\n";
            }
        }
    } else {
        std::cout << "[Mux Info] 无音频流参数，仅处理视频\n";
    }

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

    std::cout << "[Mux] 开始写入数据包...\n";

    // 使用优先队列进行音视频同步（按PTS排序）
    std::priority_queue<AVPacket*, std::vector<AVPacket*>, PacketComparator> packet_queue;

    int video_packet_count = 0;
    int audio_packet_count = 0;
    bool video_done = false;
    bool audio_done = false;

    // 音频时间戳校准变量
    int64_t audio_accumulated_samples = 0;  // 累计音频样本数
    int64_t last_audio_pts = 0;             // 上一个音频包的PTS

    // 主循环：交错写入音视频包
    while (!video_done || !audio_done || !packet_queue.empty()) {
        // 从视频队列获取包（如果未完成）
        if (!video_done) {
            AVPacket* video_pkt = av_packet_alloc();
            if (video_pkt) {
                bool success = g_en_video_pkt_queue.pop(*video_pkt);
                if (success) {
                    video_pkt->stream_index = video_stream->index;
                    
                    // 检查视频包的PTS是否有效
                    if (video_pkt->pts < 0) {
                        std::cerr << "[Mux Warn] 无效的视频包PTS: " << video_pkt->pts << "，跳过此包\n";
                        av_packet_free(&video_pkt);
                        continue;
                    }
                    
                    // 时间基转换：从编码器输出时间基到视频流时间基
                    av_packet_rescale_ts(video_pkt, output_time_base, video_stream->time_base);
                    packet_queue.push(video_pkt);
                    video_packet_count++;

                    // 调试信息
                    if (video_packet_count % 100 == 0) {
                        std::cout << "[Mux Debug] 视频包: pts=" << video_pkt->pts
                                  << " (" << (double)video_pkt->pts * video_stream->time_base.num / video_stream->time_base.den << "秒)\n";
                    }
                } else {
                    // 视频队列已空
                    video_done = true;
                    av_packet_free(&video_pkt);
                }
            }
        }

        // 从音频队列获取包（如果存在音频流且未完成）
        if (audio_stream && !audio_done) {
            AVPacket* audio_pkt = av_packet_alloc();
            if (audio_pkt) {
                bool success = g_en_audio_pkt_queue.pop(*audio_pkt);
                if (success) {
                    audio_pkt->stream_index = audio_stream->index;

                    // 关键修复：音频包时间戳重新计算
                    // AC3通常每帧1536个样本，但我们使用更通用的方法
                    // 对于AC3，我们可以根据采样率计算正确的持续时间

                    // 方法1：如果编码器给我们提供了正确的duration
                    if (audio_pkt->duration <= 0) {
                        // 方法2：对于AC3，每帧通常是1536个样本
                        int samples_per_frame = 1536; // AC3标准帧大小
                        audio_pkt->duration = samples_per_frame;
                    }

                    // 设置音频包的pts：基于累计样本数
                    audio_pkt->pts = av_rescale_q(audio_accumulated_samples,
                                                  (AVRational){1, audio_stream->codecpar->sample_rate},
                                                  audio_stream->time_base);
                    audio_accumulated_samples += audio_pkt->duration;

                    // 确保dts与pts相同（音频通常不需要B帧）
                    audio_pkt->dts = audio_pkt->pts;

                    packet_queue.push(audio_pkt);
                    audio_packet_count++;

                    // 调试信息
                    if (audio_packet_count % 100 == 0) {
                        std::cout << "[Mux Debug] 音频包: pts=" << audio_pkt->pts
                                  << " (" << (double)audio_pkt->pts * audio_stream->time_base.num / audio_stream->time_base.den << "秒)"
                                  << ", 累计样本=" << audio_accumulated_samples << "\n";
                    }
                } else {
                    // 音频队列已空
                    audio_done = true;
                    av_packet_free(&audio_pkt);
                }
            }
        }

        // 写入当前PTS最小的包（实现音视频交错）
        while (!packet_queue.empty()) {
            AVPacket* pkt = packet_queue.top();
            packet_queue.pop();

            // 写入数据包
            ret = av_interleaved_write_frame(out_fmt_ctx, pkt);
            if (ret < 0) {
                char err_buf[1024];
                av_strerror(ret, err_buf, sizeof(err_buf));
                std::cerr << "[Mux Error] 写入数据包失败: " << err_buf
                          << " (stream=" << pkt->stream_index
                          << ", pts=" << pkt->pts << ", size=" << pkt->size
                          << ")\n";
            }

            // 每1000个包输出一次信息
            int total_packets = video_packet_count + audio_packet_count;
            if (total_packets % 1000 == 0) {
                std::cout << "[Mux] 已处理 " << total_packets
                          << " 个包（视频: " << video_packet_count
                          << ", 音频: " << audio_packet_count << "）\n";
            }

            av_packet_free(&pkt);
        }

        // 防止过度消耗CPU
        if (packet_queue.empty() && (!video_done || !audio_done)) {
            // 小等待，避免busy loop
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
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
              << "，视频包: " << video_packet_count
              << "，音频包: " << audio_packet_count
              << "，音频总样本数: " << audio_accumulated_samples << "\n";
}