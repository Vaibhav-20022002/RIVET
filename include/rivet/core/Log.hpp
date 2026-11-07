#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <sstream>
#include <string_view>

namespace rivet::core {

enum class LogLevel : uint8_t { Silent = 0, Error = 1, Warn = 2, Info = 3, Debug = 4, Trace = 5 };

[[nodiscard]] inline LogLevel parse_log_level(std::string_view level) noexcept {
  auto ieq = [](std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
      char ca = (a[i] >= 'a' && a[i] <= 'z') ? static_cast<char>(a[i] - 32) : a[i];
      char cb = (b[i] >= 'a' && b[i] <= 'z') ? static_cast<char>(b[i] - 32) : b[i];
      if (ca != cb) return false;
    }
    return true;
  };
  if (ieq(level, "SILENT") || ieq(level, "OFF") || ieq(level, "NONE")) return LogLevel::Silent;
  if (ieq(level, "ERROR") || ieq(level, "ERR")) return LogLevel::Error;
  if (ieq(level, "WARN") || ieq(level, "WARNING")) return LogLevel::Warn;
  if (ieq(level, "INFO")) return LogLevel::Info;
  if (ieq(level, "DEBUG")) return LogLevel::Debug;
  if (ieq(level, "TRACE")) return LogLevel::Trace;
  return LogLevel::Info;
}

[[nodiscard]] inline const char *log_level_tag(LogLevel lvl) noexcept {
  switch (lvl) {
    case LogLevel::Error:
      return "ERROR";
    case LogLevel::Warn:
      return "WARN ";
    case LogLevel::Info:
      return "INFO ";
    case LogLevel::Debug:
      return "DEBUG";
    case LogLevel::Trace:
      return "TRACE";
    case LogLevel::Silent:
      return "OFF  ";
  }
  return "INFO ";
}

class Logger {
  inline static std::atomic<LogLevel> global_level_{LogLevel::Warn};

public:
  static void set_level(LogLevel lvl) noexcept {
    global_level_.store(lvl, std::memory_order_relaxed);
  }

  [[nodiscard]] static LogLevel level() noexcept {
    return global_level_.load(std::memory_order_relaxed);
  }

  [[nodiscard]] static bool enabled(LogLevel lvl) noexcept {
    return static_cast<uint8_t>(lvl) <=
        static_cast<uint8_t>(global_level_.load(std::memory_order_relaxed));
  }

  template<typename... Args>
  static void write(LogLevel lvl, std::string_view component, Args &&...args) {
    if (!enabled(lvl)) return;
    std::ostringstream oss;
    oss << "[RIVET][" << log_level_tag(lvl) << "][" << component << "] ";
    (oss << ... << std::forward<Args>(args));
    oss << '\n';
    const std::string line = oss.str();
    std::fwrite(line.data(), 1, line.size(), stderr);
  }
};

} //< namespace rivet::core

#define RIVET_LOG_ERROR(comp, ...)                                                     \
  do {                                                                                 \
    if (::rivet::core::Logger::enabled(::rivet::core::LogLevel::Error)) {              \
      ::rivet::core::Logger::write(::rivet::core::LogLevel::Error, comp, __VA_ARGS__); \
    }                                                                                  \
  } while (0)

#define RIVET_LOG_WARN(comp, ...)                                                     \
  do {                                                                                \
    if (::rivet::core::Logger::enabled(::rivet::core::LogLevel::Warn)) {              \
      ::rivet::core::Logger::write(::rivet::core::LogLevel::Warn, comp, __VA_ARGS__); \
    }                                                                                 \
  } while (0)

#define RIVET_LOG_INFO(comp, ...)                                                     \
  do {                                                                                \
    if (::rivet::core::Logger::enabled(::rivet::core::LogLevel::Info)) {              \
      ::rivet::core::Logger::write(::rivet::core::LogLevel::Info, comp, __VA_ARGS__); \
    }                                                                                 \
  } while (0)

#define RIVET_LOG_DEBUG(comp, ...)                                                     \
  do {                                                                                 \
    if (::rivet::core::Logger::enabled(::rivet::core::LogLevel::Debug)) {              \
      ::rivet::core::Logger::write(::rivet::core::LogLevel::Debug, comp, __VA_ARGS__); \
    }                                                                                  \
  } while (0)
