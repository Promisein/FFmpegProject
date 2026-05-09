#include <iostream>
#include <thread>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <sstream>
#include <filesystem>
#include "demux.h"
#include "videodecoder.h"
#include "audiodecoder.h"
#include "videoencoder.h"
#include "audioencoder.h"
#include "mux.h"
#include "pipeline.h"
#include "ffmpeg_raii.h"
#include "config.h"
#include "thread_pool.h"
#include "logger.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
}
#include <windows.h>

namespace fs = std::filesystem;

// ====================== 单文件转码任务 ======================
struct TranscodeResult {
    std::string filename;
    bool success;
    std::string error;
    int video_frames = 0;
    int audio_frames = 0;
};

TranscodeResult transcode_single(const std::string& input_path,
                                  const std::string& output_dir,
                                  const ProcessingConfig& config) {
    TranscodeResult result;
    result.filename = input_path;

    std::string output_file = output_dir + "/output.mp4";

    Logger::info("main", std::string("开始转码: ") + input_path + " -> " + output_file);

    // 打开输入文件
    AVFormatContext* fmt_ctx_raw = nullptr;
    if (avformat_open_input(&fmt_ctx_raw, input_path.c_str(), nullptr, nullptr) < 0) {
        result.success = false;
        result.error = "打开输入文件失败";
        return result;
    }
    InputFormatContextPtr fmt_ctx(fmt_ctx_raw);
    if (avformat_find_stream_info(fmt_ctx.get(), nullptr) < 0) {
        result.success = false;
        result.error = "获取媒体流信息失败";
        return result;
    }

    // 查找流索引
    int video_stream_idx = -1, audio_stream_idx = -1;
    for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
            video_stream_idx = i;
        else if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
            audio_stream_idx = i;
    }
    if (video_stream_idx == -1 || audio_stream_idx == -1) {
        result.success = false;
        result.error = "未找到视频/音频流";
        return result;
    }

    AVCodecParameters* video_dec_par = fmt_ctx->streams[video_stream_idx]->codecpar;
    AVCodecParameters* audio_dec_par = fmt_ctx->streams[audio_stream_idx]->codecpar;

    // 确定输出分辨率
    int out_width = video_dec_par->width;
    int out_height = video_dec_par->height;
    if (config.rotate == ROTATE_90_CW || config.rotate == ROTATE_270_CW) {
        std::swap(out_width, out_height);
    }

    // MPEG4 编码参数
    CodecParametersPtr mpeg4_params(avcodec_parameters_alloc());
    mpeg4_params->codec_type = AVMEDIA_TYPE_VIDEO;
    mpeg4_params->codec_id = AV_CODEC_ID_MPEG4;
    mpeg4_params->codec_tag = 0x7634706d;
    mpeg4_params->width = out_width;
    mpeg4_params->height = out_height;
    mpeg4_params->format = AV_PIX_FMT_YUV420P;
    mpeg4_params->bit_rate = 1000000;

    // AC3 编码参数
    CodecParametersPtr ac3_params(avcodec_parameters_alloc());
    ac3_params->codec_type = AVMEDIA_TYPE_AUDIO;
    ac3_params->codec_id = AV_CODEC_ID_AC3;
    ac3_params->sample_rate = audio_dec_par->sample_rate;
    ac3_params->channels = audio_dec_par->channels;
    ac3_params->channel_layout = av_get_default_channel_layout(audio_dec_par->channels);
    ac3_params->format = AV_SAMPLE_FMT_FLTP;
    ac3_params->bit_rate = 128000;

    // 输出时间基
    AVRational input_frame_rate = fmt_ctx->streams[video_stream_idx]->r_frame_rate;
    if (input_frame_rate.num == 0 || input_frame_rate.den == 0) {
        input_frame_rate = (AVRational){25, 1};
    }
    AVRational output_time_base = av_inv_q(input_frame_rate);

    // 创建独立的 Pipeline（每个任务有自己的一套队列）
    Pipeline pipeline;

    // 启动 6 个转码线程
    std::thread demux_th(demux_thread,
                         fmt_ctx.get(), video_stream_idx, audio_stream_idx,
                         std::ref(pipeline.video_pkt_queue),
                         std::ref(pipeline.audio_pkt_queue),
                         &pipeline);

    std::thread video_dec_th(video_decode_thread, video_dec_par,
                             std::ref(pipeline.video_pkt_queue),
                             std::ref(pipeline.video_frame_ringbuf),
                             &pipeline);
    std::thread audio_dec_th(audio_decode_thread, audio_dec_par,
                             std::ref(pipeline.audio_pkt_queue),
                             std::ref(pipeline.audio_frame_ringbuf),
                             &pipeline);

    std::thread video_enc_th(video_encode_thread, video_dec_par, output_time_base,
                             std::ref(pipeline.video_frame_ringbuf),
                             std::ref(pipeline.en_video_pkt_queue),
                             config, &pipeline);
    std::thread audio_enc_th(audio_encode_thread, audio_dec_par, output_time_base,
                             std::ref(pipeline.audio_frame_ringbuf),
                             std::ref(pipeline.en_audio_pkt_queue),
                             config, &pipeline);

    std::thread mux_th(mux_thread, output_file,
                       mpeg4_params.get(), ac3_params.get(), output_time_base,
                       std::ref(pipeline.en_video_pkt_queue),
                       std::ref(pipeline.en_audio_pkt_queue),
                       config, &pipeline);

    // 等待当前任务的所有线程完成
    demux_th.join();
    video_dec_th.join();
    audio_dec_th.join();
    video_enc_th.join();
    audio_enc_th.join();
    mux_th.join();

    if (pipeline.has_error()) {
        result.success = false;
        result.error = pipeline.get_error();
        return result;
    }

    result.success = true;
    Logger::info("main", std::string("完成: ") + input_path);
    return result;
}

// ====================== 打印用法 ======================
void print_usage(const char* prog) {
    std::ostringstream usage;
    usage << "用法: " << prog << " [-rotate <angle>] [-speed <ratio>] [-threads <n>]\n"
          << "  -rotate <angle>  视频旋转: 90, 180, 270\n"
          << "  -speed  <ratio>  播放速度: 0.5, 0.75, 1.25, 1.5, 2, 4\n"
          << "  -threads <n>     线程池并发数（默认 3）\n"
          << "\n"
          << "自动扫描 ../input/*.mp4，输出到 ../output/<文件名>/\n"
          << "示例: " << prog << " -rotate 90 -speed 1.5 -threads 4";
    Logger::info("usage", usage.str());
}

// ====================== 主函数 ======================
int main(int argc, char* argv[]) {
    SetConsoleOutputCP(CP_UTF8);

    // 解析 CLI
    ProcessingConfig config;
    int pool_size = 3;  // 默认并发 3 个任务

    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "-rotate") == 0 && i + 1 < argc) {
            int angle = std::stoi(argv[++i]);
            if (angle == 90)       config.rotate = ROTATE_90_CW;
            else if (angle == 180) config.rotate = ROTATE_180;
            else if (angle == 270) config.rotate = ROTATE_270_CW;
            else {
                Logger::error("main", std::string("不支持的旋转角度: ") + std::to_string(angle));
                return 1;
            }
        } else if (std::strcmp(argv[i], "-speed") == 0 && i + 1 < argc) {
            config.speed_ratio = std::stod(argv[++i]);
            if (config.speed_ratio <= 0) {
                Logger::error("main", "倍速值必须 > 0");
                return 1;
            }
        } else if (std::strcmp(argv[i], "-threads") == 0 && i + 1 < argc) {
            pool_size = std::stoi(argv[++i]);
            if (pool_size < 1) pool_size = 1;
        } else if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            Logger::error("main", std::string("未知参数: ") + argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    Logger::info("Main", std::string("配置: 旋转=") + std::to_string(config.rotate)
                 + "度, 倍速=" + std::to_string(config.speed_ratio)
                 + "x, 线程池=" + std::to_string(pool_size));

    avformat_network_init();

    // 扫描 input/ 目录
    std::string input_dir = "../input";
    std::string output_base = "../output";
    std::vector<std::string> input_files;

    try {
        for (const auto& entry : fs::directory_iterator(input_dir)) {
            if (!entry.is_regular_file()) continue;
            std::string ext = entry.path().extension().string();
            if (ext == ".mp4" || ext == ".MP4") {
                input_files.push_back(entry.path().string());
            }
        }
    } catch (const fs::filesystem_error& e) {
        Logger::error("main", std::string("扫描输入目录失败: ") + e.what());
        return -1;
    }

    // 排序保证处理顺序一致
    std::sort(input_files.begin(), input_files.end());

    if (input_files.empty()) {
        Logger::error("main", "在 " + input_dir + " 未找到 .mp4 文件");
        return -1;
    }

    Logger::info("Main", "扫描到 " + std::to_string(input_files.size()) + " 个视频文件");

    // 限制线程池大小不超过文件数
    if (pool_size > static_cast<int>(input_files.size())) {
        pool_size = static_cast<int>(input_files.size());
    }

    // 创建线程池
    ThreadPool pool(pool_size);

    // 存储结果
    std::vector<TranscodeResult> results(input_files.size());

    // 对每个输入文件提交任务
    for (size_t i = 0; i < input_files.size(); i++) {
        std::string filepath = input_files[i];

        // 提取文件名（不含扩展名）
        fs::path p(filepath);
        std::string stem = p.stem().string();

        // 创建输出子目录
        std::string output_dir = output_base + "/" + stem;
        try {
            fs::create_directories(output_dir);
        } catch (const fs::filesystem_error& e) {
            Logger::error("main", "创建输出目录失败: " + output_dir + " - " + e.what());
            results[i].success = false;
            results[i].error = e.what();
            continue;
        }

        pool.submit([filepath, output_dir, config, &results, i]() {
            results[i] = transcode_single(filepath, output_dir, config);
        });
    }

    // 等待所有任务完成
    Logger::info("Main", "已提交 " + std::to_string(pool.total_submitted()) + " 个任务，等待完成...");
    pool.wait_all();
    Logger::info("Main", "所有任务已完成");

    // 打印结果汇总
    int success_count = 0, fail_count = 0;
    Logger::info("Main", "========== 转码结果汇总 ==========");
    for (size_t i = 0; i < results.size(); i++) {
        const auto& r = results[i];
        if (r.success) {
            Logger::info("Main", "  [OK]    " + r.filename);
            success_count++;
        } else {
            Logger::error("Main", "  [FAIL]  " + r.filename + " - " + r.error);
            fail_count++;
        }
    }
    Logger::info("Main", "成功: " + std::to_string(success_count) + ", 失败: "
                 + std::to_string(fail_count) + ", 总计: " + std::to_string(results.size()));

    avformat_network_deinit();
    return fail_count > 0 ? 1 : 0;
}
