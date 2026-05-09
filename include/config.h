//
// Processing config: rotation + speed change parameters
//
#ifndef FFMPEGPROJECT_CONFIG_H
#define FFMPEGPROJECT_CONFIG_H

#include "video_rotate.h"  // for VideoRotateAngle

// 视频编码器
enum VideoCodec {
    VIDEO_CODEC_MPEG4 = 0,
    VIDEO_CODEC_H264,
};

// 音频编码器
enum AudioCodec {
    AUDIO_CODEC_AC3 = 0,
    AUDIO_CODEC_AAC,
};

// 处理配置
struct ProcessingConfig {
    VideoRotateAngle rotate = ROTATE_NONE;
    double speed_ratio = 1.0;   // 1.0 = normal, 2.0 = 2x fast, 0.5 = half speed
    VideoCodec video_codec = VIDEO_CODEC_MPEG4;  // 视频编码器
    AudioCodec audio_codec = AUDIO_CODEC_AC3;    // 音频编码器
};

#endif // FFMPEGPROJECT_CONFIG_H
