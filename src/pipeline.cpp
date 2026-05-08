//
// Pipeline implementation
//
#include "pipeline.h"

void Pipeline::report_error(const std::string& msg) {
    std::lock_guard<std::mutex> lock(error_mtx_);
    error_msg_ = msg;
    error_flag_.store(true, std::memory_order_release);
}

bool Pipeline::has_error() const {
    return error_flag_.load(std::memory_order_acquire);
}

std::string Pipeline::get_error() const {
    std::lock_guard<std::mutex> lock(error_mtx_);
    return error_msg_;
}
