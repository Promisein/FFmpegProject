//
// Created by Jianing on 2025/12/22.
//

#ifndef FFMPEGPROJECT_PACKET_QUEUE_H
#define FFMPEGPROJECT_PACKET_QUEUE_H

#include <queue>
#include <mutex>
#include <condition_variable>

// 前置声明
extern "C" {
#include <libavcodec/packet.h> // AVPacket的完整定义在这个头文件里
}

// 线程安全Packet队列（声明+实现一体，模板类特性）
template <typename T>
class PacketQueue {
public:
    std::queue<T> queue;
    std::mutex mtx;
    std::condition_variable cond;

    // 推送Packet（加锁）
    void push(const T& pkt) {
        std::lock_guard<std::mutex> lock(mtx);
        queue.push(pkt);
        cond.notify_one();
    }

    // 取出Packet（阻塞/非阻塞）
    bool pop(T& pkt, bool block = true) {
        std::unique_lock<std::mutex> lock(mtx);
        if (block) {
            cond.wait(lock, [this]() { return !queue.empty(); });
        } else if (queue.empty()) {
            return false;
        }
        pkt = queue.front();
        queue.pop();
        return true;
    }

    // 判断队列是否为空
    bool is_empty() {
        std::lock_guard<std::mutex> lock(mtx);
        return queue.empty();
    }
};
#endif //FFMPEGPROJECT_PACKET_QUEUE_H
