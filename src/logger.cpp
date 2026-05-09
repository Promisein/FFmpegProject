//
// Logger implementation
//
#include "logger.h"

std::mutex Logger::mtx_;
Logger::Level Logger::min_level_ = Logger::INFO;
std::ofstream Logger::log_file_;
bool Logger::file_enabled_ = false;

void Logger::set_log_file(const std::string& path) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (log_file_.is_open()) {
        log_file_.close();
    }
    log_file_.open(path, std::ios::app);
    file_enabled_ = log_file_.is_open();
}

void Logger::log(Level level, const std::string& tag, const std::string& msg) {
    if (level < min_level_) return;

    // 获取当前时间
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    std::tm tm_now;
#if defined(_WIN32) || defined(_WIN64)
    localtime_s(&tm_now, &time_t_now);
#else
    localtime_r(&time_t_now, &tm_now);
#endif

    // 格式化: [HH:MM:SS.mmm] [LEVEL] [TAG] message
    std::ostringstream line;
    line << "[" << std::setfill('0') << std::setw(2) << tm_now.tm_hour
         << ":" << std::setw(2) << tm_now.tm_min
         << ":" << std::setw(2) << tm_now.tm_sec
         << "." << std::setw(3) << ms.count() << "]"
         << " [" << level_str(level) << "]"
         << " " << tag << " " << msg;

    std::string output = line.str();

    std::lock_guard<std::mutex> lock(mtx_);

    // 控制台输出
    if (level >= ERROR) {
        std::cerr << output << std::endl;
    } else {
        std::cout << output << std::endl;
    }

    // 文件输出
    if (file_enabled_ && log_file_.is_open()) {
        log_file_ << output << std::endl;
        log_file_.flush();
    }
}
