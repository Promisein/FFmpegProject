//
// RAII wrappers for FFmpeg C resources using std::unique_ptr + custom deleters
//
#ifndef FFMPEGPROJECT_FFMPEG_RAII_H
#define FFMPEGPROJECT_FFMPEG_RAII_H

#include <memory>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
}

// --- AVCodecContext ---
struct CodecContextDeleter {
    void operator()(AVCodecContext* ctx) const {
        if (ctx) avcodec_free_context(&ctx);
    }
};
using CodecContextPtr = std::unique_ptr<AVCodecContext, CodecContextDeleter>;

// --- AVFormatContext (input, opened via avformat_open_input) ---
void avformat_close_input_void(AVFormatContext** ctx);
inline void avformat_close_input_void(AVFormatContext** ctx) {
    if (ctx && *ctx) avformat_close_input(ctx);
}

struct InputFormatContextDeleter {
    void operator()(AVFormatContext* ctx) const {
        if (ctx) {
            AVFormatContext* p = ctx;
            avformat_close_input(&p);
        }
    }
};
using InputFormatContextPtr = std::unique_ptr<AVFormatContext, InputFormatContextDeleter>;

// --- AVFrame ---
struct FrameDeleter {
    void operator()(AVFrame* f) const {
        if (f) av_frame_free(&f);
    }
};
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;

// --- AVPacket ---
struct PacketDeleter {
    void operator()(AVPacket* p) const {
        if (p) av_packet_free(&p);
    }
};
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;

// --- AVCodecParameters ---
struct CodecParametersDeleter {
    void operator()(AVCodecParameters* p) const {
        if (p) avcodec_parameters_free(&p);
    }
};
using CodecParametersPtr = std::unique_ptr<AVCodecParameters, CodecParametersDeleter>;

#endif // FFMPEGPROJECT_FFMPEG_RAII_H
