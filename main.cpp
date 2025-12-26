#include <iostream>
#include <thread>
#include "demux.h"
#include "videodecoder.h"
#include "audiodecoder.h"
#include "videoencoder.h"
#include "audioencoder.h"
#include "mux.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
}
#include <windows.h>

void verify_output_file(const std::string& filename) {
    AVFormatContext* fmt_ctx = nullptr;

    if (avformat_open_input(&fmt_ctx, filename.c_str(), nullptr, nullptr) != 0) {
        std::cerr << "[Verify] 无法打开文件: " << filename << "\n";
        return;
    }

    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
        std::cerr << "[Verify] 无法获取流信息\n";
        avformat_close_input(&fmt_ctx);
        return;
    }

    for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
        AVStream* stream = fmt_ctx->streams[i];
        AVCodecParameters* codecpar = stream->codecpar;

        std::cout << "[Verify] 流 #" << i << ": "
                  << "codec_type=" << (codecpar->codec_type == AVMEDIA_TYPE_VIDEO ? "video" : "audio")
                  << ", codec_id=" << codecpar->codec_id
                  << " (" << avcodec_get_name(codecpar->codec_id) << ")"
                  << ", codec_tag=0x" << std::hex << codecpar->codec_tag << std::dec
                  << ", width=" << codecpar->width
                  << ", height=" << codecpar->height << "\n";
    }

    avformat_close_input(&fmt_ctx);
}

int main(int argc, char* argv[])
{
    SetConsoleOutputCP(CP_UTF8);  // 设置控制台输出为 UTF-8

    const char* input_file = "../input.mp4";
    const char* output_file = "../output.mp4";

    // 初始化FFmpeg
    avformat_network_init();

    // 打开输入文件 & 获取流信息
    AVFormatContext* fmt_ctx = nullptr;
    if (avformat_open_input(&fmt_ctx, input_file, nullptr, nullptr) < 0) {
        std::cerr << "[Error] 打开输入文件失败: " << input_file << "\n";
        return -1;
    }
    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
        std::cerr << "[Error] 获取媒体流信息失败\n";
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
        std::cerr << "[Error] 未找到视频/音频流\n";
        avformat_close_input(&fmt_ctx);
        return -1;
    }


    // 获取音视频解码参数
    AVCodecParameters* video_dec_par = fmt_ctx->streams[video_stream_idx]->codecpar;
    AVCodecParameters* audio_dec_par = fmt_ctx->streams[audio_stream_idx]->codecpar;


    AVCodecParameters* mpeg4_params = avcodec_parameters_alloc();
    mpeg4_params->codec_type = AVMEDIA_TYPE_VIDEO;
    mpeg4_params->codec_id = AV_CODEC_ID_MPEG4;  // MPEG4的ID是12
    mpeg4_params->codec_tag = 0x7634706d;  // 'mp4v'的小端表示
    mpeg4_params->width = video_dec_par->width;
    mpeg4_params->height = video_dec_par->height;
    mpeg4_params->format = AV_PIX_FMT_YUV420P;
    mpeg4_params->bit_rate = 1000000;

    std::cout << "[Main] 创建MPEG4编码参数: codec_id=" << mpeg4_params->codec_id
              << ", codec_tag=0x" << std::hex << mpeg4_params->codec_tag << std::dec
              << ", 分辨率=" << mpeg4_params->width << "x" << mpeg4_params->height << "\n";
    // 定义输出时间基（统一为输入视频流的时间基，保证同步）
    AVRational output_time_base = fmt_ctx->streams[video_stream_idx]->time_base;

    // ====================== 创建所有线程 ======================
    // 1. 解封装线程
    std::thread demux_th(demux_thread, fmt_ctx, video_stream_idx, audio_stream_idx);

    // 2. 解码线程
    std::thread video_dec_th(video_decode_thread, video_dec_par);
    // std::thread audio_dec_th(audio_decode_thread, audio_dec_par);

    // 3. 编码线程
    std::thread video_enc_th(video_encode_thread, video_dec_par, (AVRational){1, 25});
    // std::thread audio_enc_th(audio_encode_thread, audio_dec_par, output_time_base);

    // 4. 复用线程 - 直接构造MPEG4编码参数


    std::thread mux_th(mux_thread, std::string(output_file), mpeg4_params, audio_dec_par);

    // ====================== 等待线程结束 ======================
    demux_th.join();
    video_dec_th.join();
    // audio_dec_th.join();
    video_enc_th.join();
    // audio_enc_th.join();
    mux_th.join();

    // 释放资源
    verify_output_file(std::string(output_file));
    avformat_close_input(&fmt_ctx);
    avformat_network_deinit();

    return 0;
}