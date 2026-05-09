//
// ThreadPool implementation
//
#include "thread_pool.h"
#include "logger.h"

ThreadPool::ThreadPool(size_t num_threads) {
    for (size_t i = 0; i < num_threads; i++) {
        workers_.emplace_back(&ThreadPool::worker_loop, this);
    }
    Logger::info("ThreadPool", std::string("创建线程池，工作线程数: ") + std::to_string(num_threads));
}

ThreadPool::~ThreadPool() {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        stop_ = true;
    }
    cv_.notify_all();
    for (auto& w : workers_) {
        if (w.joinable()) w.join();
    }
    Logger::info("ThreadPool", "线程池已销毁");
}

void ThreadPool::submit(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        tasks_.push(std::move(task));
        total_submitted_++;
    }
    cv_.notify_one();
}

void ThreadPool::wait_all() {
    std::unique_lock<std::mutex> lock(mtx_);
    done_cv_.wait(lock, [this]() {
        return tasks_.empty() && active_count_ == 0;
    });
}

size_t ThreadPool::active_count() const {
    return active_count_.load();
}

size_t ThreadPool::total_submitted() const {
    return total_submitted_.load();
}

void ThreadPool::worker_loop() {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mtx_);
            cv_.wait(lock, [this]() { return stop_ || !tasks_.empty(); });

            if (stop_ && tasks_.empty()) {
                return;
            }

            task = std::move(tasks_.front());
            tasks_.pop();
        }

        active_count_++;
        task();
        active_count_--;
        total_completed_++;

        // 通知 wait_all 可能完成了
        done_cv_.notify_one();
    }
}
