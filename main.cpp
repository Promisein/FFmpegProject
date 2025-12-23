#include <iostream>
#include <thread>
#include "demux.h"
#include "videodecoder.h"
#include "audiodecoder.h"
#include "videoencoder.h"
#include "audioencoder.h"
#include "mux.h"
#include "ring_buffer.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
}

int main(int argc, char* argv[]) {
//    if (argc < 3) {
//        std::cerr << "用法: " << argv[0] << " <输入文件> <输出文件>" << std::endl;
//        return -1;
//    }
    const char* input_file = "../input.mp4";
    const char* output_file = "../ouput.mp4";

    // 初始化FFmpeg
    avformat_network_init();

    // 打开输入文件 & 获取流信息
    AVFormatContext* fmt_ctx = nullptr;
    if (avformat_open_input(&fmt_ctx, input_file, nullptr, nullptr) < 0) {
        std::cerr << "[Error] 打开输入文件失败: " << input_file << std::endl;
        return -1;
    }
    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
        std::cerr << "[Error] 获取媒体流信息失败" << std::endl;
        avformat_close_input(&fmt_ctx);
        return -1;
    }

    // 查找视频流、音频流索引
    int video_stream_idx = -1, audio_stream_idx = -1;
    for (int i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_idx = i;
        } else if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audio_stream_idx = i;
        }
    }
    if (video_stream_idx == -1 || audio_stream_idx == -1) {
        std::cerr << "[Error] 未找到视频/音频流" << std::endl;
        avformat_close_input(&fmt_ctx);
        return -1;
    }

    // 获取音视频解码参数
    AVCodecParameters* video_dec_par = fmt_ctx->streams[video_stream_idx]->codecpar;
    AVCodecParameters* audio_dec_par = fmt_ctx->streams[audio_stream_idx]->codecpar;

    // 定义输出时间基（统一为输入视频流的时间基，保证同步）
    AVRational output_time_base = fmt_ctx->streams[video_stream_idx]->time_base;

    // ====================== 创建所有线程 ======================
    // 1. 解封装线程
    std::thread demux_th(demux_thread, fmt_ctx, video_stream_idx, audio_stream_idx);

    // 2. 解码线程
    std::thread video_dec_th(video_decode_thread, video_dec_par);
    std::thread audio_dec_th(audio_decode_thread, audio_dec_par);

    // 3. 编码线程
    std::thread video_enc_th(video_encode_thread, video_dec_par, output_time_base);
    std::thread audio_enc_th(audio_encode_thread, audio_dec_par, output_time_base);

    // 4. 复用线程（需等待编码参数初始化，此处简化：直接传解码参数，实际可从编码器上下文获取）
    std::thread mux_th(mux_thread, std::string(output_file), video_dec_par, audio_dec_par);

    // ====================== 等待线程结束 ======================
    demux_th.join();
    video_dec_th.join();
    audio_dec_th.join();
    video_enc_th.join();
    audio_enc_th.join();
    mux_th.join();

    // 释放资源
    avformat_close_input(&fmt_ctx);
    avformat_network_deinit();

    return 0;
}