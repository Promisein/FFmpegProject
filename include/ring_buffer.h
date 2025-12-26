#ifndef FFMPEGPROJECT_RING_BUFFER_H
#define FFMPEGPROJECT_RING_BUFFER_H

#include <mutex>
#include <condition_variable>
#include <vector>
#include <stdint.h>
#include <iostream>  // 仅保留cout/cerr日志

// FFmpeg核心头文件（仅保留必要部分）
extern "C" {
#include <libavutil/frame.h>
#include <libavcodec/packet.h>
#include <libavutil/error.h>
}

// 环形缓冲区模板类（适配AVFrame*/AVPacket*，修复段错误+内存泄漏）
template <typename T>
class RingBuffer {
private:
    std::vector<T> buffer;          // 缓冲区数组
    uint32_t capacity;              // 最大容量
    uint32_t read_idx = 0;          // 读索引
    uint32_t write_idx = 0;         // 写索引
    uint32_t count = 0;             // 当前元素数
    std::mutex mtx;                 // 互斥锁
    std::condition_variable not_full;  // 生产者等待（非满）
    std::condition_variable not_empty; // 消费者等待（非空）
    bool is_flush = false;          // 刷新/退出标记

    // 判空/判满（私有内联函数）
    bool is_empty() const { return count == 0; }
    bool is_full() const { return count == capacity; }

    // 初始化单个元素（AVFrame*/AVPacket*分配有效内存，避免野指针）
    void init_element(T& elem) {
        if constexpr (std::is_same_v<T, AVFrame*>) {
            elem = av_frame_alloc();
            if (!elem) std::cerr << "[RingBuffer] 错误：av_frame_alloc 分配失败！" << std::endl;
        } else if constexpr (std::is_same_v<T, AVPacket*>) {
            elem = av_packet_alloc();
            if (!elem) std::cerr << "[RingBuffer] 错误：av_packet_alloc 分配失败！" << std::endl;
        } else {
            elem = T(); // 其他类型默认初始化
        }
    }

    // 释放单个元素（彻底释放结构体+数据，修复内存泄漏）
    void free_element(T& elem) {
        if constexpr (std::is_same_v<T, AVFrame*>) {
            if (elem) {
                av_frame_unref(elem);
                av_frame_free(&elem);
                elem = nullptr;
            }
        } else if constexpr (std::is_same_v<T, AVPacket*>) {
            if (elem) {
                av_packet_unref(elem);
                av_packet_free(&elem);
                elem = nullptr;
            }
        }
    }

    // 拷贝元素（带引用计数管理，避免内存泄漏）
    bool copy_element(T& dst, const T& src) {
        if constexpr (std::is_same_v<T, AVFrame*>) {
            if (!dst || !src) {
                std::cerr << "[RingBuffer] 错误：AVFrame* 为空，拷贝失败！" << std::endl;
                return false;
            }
            av_frame_unref(dst); // 先清空旧数据
            int ret = av_frame_ref(dst, src);
            if (ret < 0) {
                char err_buf[1024];
                av_strerror(ret, err_buf, sizeof(err_buf));
                std::cerr << "[RingBuffer] 错误：av_frame_ref 失败：" << err_buf << std::endl;
                return false;
            }
        } else if constexpr (std::is_same_v<T, AVPacket*>) {
            if (!dst || !src) {
                std::cerr << "[RingBuffer] 错误：AVPacket* 为空，拷贝失败！" << std::endl;
                return false;
            }
            av_packet_unref(dst); // 先清空旧数据
            int ret = av_packet_ref(dst, src);
            if (ret < 0) {
                char err_buf[1024];
                av_strerror(ret, err_buf, sizeof(err_buf));
                std::cerr << "[RingBuffer] 错误：av_packet_ref 失败：" << err_buf << std::endl;
                return false;
            }
        } else {
            dst = src; // 其他类型直接赋值
        }
        return true;
    }

public:
    // 构造函数：初始化所有元素为有效指针（核心修复段错误）
    explicit RingBuffer(uint32_t cap = 30)  // 容量默认30（按需调整，避免内存浪费）
            : capacity(cap) {
        buffer.resize(capacity);
        // 初始化每个元素为有效AVFrame*/AVPacket*
        for (auto& elem : buffer) {
            init_element(elem);
        }
    }

    // 推送元素（阻塞，直到缓冲区非满/收到退出信号）
    bool push(const T& data) {
        std::unique_lock<std::mutex> lock(mtx);

        // 等待缓冲区非满
        not_full.wait(lock, [this]() { return !is_full() || is_flush; });

        if (is_flush) {
            std::cout << "[RingBuffer] 提示：收到刷新信号，停止push！" << std::endl;
            return false;
        }

        // 拷贝数据到缓冲区
        if (!copy_element(buffer[write_idx], data)) {
            return false;
        }

        write_idx = (write_idx + 1) % capacity;
        count++;
        not_empty.notify_one(); // 通知消费者有数据
        return true;
    }

    // 取出元素（阻塞，直到缓冲区非空/收到退出信号）
    bool pop(T& data) {
        std::unique_lock<std::mutex> lock(mtx);

        // 等待缓冲区非空
        not_empty.wait(lock, [this]() { return !is_empty() || is_flush; });

        if (is_empty() && is_flush)
        {
            std::cout << "[RingBuffer] 提示：缓冲区空且收到刷新信号，停止pop！" << std::endl;
            return false;
        }

        // 拷贝缓冲区数据到输出
        if (!copy_element(data, buffer[read_idx])) {
            return false;
        }

        // 清空缓冲区当前元素的旧数据（复用结构体）
        if constexpr (std::is_same_v<T, AVFrame*>) {
            av_frame_unref(buffer[read_idx]);
        } else if constexpr (std::is_same_v<T, AVPacket*>) {
            av_packet_unref(buffer[read_idx]);
        }

        read_idx = (read_idx + 1) % capacity;
        count--;
        not_full.notify_one(); // 通知生产者有空位
        return true;
    }

    // 发送刷新信号（通知所有线程退出）
    void flush() {
        std::unique_lock<std::mutex> lock(mtx);
        is_flush = true;
        not_full.notify_all();
        not_empty.notify_all();
        std::cout << "[RingBuffer] 提示：发送刷新信号，唤醒所有等待线程！" << std::endl;
    }

    // 重置缓冲区（清空数据+重新初始化）
    void reset() {
        std::unique_lock<std::mutex> lock(mtx);
        // 释放并重新初始化所有元素
        for (auto& elem : buffer) {
            free_element(elem);
            init_element(elem);
        }
        read_idx = 0;
        write_idx = 0;
        count = 0;
        is_flush = false;
        std::cout << "[RingBuffer] 提示：缓冲区已重置！" << std::endl;
    }

    // 析构函数：彻底释放所有内存（修复内存泄漏）
    ~RingBuffer() {
        std::unique_lock<std::mutex> lock(mtx);
        for (auto& elem : buffer) {
            free_element(elem);
        }
        buffer.clear();
        std::cout << "[RingBuffer] 提示：缓冲区已析构，内存释放完成！" << std::endl;
    }

    // 获取当前元素数量（调试用）
    uint32_t size() const {
        std::unique_lock<std::mutex> lock(mtx);
        return count;
    }

    // 获取缓冲区容量（调试用）
    uint32_t get_capacity() const {
        return capacity;
    }
};

// 全局环形缓冲区声明（按需调整类型/容量）
extern RingBuffer<AVFrame*> g_video_frame_ringbuf;
extern RingBuffer<AVFrame*> g_audio_frame_ringbuf;

#endif //FFMPEGPROJECT_RING_BUFFER_H