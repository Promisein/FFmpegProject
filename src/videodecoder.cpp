//
// Created by Jianing on 2025/12/22.
//
#include "videodecoder.h"
#include "pipeline.h"
#include "ffmpeg_raii.h"
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
        std::cout << "[Info] YUV文件已打开: " << filename << "\n";
        return true;
    }

    void write_frame(AVFrame* frame) {
        if (!yuv_file.is_open() || !frame || !frame->data[0]) {
            return;
        }
        int width = frame->width;
        int height = frame->height;
        for (int i = 0; i < height; i++) {
            yuv_file.write(reinterpret_cast<char*>(frame->data[0] + i * frame->linesize[0]), width);
        }
        for (int i = 0; i < height/2; i++) {
            yuv_file.write(reinterpret_cast<char*>(frame->data[1] + i * frame->linesize[1]), width/2);
        }
        for (int i = 0; i < height/2; i++) {
            yuv_file.write(reinterpret_cast<char*>(frame->data[2] + i * frame->linesize[2]), width/2);
        }
        frame_count++;
        if (frame_count % 10 == 0) {
            std::cout << "[YUV] 已写入 " << frame_count << " 帧到YUV文件\n";
        }
    }

    void close() {
        if (yuv_file.is_open()) {
            yuv_file.close();
            std::cout << "[Info] YUV文件已关闭，共写入 " << frame_count << " 帧\n";
        }
    }

    ~YUVFileWriter() { close(); }
};
#endif

void video_decode_thread(AVCodecParameters* codec_par,
                         PacketQueue<AVPacket>& in_queue,
                         RingBuffer<AVFrame*>& out_rb,
                         Pipeline* pipeline) {
    std::cout << "start videoDecode!\n";

#if ENABLE_YUV_OUTPUT
    YUVFileWriter yuv_writer;
    if (!yuv_writer.open("output.yuv")) {
        std::cerr << "[Warning] YUV文件输出功能初始化失败，但继续解码流程\n";
    }
#endif

    const AVCodec* codec = avcodec_find_decoder(codec_par->codec_id);
    if (!codec) {
        if (pipeline) pipeline->report_error("[VideoDecoder] 找不到视频解码器");
        return;
    }

    CodecContextPtr codec_ctx(avcodec_alloc_context3(codec));
    if (!codec_ctx) {
        if (pipeline) pipeline->report_error("[VideoDecoder] 分配视频解码器上下文失败");
        return;
    }
    if (avcodec_parameters_to_context(codec_ctx.get(), codec_par) < 0) {
        if (pipeline) pipeline->report_error("[VideoDecoder] 复制视频流参数失败");
        return;
    }
    if (avcodec_open2(codec_ctx.get(), codec, nullptr) < 0) {
        if (pipeline) pipeline->report_error("[VideoDecoder] 打开视频解码器失败");
        return;
    }

    AVPacket pkt;
    FramePtr frame(av_frame_alloc());
    int frame_count = 0;

    while (in_queue.pop(pkt)) {
        if (!pkt.data) {
            avcodec_send_packet(codec_ctx.get(), nullptr);
            break;
        }

        if (avcodec_send_packet(codec_ctx.get(), &pkt) < 0) {
            std::cerr << "[Warn] 视频Packet发送失败\n";
            av_packet_unref(&pkt);
            continue;
        }

        while (avcodec_receive_frame(codec_ctx.get(), frame.get()) >= 0) {
            frame_count++;
            if (frame_count % 10 == 0) {
                std::cout << "[Video] 解码YUV帧: pts=" << frame->pts
                          << " width=" << frame->width
                          << " height=" << frame->height
                          << " → 推入环形缓冲区\n";
            }

#if ENABLE_YUV_OUTPUT
            if (frame->format == AV_PIX_FMT_YUV420P) {
                yuv_writer.write_frame(frame.get());
            } else {
                if (frame_count % 10 == 0) {
                    std::cout << "[Info] 非YUV420P格式(" << frame->format
                              << ")，跳过YUV文件写入\n";
                }
            }
#endif
            out_rb.push(frame.get());
            av_frame_unref(frame.get());
        }
        av_packet_unref(&pkt);
    }

    out_rb.flush();
    std::cout << "[VideoDecoder Info] 视频解码线程退出，共处理 " << frame_count << " 帧\n";
    // RAII: codec_ctx, frame 自动释放
}
