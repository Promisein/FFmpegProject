//
// Created by Jianing on 2025/12/22.
//
#include "audiodecoder.h"
#include "ring_buffer.h"
#include <iostream>
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
}

void audio_decode_thread(AVCodecParameters* codec_par) {
    std::cout << "start audioDecode!\n";
    const AVCodec* codec = avcodec_find_decoder(codec_par->codec_id);
    if (!codec) {
        std::cerr << "[Error] 找不到音频解码器" << std::endl;
        return;
    }

    AVCodecContext* codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) {
        std::cerr << "[Error] 分配音频解码器上下文失败" << std::endl;
        return;
    }
    if (avcodec_parameters_to_context(codec_ctx, codec_par) < 0) {
        std::cerr << "[Error] 复制音频流参数失败" << std::endl;
        avcodec_free_context(&codec_ctx);
        return;
    }

    if (avcodec_open2(codec_ctx, codec, nullptr) < 0) {
        std::cerr << "[Error] 打开音频解码器失败" << std::endl;
        avcodec_free_context(&codec_ctx);
        return;
    }

    std::cout << "[Audio Decode Info] 音频解码器打开成功（解码器名称：" << codec->name << "）" << std::endl;

    // 3. 初始化资源（必须！修复原代码未初始化问题）
    AVPacket pkt = {0};          // AVPacket零初始化（避免野指针）
    AVFrame* frame = av_frame_alloc(); // 分配解码后的Frame

    if (!frame) {
        std::cerr << "[Audio Decode Error] 分配AVFrame失败！" << std::endl;
        avcodec_free_context(&codec_ctx);
        return;
    }

    // 4. 核心逻辑：从g_audio_pkt_queue取出Packet → 解码 → 推Frame到环形缓冲区
    int pkt_count = 0; // 统计处理的Packet数量
    while (true) {
        // 从PacketQueue取出待解码的音频Packet（阻塞等待）
        if (!g_audio_pkt_queue.pop(pkt)) {
            std::cout << "[Audio Decode Info] 音频Packet队列已空/退出，停止解码" << std::endl;
            break;
        }
        pkt_count++;

        // 空Packet：表示解封装线程已结束，刷新解码器剩余数据
        if (!pkt.data) {
            std::cout << "[Audio Decode Info] 收到结束Packet，刷新解码器" << std::endl;
            avcodec_send_packet(codec_ctx, nullptr); // 发送空Packet刷新解码器
            break;
        }

        std::cout << "[Audio Decode Info] 处理第" << pkt_count << "个音频Packet（大小：" << pkt.size << "）" << std::endl;

        // 5. 将Packet发送到解码器
        int ret = avcodec_send_packet(codec_ctx, &pkt);
        if (ret < 0) {
            std::cerr << "[Audio Decode Warn] 音频Packet发送失败（错误码：" << ret << "），跳过该Packet" << std::endl;
            av_packet_unref(&pkt); // 释放当前Packet引用
            continue;
        }

        // 6. 循环接收解码后的Frame
        while (true) {
            ret = avcodec_receive_frame(codec_ctx, frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break; // 无更多Frame，退出循环
            } else if (ret < 0) {
                std::cerr << "[Audio Decode Error] 解码音频Frame失败（错误码：" << ret << "）" << std::endl;
                break;
            }

            // 检查解码后的Frame是否有效
            if (!frame->data[0]) {
                std::cerr << "[Audio Decode Warn] 解码出空的音频Frame，跳过" << std::endl;
                av_frame_unref(frame);
                continue;
            }

            // 7. 将解码后的Frame推入环形缓冲区（g_audio_frame_ringbuf）
            std::cout << "[Audio Decode Info] 解码PCM帧成功（PTS：" << frame->pts << "），推入环形缓冲区" << std::endl;
            g_audio_frame_ringbuf.push(frame); // 推Frame到环形缓冲区

            // 释放当前Frame的引用（环形缓冲区已通过av_frame_ref复制，此处必须unref）
            av_frame_unref(frame);
        }

        // 释放当前Packet的引用（避免内存泄漏）
        av_packet_unref(&pkt);
    }

    // 8. 处理解码器剩余的Frame（刷新后的数据）
    while (true) {
        int ret = avcodec_receive_frame(codec_ctx, frame);
        if (ret < 0) break;

        if (frame->data[0]) {
            std::cout << "[Audio Decode Info] 刷新解码器，获取剩余PCM帧（PTS：" << frame->pts << "），推入环形缓冲区" << std::endl;
            g_audio_frame_ringbuf.push(frame);
            av_frame_unref(frame);
        }
    }

    // 9. 解码结束：发送刷新信号给编码线程
    std::cout << "[Audio Decode Info] 音频解码完成，共处理" << pkt_count << "个Packet，发送刷新信号" << std::endl;
    g_audio_frame_ringbuf.flush();

    // 10. 释放所有资源（避免内存泄漏）
    av_frame_free(&frame);       // 释放Frame
    avcodec_free_context(&codec_ctx); // 释放解码器上下文
    std::cout << "===== 音频解码线程退出 =====" << std::endl;
}


