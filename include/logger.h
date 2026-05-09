//
// 线程安全的结构化日志系统
// Thread-safe structured logging system
//
#ifndef FFMPEGPROJECT_LOGGER_H
#define FFMPEGPROJECT_LOGGER_H

#include <string>
#include <mutex>
#include <fstream>
#include <sstream>
#include <chrono>
#include <iomanip>
#include <iostream>

class Logger {
public:
    enum Level { DEBUG = 0, INFO = 1, WARN = 2, ERROR = 3 };

    static void set_min_level(Level level) { min_level_ = level; }
    static void set_log_file(const std::string& path);

    static void debug(const std::string& tag, const std::string& msg)  { log(DEBUG, tag, msg); }
    static void info(const std::string& tag, const std::string& msg)   { log(INFO,  tag, msg); }
    static void warn(const std::string& tag, const std::string& msg)   { log(WARN,  tag, msg); }
    static void error(const std::string& tag, const std::string& msg)  { log(ERROR, tag, msg); }

private:
    static void log(Level level, const std::string& tag, const std::string& msg);

    static std::mutex mtx_;
    static Level min_level_;
    static std::ofstream log_file_;
    static bool file_enabled_;

    static const char* level_str(Level level) {
        switch (level) {
            case DEBUG: return "DEBUG";
            case INFO:  return "INFO ";
            case WARN:  return "WARN ";
            case ERROR: return "ERROR";
            default:    return "?????";
        }
    }

    static const char* level_console_color(Level level) {
        // Windows 控制台颜色代码 (可选, 仅在 Windows 生效)
        return "";
    }
};

// ====== 便捷宏定义, 自动使用 __FILE__ 作为 tag ======
#define LOG_DEBUG(msg) Logger::debug("[" __FILE__ "]", msg)
#define LOG_INFO(msg)  Logger::info("[" __FILE__ "]", msg)
#define LOG_WARN(msg)  Logger::warn("[" __FILE__ "]", msg)
#define LOG_ERROR(msg) Logger::error("[" __FILE__ "]", msg)

#endif //FFMPEGPROJECT_LOGGER_H
