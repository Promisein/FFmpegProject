//
// Processing config: rotation + speed change parameters
//
#ifndef FFMPEGPROJECT_CONFIG_H
#define FFMPEGPROJECT_CONFIG_H

#include "video_rotate.h"  // for VideoRotateAngle

// 处理配置
struct ProcessingConfig {
    VideoRotateAngle rotate = ROTATE_NONE;
    double speed_ratio = 1.0;  // 1.0 = normal, 2.0 = 2x fast, 0.5 = half speed
};

#endif // FFMPEGPROJECT_CONFIG_H
