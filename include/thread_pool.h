//
// 自定义固定大小线程池
//
#ifndef FFMPEGPROJECT_THREAD_POOL_H
#define FFMPEGPROJECT_THREAD_POOL_H

#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <atomic>
#include <memory>

class ThreadPool {
public:
    explicit ThreadPool(size_t num_threads);
    ~ThreadPool();

    // 提交任务（非阻塞）
    void submit(std::function<void()> task);

    // 等待所有已提交任务完成
    void wait_all();

    // 获取正在执行的任务数
    size_t active_count() const;

    // 获取总任务数（已提交）
    size_t total_submitted() const;

private:
    void worker_loop();

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    mutable std::mutex mtx_;
    std::condition_variable cv_;       // 通知 worker 有新任务
    std::condition_variable done_cv_;  // 通知 wait_all 所有任务完成
    bool stop_ = false;
    std::atomic<size_t> active_count_{0};
    std::atomic<size_t> total_submitted_{0};
    std::atomic<size_t> total_completed_{0};
};

#endif // FFMPEGPROJECT_THREAD_POOL_H
