//
// Created by Jianing on 2025/12/22.
//

#ifndef FFMPEGPROJECT_RING_BUFFER_H
#define FFMPEGPROJECT_RING_BUFFER_H

#include <mutex>
#include <condition_variable>
#include <vector>
#include <stdint.h>

// FFmpeg完整头文件（必须，AVFrame需要完整定义）
extern "C" {
#include <libavutil/frame.h>
#include "libavcodec/packet.h"
}

// 环形缓冲区模板类（适配AVFrame）
template <typename T>
class RingBuffer {
private:
    std::vector<T> buffer;    // 缓冲区数组
    uint32_t capacity;        // 缓冲区最大容量
    uint32_t read_idx;        // 读索引
    uint32_t write_idx;       // 写索引
    uint32_t count;           // 当前元素数量
    std::mutex mtx;           // 互斥锁
    std::condition_variable not_full;  // 非满条件（生产者等待）
    std::condition_variable not_empty; // 非空条件（消费者等待）
    bool is_flush;            // 刷新标记（结束信号）

    // 判空/判满
    bool is_empty() const { return count == 0; }
    bool is_full() const { return count == capacity; }

public:
    // 构造函数：指定缓冲区容量
    explicit RingBuffer(uint32_t cap = 30)
            : capacity(cap), read_idx(0), write_idx(0), count(0), is_flush(false) {
        buffer.resize(capacity);
    }

    // 推送元素（阻塞，直到缓冲区非满）
    bool push(const T& frame) {
        std::unique_lock<std::mutex> lock(mtx);

        // 等待缓冲区非满（除非收到刷新信号）
        not_full.wait(lock, [this]() { return !is_full() || is_flush; });

        if (is_flush) return false; // 刷新信号：停止推送

        // 拷贝Frame（AVFrame需引用计数）
        if constexpr (std::is_same_v<T, AVFrame*>) {
            av_frame_ref(buffer[write_idx], frame);
        } else {
            buffer[write_idx] = frame;
        }

        write_idx = (write_idx + 1) % capacity;
        count++;
        not_empty.notify_one(); // 通知消费者：有数据
        return true;
    }

    // 取出元素（阻塞，直到缓冲区非空）
    bool pop(T& frame) {
        std::unique_lock<std::mutex> lock(mtx);

        // 等待缓冲区非空（除非收到刷新信号）
        not_empty.wait(lock, [this]() { return !is_empty() || is_flush; });

        if (is_empty() && is_flush) return false; // 无数据且刷新：退出

        // 拷贝Frame（AVFrame需引用计数）
        if constexpr (std::is_same_v<T, AVFrame*>) {
            av_frame_ref(frame, buffer[read_idx]);
            av_frame_unref(buffer[read_idx]); // 释放缓冲区中的Frame
        } else {
            frame = buffer[read_idx];
        }

        read_idx = (read_idx + 1) % capacity;
        count--;
        not_full.notify_one(); // 通知生产者：有空位
        return true;
    }

    // 发送刷新信号（通知所有线程退出）
    void flush() {
        std::unique_lock<std::mutex> lock(mtx);
        is_flush = true;
        not_full.notify_all();
        not_empty.notify_all();
    }

    // 重置缓冲区
    void reset() {
        std::unique_lock<std::mutex> lock(mtx);
        for (auto& elem : buffer) {
            if constexpr (std::is_same_v<T, AVFrame*>) {
                av_frame_unref(elem);
            }
        }
        read_idx = 0;
        write_idx = 0;
        count = 0;
        is_flush = false;
    }

    ~RingBuffer() {
        reset();
    }
};

// 全局环形缓冲区声明（视频/音频Frame）
extern RingBuffer<AVFrame*> g_video_frame_ringbuf;
extern RingBuffer<AVFrame*> g_audio_frame_ringbuf;

// 编码后Packet队列（供mux线程消费）
extern RingBuffer<AVPacket*> g_video_pkt_ringbuf;
extern RingBuffer<AVPacket*> g_audio_pkt_ringbuf;

#endif //FFMPEGPROJECT_RING_BUFFER_H
