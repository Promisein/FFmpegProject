//
// Created by Jianing on 2025/12/26.
//

#ifndef FFMPEGPROJECT_VIDEO_ROTATE_H
#define FFMPEGPROJECT_VIDEO_ROTATE_H

extern "C" {
#include <libavutil/frame.h>
}

// 旋转角度枚举
enum VideoRotateAngle {
    ROTATE_NONE = 0,      // 不旋转
    ROTATE_90_CW = 1,     // 顺时针90度
    ROTATE_180 = 2,       // 180度
    ROTATE_270_CW = 3,    // 顺时针270度（或逆时针90度）
};

// 旋转配置结构体
struct VideoRotateConfig {
    VideoRotateAngle angle;   // 旋转角度
    bool enable;             // 是否启用旋转
};

// 视频旋转处理器类
class VideoRotateProcessor {
private:
    VideoRotateConfig config;

public:
    // 构造函数
    VideoRotateProcessor();
    explicit VideoRotateProcessor(VideoRotateAngle angle);

    // 设置旋转配置
    void setConfig(const VideoRotateConfig& cfg);

    // 获取当前配置
    VideoRotateConfig getConfig() const;

    // 启用/禁用旋转
    void enable(bool enable_flag = true);
    void disable();

    // 旋转单个视频帧
    // 返回旋转后的帧（需要调用者释放），如果失败返回nullptr
    AVFrame* rotateFrame(AVFrame* src_frame);

    // 检查是否需要旋转（根据配置）
    bool needRotate() const;

    // 获取旋转后的分辨率（宽高）
    void getRotatedDimensions(int src_width, int src_height, int* dst_width, int* dst_height) const;

    // 静态工具函数：直接旋转帧
    static AVFrame* rotateFrameDirect(AVFrame* src_frame, VideoRotateAngle angle);

    // 静态工具函数：检查帧格式是否支持旋转
    static bool isFormatSupported(int pix_fmt);
};

#endif //FFMPEGPROJECT_VIDEO_ROTATE_H
