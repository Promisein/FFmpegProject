//
// Created by Jianing on 2025/12/22.
//
#include "mux.h"
#include "pipeline.h"
#include "ffmpeg_raii.h"
#include "logger.h"
#include <queue>
#include <functional>
#include <memory>
#include <thread>
#include <cmath>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
}

struct PacketComparator {
    bool operator()(const AVPacket* a, const AVPacket* b) const {
        if (!a || !b) return false;
        if (a->pts == b->pts) {
            return a->stream_index > b->stream_index;
        }
        return a->pts > b->pts;
    }
};

void mux_thread(const std::string& output_file,
                AVCodecParameters* video_enc_par,
                AVCodecParameters* audio_enc_par,
                AVRational output_time_base,
                DeepCopyPacketQueue& video_q,
                DeepCopyPacketQueue& audio_q,
                const ProcessingConfig& config,
                Pipeline* pipeline) {
    Logger::info("Mux", std::string("开始创建输出文件: ") + output_file);

    AVFormatContext* out_fmt_ctx_raw = nullptr;
    char err_buf[1024];
    int ret = avformat_alloc_output_context2(&out_fmt_ctx_raw, nullptr, "mp4", output_file.c_str());
    if (ret < 0 || !out_fmt_ctx_raw) {
        av_strerror(ret, err_buf, sizeof(err_buf));
        if (pipeline) pipeline->report_error(std::string("[Mux] 分配输出上下文失败: ") + err_buf);
        return;
    }

    auto cleanup_output = [&](AVFormatContext* ctx) {
        if (ctx) {
            if (!(ctx->oformat->flags & AVFMT_NOFILE) && ctx->pb) {
                avio_closep(&ctx->pb);
            }
            avformat_free_context(ctx);
        }
    };

    AVStream* video_stream = avformat_new_stream(out_fmt_ctx_raw, nullptr);
    if (!video_stream) {
        if (pipeline) pipeline->report_error("[Mux] 创建视频流失败");
        cleanup_output(out_fmt_ctx_raw);
        return;
    }

    ret = avcodec_parameters_copy(video_stream->codecpar, video_enc_par);
    if (ret < 0) {
        av_strerror(ret, err_buf, sizeof(err_buf));
        if (pipeline) pipeline->report_error(std::string("[Mux] 复制视频编码参数失败: ") + err_buf);
        cleanup_output(out_fmt_ctx_raw);
        return;
    }

    video_stream->time_base = output_time_base;

    Logger::info("Mux", std::string("视频流配置: 分辨率=")
                 + std::to_string(video_stream->codecpar->width) + "x"
                 + std::to_string(video_stream->codecpar->height)
                 + ", 时间基=" + std::to_string(video_stream->time_base.num)
                 + "/" + std::to_string(video_stream->time_base.den));

    AVStream* audio_stream = nullptr;
    if (audio_enc_par && audio_enc_par->codec_type == AVMEDIA_TYPE_AUDIO) {
        audio_stream = avformat_new_stream(out_fmt_ctx_raw, nullptr);
        if (audio_stream) {
            ret = avcodec_parameters_copy(audio_stream->codecpar, audio_enc_par);
            if (ret < 0) {
                av_strerror(ret, err_buf, sizeof(err_buf));
                Logger::warn("Mux", std::string("复制音频编码参数失败: ") + err_buf);
                audio_stream = nullptr;
            } else {
                audio_stream->time_base = (AVRational){1, audio_enc_par->sample_rate};
                Logger::info("Mux", std::string("音频流配置: 采样率=")
                             + std::to_string(audio_stream->codecpar->sample_rate)
                             + ", 声道数=" + std::to_string(audio_stream->codecpar->channels));
            }
        }
    } else {
        Logger::info("Mux", "无音频流参数，仅处理视频");
    }

    av_dump_format(out_fmt_ctx_raw, 0, output_file.c_str(), 1);

    if (!(out_fmt_ctx_raw->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&out_fmt_ctx_raw->pb, output_file.c_str(), AVIO_FLAG_WRITE);
        if (ret < 0) {
            av_strerror(ret, err_buf, sizeof(err_buf));
            if (pipeline) pipeline->report_error(std::string("[Mux] 打开输出文件失败: ") + err_buf);
            cleanup_output(out_fmt_ctx_raw);
            return;
        }
    }

    ret = avformat_write_header(out_fmt_ctx_raw, nullptr);
    if (ret < 0) {
        av_strerror(ret, err_buf, sizeof(err_buf));
        if (pipeline) pipeline->report_error(std::string("[Mux] 写入文件头失败: ") + err_buf);
        cleanup_output(out_fmt_ctx_raw);
        return;
    }

    Logger::info("Mux", "开始写入数据包...");

    std::priority_queue<AVPacket*, std::vector<AVPacket*>, PacketComparator> packet_queue;

    int video_packet_count = 0;
    int audio_packet_count = 0;
    bool video_done = false;
    bool audio_done = false;
    int64_t audio_accumulated_samples = 0;

    while (!video_done || !audio_done || !packet_queue.empty()) {
        if (!video_done) {
            PacketPtr video_pkt(av_packet_alloc());
            if (video_pkt) {
                bool success = video_q.pop(*video_pkt);
                if (success) {
                    video_pkt->stream_index = video_stream->index;

                    if (video_pkt->pts < 0) {
                        Logger::warn("Mux", std::string("无效的视频包PTS: ")
                                     + std::to_string(video_pkt->pts) + "，跳过此包");
                        continue;
                    }

                    av_packet_rescale_ts(video_pkt.get(), output_time_base, video_stream->time_base);
                    packet_queue.push(video_pkt.release());
                    video_packet_count++;

                    if (video_packet_count % 100 == 0) {
                        Logger::debug("Mux", std::string("视频包: pts=")
                                      + std::to_string(packet_queue.empty() ? -1 : packet_queue.top()->pts));
                    }
                } else {
                    video_done = true;
                }
            }
        }

        if (audio_stream && !audio_done) {
            PacketPtr audio_pkt(av_packet_alloc());
            if (audio_pkt) {
                bool success = audio_q.pop(*audio_pkt);
                if (success) {
                    audio_pkt->stream_index = audio_stream->index;

                    if (audio_pkt->duration <= 0) {
                        int frame_size = audio_stream->codecpar->frame_size;
                        if (frame_size <= 0) frame_size = 1024;
                        audio_pkt->duration = frame_size;
                    }

                    audio_pkt->pts = av_rescale_q(audio_accumulated_samples,
                                                  (AVRational){1, audio_stream->codecpar->sample_rate},
                                                  audio_stream->time_base);
                    audio_accumulated_samples += audio_pkt->duration;
                    audio_pkt->dts = audio_pkt->pts;

                    packet_queue.push(audio_pkt.release());
                    audio_packet_count++;

                    if (audio_packet_count % 100 == 0) {
                        Logger::debug("Mux", std::string("音频包: pts=")
                                      + std::to_string(packet_queue.empty() ? -1 : packet_queue.top()->pts)
                                      + ", 累计=" + std::to_string(audio_accumulated_samples));
                    }
                } else {
                    audio_done = true;
                }
            }
        }

        while (!packet_queue.empty()) {
            AVPacket* pkt = packet_queue.top();
            packet_queue.pop();

            ret = av_interleaved_write_frame(out_fmt_ctx_raw, pkt);
            if (ret < 0) {
                av_strerror(ret, err_buf, sizeof(err_buf));
                Logger::error("Mux", std::string("写入数据包失败: ") + err_buf
                              + " (stream=" + std::to_string(pkt->stream_index)
                              + ", pts=" + std::to_string(pkt->pts)
                              + ", size=" + std::to_string(pkt->size) + ")");
            }

            int total_packets = video_packet_count + audio_packet_count;
            if (total_packets % 1000 == 0) {
                Logger::info("Mux", std::string("已处理 ") + std::to_string(total_packets)
                             + " 个包（视频: " + std::to_string(video_packet_count)
                             + ", 音频: " + std::to_string(audio_packet_count) + "）");
            }

            av_packet_free(&pkt);
        }

        if (packet_queue.empty() && (!video_done || !audio_done)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    Logger::info("Mux", "写入文件尾...");
    ret = av_write_trailer(out_fmt_ctx_raw);
    if (ret < 0) {
        av_strerror(ret, err_buf, sizeof(err_buf));
        Logger::warn("Mux", std::string("写入文件尾失败: ") + err_buf);
    }

    Logger::info("Mux", std::string("完成！文件: ") + output_file
                 + "，视频包: " + std::to_string(video_packet_count)
                 + "，音频包: " + std::to_string(audio_packet_count)
                 + "，音频累计: " + std::to_string(audio_accumulated_samples));

    cleanup_output(out_fmt_ctx_raw);
}
