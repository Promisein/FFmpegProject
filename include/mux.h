//
// Created by Jianing on 2025/12/22.
//

#ifndef FFMPEGPROJECT_MUX_H
#define FFMPEGPROJECT_MUX_H

#include <string>
#include "common.h"
struct AVCodecParameters;

// 复用线程（入参：输出文件路径、视频/音频编码参数）
void mux_thread(const std::string& output_file,
                AVCodecParameters* video_enc_par,
                AVCodecParameters* audio_enc_par);


#endif //FFMPEGPROJECT_MUX_H
