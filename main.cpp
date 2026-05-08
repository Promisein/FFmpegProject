#include <iostream>
#include <thread>
#include "demux.h"
#include "videodecoder.h"
#include "audiodecoder.h"
#include "videoencoder.h"
#include "audioencoder.h"
#include "mux.h"
#include "pipeline.h"
#include "ffmpeg_raii.h"

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

int main(int argc, char* argv[]) {
    SetConsoleOutputCP(CP_UTF8);

    const char* input_file = "../input3.mp4";
    const char* output_file = "../output.mp4";

    avformat_network_init();

    // 打开输入文件
    AVFormatContext* fmt_ctx_raw = nullptr;
    if (avformat_open_input(&fmt_ctx_raw, input_file, nullptr, nullptr) < 0) {
        std::cerr << "[Error] 打开输入文件失败: " << input_file << "\n";
        return -1;
    }
    InputFormatContextPtr fmt_ctx(fmt_ctx_raw);
    if (avformat_find_stream_info(fmt_ctx.get(), nullptr) < 0) {
        std::cerr << "[Error] 获取媒体流信息失败\n";
        return -1;
    }

    // 查找视频流、音频流索引
    int video_stream_idx = -1, audio_stream_idx = -1;
    for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_idx = i;
        } else if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audio_stream_idx = i;
        }
    }
    if (video_stream_idx == -1 || audio_stream_idx == -1) {
        std::cerr << "[Error] 未找到视频/音频流\n";
        return -1;
    }

    AVCodecParameters* video_dec_par = fmt_ctx->streams[video_stream_idx]->codecpar;
    AVCodecParameters* audio_dec_par = fmt_ctx->streams[audio_stream_idx]->codecpar;

    // 创建MPEG4编码参数
    CodecParametersPtr mpeg4_params(avcodec_parameters_alloc());
    mpeg4_params->codec_type = AVMEDIA_TYPE_VIDEO;
    mpeg4_params->codec_id = AV_CODEC_ID_MPEG4;
    mpeg4_params->codec_tag = 0x7634706d;
    mpeg4_params->width = video_dec_par->width;
    mpeg4_params->height = video_dec_par->height;
    mpeg4_params->format = AV_PIX_FMT_YUV420P;
    mpeg4_params->bit_rate = 1000000;

    std::cout << "[Main] 创建MPEG4编码参数: codec_id=" << mpeg4_params->codec_id
              << ", codec_tag=0x" << std::hex << mpeg4_params->codec_tag << std::dec
              << ", 分辨率=" << mpeg4_params->width << "x" << mpeg4_params->height << "\n";

    // 创建AC3编码参数
    CodecParametersPtr ac3_params(avcodec_parameters_alloc());
    ac3_params->codec_type = AVMEDIA_TYPE_AUDIO;
    ac3_params->codec_id = AV_CODEC_ID_AC3;
    ac3_params->sample_rate = audio_dec_par->sample_rate;
    ac3_params->channels = audio_dec_par->channels;
    ac3_params->channel_layout = av_get_default_channel_layout(audio_dec_par->channels);
    ac3_params->format = AV_SAMPLE_FMT_FLTP;
    ac3_params->bit_rate = 128000;

    std::cout << "[Main] 创建AC3编码参数: codec_id=" << ac3_params->codec_id
              << ", 采样率=" << ac3_params->sample_rate
              << ", 声道数=" << ac3_params->channels << "\n";

    // 获取输出时间基
    AVRational input_frame_rate = fmt_ctx->streams[video_stream_idx]->r_frame_rate;
    if (input_frame_rate.num == 0 || input_frame_rate.den == 0) {
        input_frame_rate = (AVRational){25, 1};
    }
    AVRational output_time_base = av_inv_q(input_frame_rate);
    std::cout << "[Main] 输入视频帧率: " << input_frame_rate.num << "/" << input_frame_rate.den
              << " (" << av_q2d(input_frame_rate) << "fps)\n";
    std::cout << "[Main] 输出时间基: " << output_time_base.num << "/" << output_time_base.den << "\n";

    // ====================== 创建 Pipeline（管理所有队列） ======================
    Pipeline pipeline;

    // ====================== 创建所有线程 ======================
    // 1. 解封装线程
    std::thread demux_th(demux_thread,
                         fmt_ctx.get(), video_stream_idx, audio_stream_idx,
                         std::ref(pipeline.video_pkt_queue),
                         std::ref(pipeline.audio_pkt_queue),
                         &pipeline);

    // 2. 解码线程
    std::thread video_dec_th(video_decode_thread, video_dec_par,
                             std::ref(pipeline.video_pkt_queue),
                             std::ref(pipeline.video_frame_ringbuf),
                             &pipeline);
    std::thread audio_dec_th(audio_decode_thread, audio_dec_par,
                             std::ref(pipeline.audio_pkt_queue),
                             std::ref(pipeline.audio_frame_ringbuf),
                             &pipeline);

    // 3. 编码线程
    std::thread video_enc_th(video_encode_thread, video_dec_par, output_time_base,
                             std::ref(pipeline.video_frame_ringbuf),
                             std::ref(pipeline.en_video_pkt_queue),
                             &pipeline);
    std::thread audio_enc_th(audio_encode_thread, audio_dec_par, output_time_base,
                             std::ref(pipeline.audio_frame_ringbuf),
                             std::ref(pipeline.en_audio_pkt_queue),
                             &pipeline);

    // 4. 复用线程
    std::thread mux_th(mux_thread, std::string(output_file),
                       mpeg4_params.get(), ac3_params.get(), output_time_base,
                       std::ref(pipeline.en_video_pkt_queue),
                       std::ref(pipeline.en_audio_pkt_queue),
                       &pipeline);

    // ====================== 等待线程结束 ======================
    demux_th.join();
    video_dec_th.join();
    audio_dec_th.join();
    video_enc_th.join();
    audio_enc_th.join();
    mux_th.join();

    // 检查是否有错误
    if (pipeline.has_error()) {
        std::cerr << "[Main Error] 转码过程中发生错误: " << pipeline.get_error() << "\n";
        return -1;
    }

    // 验证输出
    verify_output_file(std::string(output_file));

    avformat_network_deinit();
    // RAII: fmt_ctx, mpeg4_params, ac3_params 自动释放

    return 0;
}
