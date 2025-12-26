//
// Created by Jianing on 2025/12/22.
//
#include "videodecoder.h"
#include "ring_buffer.h"
#include <iostream>
#include <fstream>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
}

// ============ YUV输出开关 ============
#define ENABLE_YUV_OUTPUT 0
// ====================================

#if ENABLE_YUV_OUTPUT
class YUVFileWriter {
private:
    std::ofstream yuv_file;
    int frame_count = 0;

public:
    bool open(const std::string& filename) {
        yuv_file.open(filename, std::ios::binary);
        if (!yuv_file.is_open()) {
            std::cerr << "[Error] 无法打开YUV输出文件: " << filename << "\n";
            return false;
        }
        // 【一次性信息】保留
        std::cout << "[Info] YUV文件已打开: " << filename << "\n";
        return true;
    }

    void write_frame(AVFrame* frame) {
        if (!yuv_file.is_open() || !frame || !frame->data[0]) {
            return;
        }

        int width = frame->width;
        int height = frame->height;

        // 写入Y分量
        for (int i = 0; i < height; i++) {
            yuv_file.write(reinterpret_cast<char*>(frame->data[0] + i * frame->linesize[0]), width);
        }
        // 写入U分量
        for (int i = 0; i < height/2; i++) {
            yuv_file.write(reinterpret_cast<char*>(frame->data[1] + i * frame->linesize[1]), width/2);
        }
        // 写入V分量
        for (int i = 0; i < height/2; i++) {
            yuv_file.write(reinterpret_cast<char*>(frame->data[2] + i * frame->linesize[2]), width/2);
        }

        frame_count++;
        // YUV 写入日志：每10帧输出一次（原逻辑已符合，保留）
        if (frame_count % 10 == 0) {
            std::cout << "[YUV] 已写入 " << frame_count << " 帧到YUV文件\n";
        }
    }

    void close() {
        if (yuv_file.is_open()) {
            yuv_file.close();
            // 【结束信息】保留
            std::cout << "[Info] YUV文件已关闭，共写入 " << frame_count << " 帧\n";
        }
    }

    ~YUVFileWriter() {
        close();
    }
};
#endif

void video_decode_thread(AVCodecParameters* codec_par) {
    // 【启动信息】保留
    std::cout << "start videoDecode!\n";

#if ENABLE_YUV_OUTPUT
    YUVFileWriter yuv_writer;
    if (!yuv_writer.open("output.yuv")) {
        std::cerr << "[Warning] YUV文件输出功能初始化失败，但继续解码流程\n";
    }
#endif

    const AVCodec* codec = avcodec_find_decoder(codec_par->codec_id);
    if (!codec) {
        std::cerr << "[Error] 找不到视频解码器\n";
        return;
    }

    AVCodecContext* codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) {
        std::cerr << "[Error] 分配视频解码器上下文失败\n";
        return;
    }
    if (avcodec_parameters_to_context(codec_ctx, codec_par) < 0) {
        std::cerr << "[Error] 复制视频流参数失败\n";
        avcodec_free_context(&codec_ctx);
        return;
    }

    if (avcodec_open2(codec_ctx, codec, nullptr) < 0) {
        std::cerr << "[Error] 打开视频解码器失败\n";
        avcodec_free_context(&codec_ctx);
        return;
    }

    AVPacket pkt;
    AVFrame* frame = av_frame_alloc();
    int frame_count = 0;  // 👈 新增帧计数器

    while (g_video_pkt_queue.pop(pkt)) {
        if (!pkt.data) { // 空Packet：解码结束
            avcodec_send_packet(codec_ctx, nullptr);
            break;
        }

        if (avcodec_send_packet(codec_ctx, &pkt) < 0) {
            std::cerr << "[Warn] 视频Packet发送失败\n";
            av_packet_unref(&pkt);
            continue;
        }

        // 接收解码帧 → 推入环形缓冲区
        while (avcodec_receive_frame(codec_ctx, frame) >= 0) {
            frame_count++;  // 👈 计数递增

            // 🔁 高频日志：每10帧才输出
            if (frame_count % 10 == 0) {
                std::cout << "[Video] 解码YUV帧: pts=" << frame->pts
                          << " width=" << frame->width
                          << " height=" << frame->height
                          << " → 推入环形缓冲区\n";
            }

#if ENABLE_YUV_OUTPUT
            if (frame->format == AV_PIX_FMT_YUV420P) {
                yuv_writer.write_frame(frame);  // 内部已有 10 帧节流
            } else {
                // 非YUV420P提示：也应节流（避免刷屏）
                if (frame_count % 10 == 0) {
                    std::cout << "[Info] 非YUV420P格式(" << frame->format
                              << ")，跳过YUV文件写入\n";
                }
            }
#endif

            g_video_frame_ringbuf.push(frame);
            av_frame_unref(frame);
        }
        av_packet_unref(&pkt);
    }

    // 结束信号
    g_video_frame_ringbuf.flush();
    av_frame_free(&frame);
    avcodec_free_context(&codec_ctx);

    // 【可选：补充总结信息】
    std::cout << "[VideoDecoder Info] 视频解码线程退出，共处理 " << frame_count << " 帧\n";
}