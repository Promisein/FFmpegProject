//
// Created by Jianing on 2025/12/26.
//
#include "video_rotate.h"
#include <iostream>

extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/error.h>
}

// 构造函数
VideoRotateProcessor::VideoRotateProcessor() : config{ROTATE_NONE, false} {}

VideoRotateProcessor::VideoRotateProcessor(VideoRotateAngle angle)
        : config{angle, true} {}

// 设置旋转配置
void VideoRotateProcessor::setConfig(const VideoRotateConfig& cfg) {
    config = cfg;
}

// 获取当前配置
VideoRotateConfig VideoRotateProcessor::getConfig() const {
    return config;
}

// 启用旋转
void VideoRotateProcessor::enable(bool enable_flag) {
    config.enable = enable_flag;
}

// 禁用旋转
void VideoRotateProcessor::disable() {
    config.enable = false;
}

// 检查是否需要旋转
bool VideoRotateProcessor::needRotate() const {
    return config.enable && config.angle != ROTATE_NONE;
}

// 获取旋转后的分辨率
void VideoRotateProcessor::getRotatedDimensions(int src_width, int src_height,
                                                int* dst_width, int* dst_height) const {
    if (!needRotate()) {
        *dst_width = src_width;
        *dst_height = src_height;
        return;
    }

    switch (config.angle) {
        case ROTATE_90_CW:
        case ROTATE_270_CW:
            *dst_width = src_height;
            *dst_height = src_width;
            break;
        case ROTATE_180:
        case ROTATE_NONE:
        default:
            *dst_width = src_width;
            *dst_height = src_height;
            break;
    }
}

// 检查帧格式是否支持旋转
bool VideoRotateProcessor::isFormatSupported(int pix_fmt) {
    // 目前只支持YUV420P格式
    return pix_fmt == AV_PIX_FMT_YUV420P;
}

// 静态工具函数：直接旋转帧
AVFrame* VideoRotateProcessor::rotateFrameDirect(AVFrame* src_frame, VideoRotateAngle angle) {
    if (!src_frame || !isFormatSupported(src_frame->format)) {
        return nullptr;
    }

    VideoRotateProcessor processor(angle);
    return processor.rotateFrame(src_frame);
}

// 旋转单个视频帧
AVFrame* VideoRotateProcessor::rotateFrame(AVFrame* src_frame) {
    // 检查输入
    if (!src_frame || !needRotate() || !isFormatSupported(src_frame->format)) {
        return nullptr;
    }

    int width = src_frame->width;
    int height = src_frame->height;

    // 创建目标帧
    AVFrame* dst_frame = av_frame_alloc();
    if (!dst_frame) {
        std::cerr << "[VideoRotate] 无法分配目标帧\n";
        return nullptr;
    }

    // 设置目标帧参数
    dst_frame->format = src_frame->format;
    dst_frame->pts = src_frame->pts;
    dst_frame->pkt_dts = src_frame->pkt_dts;
    dst_frame->pkt_duration = src_frame->pkt_duration;

    // 根据旋转角度设置宽高
    switch (config.angle) {
        case ROTATE_90_CW:
        case ROTATE_270_CW:
            dst_frame->width = height;
            dst_frame->height = width;
            break;
        case ROTATE_180:
        case ROTATE_NONE:
        default:
            dst_frame->width = width;
            dst_frame->height = height;
            break;
    }

    // 分配帧缓冲区
    if (av_frame_get_buffer(dst_frame, 32) < 0) {
        std::cerr << "[VideoRotate] 无法分配帧缓冲区\n";
        av_frame_free(&dst_frame);
        return nullptr;
    }

    // 根据角度执行旋转
    switch (config.angle) {
        case ROTATE_90_CW:
            // 顺时针旋转90度
            for (int y = 0; y < height; y++) {
                for (int x = 0; x < width; x++) {
                    // Y分量
                    dst_frame->data[0][x * dst_frame->linesize[0] + (height - y - 1)] =
                            src_frame->data[0][y * src_frame->linesize[0] + x];
                }
            }
            for (int y = 0; y < height / 2; y++) {
                for (int x = 0; x < width / 2; x++) {
                    // U分量
                    dst_frame->data[1][x * dst_frame->linesize[1] + (height / 2 - y - 1)] =
                            src_frame->data[1][y * src_frame->linesize[1] + x];
                    // V分量
                    dst_frame->data[2][x * dst_frame->linesize[2] + (height / 2 - y - 1)] =
                            src_frame->data[2][y * src_frame->linesize[2] + x];
                }
            }
            break;

        case ROTATE_180:
            // 旋转180度
            for (int y = 0; y < height; y++) {
                for (int x = 0; x < width; x++) {
                    // Y分量
                    dst_frame->data[0][(height - y - 1) * dst_frame->linesize[0] + (width - x - 1)] =
                            src_frame->data[0][y * src_frame->linesize[0] + x];
                }
            }
            for (int y = 0; y < height / 2; y++) {
                for (int x = 0; x < width / 2; x++) {
                    // U分量
                    dst_frame->data[1][(height / 2 - y - 1) * dst_frame->linesize[1] + (width / 2 - x - 1)] =
                            src_frame->data[1][y * src_frame->linesize[1] + x];
                    // V分量
                    dst_frame->data[2][(height / 2 - y - 1) * dst_frame->linesize[2] + (width / 2 - x - 1)] =
                            src_frame->data[2][y * src_frame->linesize[2] + x];
                }
            }
            break;

        case ROTATE_270_CW:
            // 顺时针旋转270度（逆时针90度）
            for (int y = 0; y < height; y++) {
                for (int x = 0; x < width; x++) {
                    // Y分量
                    dst_frame->data[0][(width - x - 1) * dst_frame->linesize[0] + y] =
                            src_frame->data[0][y * src_frame->linesize[0] + x];
                }
            }
            for (int y = 0; y < height / 2; y++) {
                for (int x = 0; x < width / 2; x++) {
                    // U分量
                    dst_frame->data[1][(width / 2 - x - 1) * dst_frame->linesize[1] + y] =
                            src_frame->data[1][y * src_frame->linesize[1] + x];
                    // V分量
                    dst_frame->data[2][(width / 2 - x - 1) * dst_frame->linesize[2] + y] =
                            src_frame->data[2][y * src_frame->linesize[2] + x];
                }
            }
            break;

        case ROTATE_NONE:
        default:
            // 不旋转，直接复制
            av_frame_copy(dst_frame, src_frame);
            av_frame_copy_props(dst_frame, src_frame);
            break;
    }

    return dst_frame;
}
