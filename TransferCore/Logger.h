#pragma once
#ifndef FMT_HEADER_ONLY
#define FMT_HEADER_ONLY
#endif
#include <memory>
#include <mutex>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include <string>

class Logger {
public:
  enum class Level { DEBUG, INFO, WARNING, ERR, CRITICAL };

  static void Initialize(const std::string &logFile,
                         std::uintmax_t maxSizeBytes = 5 * 1024 * 1024,
                         int backupCount = 3,
                         const std::string &flushLevel = "warn");
  static Logger &getInstance();

  void setMinLevel(Level level);
  void setConsoleOutput(bool enabled);
  void enableDatabaseLogging(bool enabled);
  void log(Level level, const std::string &module, const std::string &message,
           int errorCode = 0);
  void debug(const std::string &module, const std::string &message);
  void info(const std::string &module, const std::string &message);
  void warning(const std::string &module, const std::string &message);
  void error(const std::string &module, const std::string &message,
             int errorCode = 0);
  void critical(const std::string &module, const std::string &message,
                int errorCode = 0);

  static void safeDebug(const std::string &module, const std::string &message);
  static void safeInfo(const std::string &module, const std::string &message);
  static void safeWarning(const std::string &module,
                          const std::string &message);
  static void safeError(const std::string &module, const std::string &message,
                        int errorCode = 0);
  Logger() = default;

  template <typename... Args>
  static void error(const std::string &tag, fmt::format_string<Args...> fmt_str,
                    Args &&...args) {
    std::string msg = "[" + tag + "]";
    spdlog::get("logger")->error(
        redact(msg + fmt::format(fmt_str, std::forward<Args>(args)...)));
  }
  template <typename... Args>
  static void warning(const std::string &tag,
                      fmt::format_string<Args...> fmt_str, Args &&...args) {
    std::string msg = "[" + tag + "]";
    spdlog::get("logger")->warn(
        redact(msg + fmt::format(fmt_str, std::forward<Args>(args)...)));
  }
  template <typename... Args>
  static void info(const std::string &tag, fmt::format_string<Args...> fmt_str,
                   Args &&...args) {
    std::string msg = "[" + tag + "]";
    spdlog::get("logger")->info(
        redact(msg + fmt::format(fmt_str, std::forward<Args>(args)...)));
  }

  template <typename... Args>
  static void debug(const std::string &tag, fmt::format_string<Args...> fmt_str,
                    Args &&...args) {
    std::string msg = "[" + tag + "]";
    spdlog::get("logger")->debug(
        redact(msg + fmt::format(fmt_str, std::forward<Args>(args)...)));
  }

  template <typename... Args>
  static void critical(const std::string &tag,
                       fmt::format_string<Args...> fmt_str, Args &&...args) {
    std::string msg = "[" + tag + "]";
    spdlog::get("logger")->critical(
        redact(msg + fmt::format(fmt_str, std::forward<Args>(args)...)));
  }

  template <typename... Args>
  static void errorC(const std::string &tag, const std::string &context,
                     fmt::format_string<Args...> fmt_str, Args &&...args) {
    std::string msg = "[" + tag + "] [file=" + redact(context) + "]";
    spdlog::get("logger")->error(
        redact(msg + fmt::format(fmt_str, std::forward<Args>(args)...)));
  }
  template <typename... Args>
  static void warningC(const std::string &tag, const std::string &context,
                       fmt::format_string<Args...> fmt_str, Args &&...args) {
    std::string msg = "[" + tag + "] [file=" + redact(context) + "]";
    spdlog::get("logger")->warn(
        redact(msg + fmt::format(fmt_str, std::forward<Args>(args)...)));
  }
  template <typename... Args>
  static void infoC(const std::string &tag, const std::string &context,
                    fmt::format_string<Args...> fmt_str, Args &&...args) {
    std::string msg = "[" + tag + "] [file=" + redact(context) + "]";
    spdlog::get("logger")->info(
        redact(msg + fmt::format(fmt_str, std::forward<Args>(args)...)));
  }

  template <typename... Args>
  static void debugC(const std::string &tag, const std::string &context,
                     fmt::format_string<Args...> fmt_str, Args &&...args) {
    std::string msg = "[" + tag + "] [file=" + redact(context) + "]";
    spdlog::get("logger")->debug(
        redact(msg + fmt::format(fmt_str, std::forward<Args>(args)...)));
  }

  template <typename... Args>
  static void criticalC(const std::string &tag, const std::string &context,
                        fmt::format_string<Args...> fmt_str, Args &&...args) {
    std::string msg = "[" + tag + "] [file=" + redact(context) + "]";
    spdlog::get("logger")->critical(
        redact(msg + fmt::format(fmt_str, std::forward<Args>(args)...)));
  }

private:
  static std::string redact(const std::string &message);
  void configureLogger(const std::string &logFile, std::uintmax_t maxSizeBytes,
                       int backupCount, const std::string &flushLevel);

  std::shared_ptr<spdlog::logger> spdlogger;
  bool dbLogging = false;
  static std::unique_ptr<Logger> instance;
  static std::once_flag initFlag;
};

