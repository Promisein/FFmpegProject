//
// Created by Jianing on 2025/12/24.
//

#ifndef FFMPEGPROJECT_DEEP_COPY_PACKEY_QUEUE_H
#define FFMPEGPROJECT_DEEP_COPY_PACKEY_QUEUE_H

#include <queue>
#include <mutex>
#include <condition_variable>
#include <chrono> // 用于超时等待

extern "C" {
#include <libavcodec/packet.h>
}

//template <typename T>
//class DeepCopyPacketQueue {
//public:
//    std::queue<T*> queue;
//    std::mutex mtx;
//    std::condition_variable cond;
//    bool is_running = true; // 新增：标记队列是否还在运行（用于退出通知）
//
//    // 推送Packet指针（转移所有权，不释放）
//    void push(T* pkt) {
//        std::lock_guard<std::mutex> lock(mtx);
//        if (!is_running) { // 已停止运行，直接释放Packet，避免内存泄漏
//            av_packet_free(&pkt);
//            return;
//        }
//        queue.push(pkt);
//        cond.notify_one();
//    }
//
//    // 取出Packet指针（阻塞/非阻塞，修复核心逻辑）
//    bool pop(T*& pkt, bool block = true, int timeout_ms = 1000) { // 新增超时参数
//        std::unique_lock<std::mutex> lock(mtx);
//        pkt = nullptr; // 初始化pkt为null，避免野指针
//
//        if (block) {
//            // 修复1：带超时的wait + 检查is_running（避免无限阻塞）
//            // 等待条件：队列非空 OR 队列已停止运行
//            bool wait_ok = cond.wait_for(lock, std::chrono::milliseconds(timeout_ms),
//                                         [this]() { return !queue.empty() || !is_running; });
//
//            // 如果等待超时 或 队列已停止且为空 → 返回false
//            if (!wait_ok || (!is_running && queue.empty())) {
//                return false;
//            }
//        } else if (queue.empty()) {
//            return false;
//        }
//
//        // 修复2：双重检查队列是否为空（避免极小窗口的空队列访问）
//        if (queue.empty()) {
//            return false;
//        }
//
//        // 正常取出Packet
//        pkt = queue.front();
//        queue.pop();
//        return true;
//    }
//
//    // 判断队列是否为空
//    bool is_empty() {
//        std::lock_guard<std::mutex> lock(mtx);
//        return queue.empty();
//    }
//
//    // 清空队列并释放所有Packet
//    void flush() {
//        std::lock_guard<std::mutex> lock(mtx);
//        while (!queue.empty()) {
//            T* pkt = queue.front();
//            av_packet_free(&pkt);
//            queue.pop();
//        }
//    }
//
//    // 新增：停止队列运行（通知所有wait的线程退出）
//    void stop() {
//        std::lock_guard<std::mutex> lock(mtx);
//        is_running = false;
//        cond.notify_all(); // 唤醒所有等待的线程，避免死锁
//    }
//
//    // 析构函数：确保资源释放
//    ~DeepCopyPacketQueue() {
//        stop();
//        flush();
//    }
//};

// 专门用于AVPacket的线程安全队列（支持深拷贝）
class DeepCopyPacketQueue {
private:
    std::queue<AVPacket*> queue;  // 存储指针，避免浅拷贝问题
    std::mutex mtx;
    std::condition_variable cond;
    bool done = false;  // 队列结束标志

public:
    ~DeepCopyPacketQueue() {
        clear();
    }

    // 推送Packet（深拷贝）
    void push(const AVPacket& pkt) {
        std::lock_guard<std::mutex> lock(mtx);
        AVPacket* pkt_copy = av_packet_clone(&pkt);
        if (pkt_copy) {
            queue.push(pkt_copy);
            cond.notify_one();
        }
    }

    // 取出Packet
    bool pop(AVPacket& pkt, bool block = true) {
        std::unique_lock<std::mutex> lock(mtx);

        if (block) {
            cond.wait(lock, [this]() { return !queue.empty() || done; });
        }

        if (queue.empty()) {
            return false;
        }

        // 转移packet数据
        av_packet_move_ref(&pkt, queue.front());

        // 释放队列中的packet指针
        av_packet_free(&queue.front());
        queue.pop();

        return true;
    }

    // 标记队列结束
    void mark_done() {
        std::lock_guard<std::mutex> lock(mtx);
        done = true;
        cond.notify_all();
    }

    // 清空队列
    void clear() {
        std::lock_guard<std::mutex> lock(mtx);
        while (!queue.empty()) {
            av_packet_free(&queue.front());
            queue.pop();
        }
    }

    // 判断队列是否为空且已结束
    bool is_empty_and_done() {
        std::lock_guard<std::mutex> lock(mtx);
        return queue.empty() && done;
    }
};

#endif //FFMPEGPROJECT_DEEP_COPY_PACKEY_QUEUE_H