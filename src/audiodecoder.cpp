//
// Created by Jianing on 2025/12/22.
//
#include "audiodecoder.h"
#include "ring_buffer.h"
#include <iostream>
#include <fstream>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
}

// ============ PCM输出开关 ============
#define ENABLE_PCM_OUTPUT 0
// ====================================

#if ENABLE_PCM_OUTPUT
class PCMFileWriter {
private:
    std::ofstream pcm_file;
    int frame_count = 0;

public:
    bool open(const std::string& filename) {
        pcm_file.open(filename, std::ios::binary);
        if (!pcm_file.is_open()) {
            std::cerr << "[Error] 无法打开PCM输出文件: " << filename << "\n";
            return false;
        }
        std::cout << "[Info] PCM文件已打开: " << filename << "\n";
        return true;
    }

    void write_frame(AVFrame* frame) {
        if (!pcm_file.is_open() || !frame || !frame->data[0]) {
            return;
        }

        // 获取音频帧信息
        int channels = frame->channels;
        int samples_per_channel = frame->nb_samples;
        int bytes_per_sample = av_get_bytes_per_sample(static_cast<AVSampleFormat>(frame->format));

        if (av_sample_fmt_is_planar(static_cast<AVSampleFormat>(frame->format))) {
            // Planar格式：每个声道的数据分开存储
            for (int ch = 0; ch < channels; ch++) {
                pcm_file.write(reinterpret_cast<char*>(frame->data[ch]),
                              samples_per_channel * bytes_per_sample);
            }
        } else {
            // Interleaved格式：所有声道数据交错存储
            pcm_file.write(reinterpret_cast<char*>(frame->data[0]),
                          samples_per_channel * channels * bytes_per_sample);
        }

        frame_count++;
        // PCM 写入日志：每50帧输出一次（音频帧通常比视频帧多）
        if (frame_count % 100 == 0) {
            std::cout << "[PCM] 已写入 " << frame_count << " 帧到PCM文件\n";
        }
    }

    void close() {
        if (pcm_file.is_open()) {
            pcm_file.close();
            std::cout << "[Info] PCM文件已关闭，共写入 " << frame_count << " 帧\n";
        }
    }

    ~PCMFileWriter() {
        close();
    }
};
#endif

void audio_decode_thread(AVCodecParameters* codec_par) {

#if ENABLE_PCM_OUTPUT
    PCMFileWriter pcm_writer;
    if (!pcm_writer.open("output.pcm")) {
        std::cerr << "[Warning] PCM文件输出功能初始化失败，但继续解码流程\n";
    }
#endif

    const AVCodec* codec = avcodec_find_decoder(codec_par->codec_id);
    if (!codec) {
        std::cerr << "[Error] 找不到音频解码器\n";
        return;
    }

    AVCodecContext* codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) {
        std::cerr << "[Error] 分配音频解码器上下文失败\n";
        return;
    }
    if (avcodec_parameters_to_context(codec_ctx, codec_par) < 0) {
        std::cerr << "[Error] 复制音频流参数失败\n";
        avcodec_free_context(&codec_ctx);
        return;
    }

    if (avcodec_open2(codec_ctx, codec, nullptr) < 0) {
        std::cerr << "[Error] 打开音频解码器失败\n";
        avcodec_free_context(&codec_ctx);
        return;
    }

    AVPacket pkt;
    AVFrame* frame = av_frame_alloc();
    int frame_count = 0;  // 👈 新增帧计数器

    while (g_audio_pkt_queue.pop(pkt)) {
        if (!pkt.data) { // 空Packet：解码结束
            avcodec_send_packet(codec_ctx, nullptr);
            break;
        }

        if (avcodec_send_packet(codec_ctx, &pkt) < 0) {
            std::cerr << "[Warn] 音频Packet发送失败\n";
            av_packet_unref(&pkt);
            continue;
        }

        // 接收解码帧 → 推入环形缓冲区
        while (avcodec_receive_frame(codec_ctx, frame) >= 0) {
            frame_count++;  // 👈 计数递增

            // 🔁 高频日志：每50帧才输出（音频帧通常比视频帧多）
            if (frame_count % 100 == 0) {
                std::cout << "[Audio Decode Info] 解码PCM帧: pts=" << frame->pts
                          << " channels=" << frame->channels
                          << " samples=" << frame->nb_samples
                          << " format=" << av_get_sample_fmt_name(static_cast<AVSampleFormat>(frame->format))
                          << " → 推入环形缓冲区\n";
            }

#if ENABLE_PCM_OUTPUT
            // 只支持部分PCM格式输出
            if (frame->format == AV_SAMPLE_FMT_S16P ||
                frame->format == AV_SAMPLE_FMT_S16 ||
                frame->format == AV_SAMPLE_FMT_FLTP ||
                frame->format == AV_SAMPLE_FMT_FLT) {
                pcm_writer.write_frame(frame);
            } else {
                // 非标准PCM格式提示
                if (frame_count % 50 == 0) {
                    std::cout << "[Info] 不支持格式(" << frame->format
                              << ")的PCM输出，跳过文件写入\n";
                }
            }
#endif

            // 推入音频Frame环形缓冲区
            g_audio_frame_ringbuf.push(frame);
            av_frame_unref(frame);
        }
        av_packet_unref(&pkt);
    }

    // 解码结束：发送刷新信号给编码线程
    g_audio_frame_ringbuf.flush();
    av_frame_free(&frame);
    avcodec_free_context(&codec_ctx);

    // 结束信息
    std::cout << "[AudioDecoder Info] 音频解码线程退出，共处理 " << frame_count << " 帧\n";
}