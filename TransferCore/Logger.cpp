#include "Logger.h"

#include <regex>

std::unique_ptr<Logger> Logger::instance;
std::once_flag Logger::initFlag;

std::string Logger::redact(const std::string &message) {
  static const std::regex windowsPath(
      R"(([A-Za-z]:[\\/][^\s'"]+|\\\\[^\s'"]+))");
  static const std::regex sha256Hex(R"(\b[0-9a-fA-F]{64}\b)");
  std::string redacted =
      std::regex_replace(message, windowsPath, "[path-redacted]");
  redacted = std::regex_replace(redacted, sha256Hex, "[sha256-redacted]");
  return redacted;
}

void Logger::Initialize(const std::string &logFile, std::uintmax_t maxSizeBytes,
                        int backupCount, const std::string &flushLevel) {
  std::call_once(initFlag, [&]() {
    instance = std::make_unique<Logger>();
    instance->configureLogger(logFile, maxSizeBytes, backupCount, flushLevel);
  });
}

Logger &Logger::getInstance() {
  if (!instance) {
    throw std::runtime_error("The logger has not been initialized. Please call "
                             "Initialize() before use.");
  }
  return *instance;
}

void Logger::configureLogger(const std::string &logFile,
                             std::uintmax_t maxSizeBytes, int backupCount,
                             const std::string &flushLevel) {
#ifdef _WIN32
#include <windows.h>
  // No início da função configureLogger:
  SetConsoleOutputCP(CP_UTF8);
#endif

  auto consoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
  auto rotatingSink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
      logFile, maxSizeBytes, backupCount);

  std::vector<spdlog::sink_ptr> sinks{consoleSink, rotatingSink};
  spdlogger =
      std::make_shared<spdlog::logger>("logger", sinks.begin(), sinks.end());
  spdlogger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%n] %v");
  spdlogger->set_level(spdlog::level::info); // default
  spdlog::level::level_enum flush_policy =
      spdlog::level::off; // Padrão: nunca fazer flush

  if (flushLevel == "always") {
    flush_policy = spdlog::level::trace; // 'trace' é o nível mais baixo, então
                                         // flushes em tudo
  } else if (flushLevel == "info") {
    flush_policy = spdlog::level::info;
  } else if (flushLevel == "warn") {
    flush_policy = spdlog::level::warn;
  } else if (flushLevel == "error") {
    flush_policy = spdlog::level::err;
  }
  spdlogger->flush_on(flush_policy);
  spdlog::register_logger(spdlogger);
}

void Logger::setMinLevel(Level level) {
  if (!spdlogger)
    return;
  switch (level) {
  case Level::DEBUG:
    spdlogger->set_level(spdlog::level::debug);
    break;
  case Level::INFO:
    spdlogger->set_level(spdlog::level::info);
    break;
  case Level::WARNING:
    spdlogger->set_level(spdlog::level::warn);
    break;
  case Level::ERR:
    spdlogger->set_level(spdlog::level::err);
    break;
  case Level::CRITICAL:
    spdlogger->set_level(spdlog::level::critical);
    break;
  }
}

void Logger::setConsoleOutput(bool enabled) {
  // No spdlog isso é feito ajustando os sinks. Aqui é omitido para
  // simplificação. Poderia remover/adicionar `stdout_sink` dinamicamente.
}

void Logger::enableDatabaseLogging(bool enabled) {
  dbLogging = enabled;
  // Sem efeito prático, mantido por compatibilidade.
}

void Logger::log(Level level, const std::string &module,
                 const std::string &message, int errorCode) {
  if (!spdlogger)
    return;
  std::string fullMessage =
      "[" + module + "]" +
      (errorCode ? " [Code: " + std::to_string(errorCode) + "] " : " ") +
      message;
  fullMessage = redact(fullMessage);
  switch (level) {
  case Level::DEBUG:
    spdlogger->debug(fullMessage);
    break;
  case Level::INFO:
    spdlogger->info(fullMessage);
    break;
  case Level::WARNING:
    spdlogger->warn(fullMessage);
    break;
  case Level::ERR:
    spdlogger->error(fullMessage);
    break;
  case Level::CRITICAL:
    spdlogger->critical(fullMessage);
    break;
  }
}

void Logger::debug(const std::string &module, const std::string &message) {
  log(Level::DEBUG, module, message);
}

void Logger::info(const std::string &module, const std::string &message) {
  log(Level::INFO, module, message);
}

void Logger::warning(const std::string &module, const std::string &message) {
  log(Level::WARNING, module, message);
}

void Logger::error(const std::string &module, const std::string &message,
                   int errorCode) {
  log(Level::ERR, module, message, errorCode);
}

void Logger::critical(const std::string &module, const std::string &message,
                      int errorCode) {
  log(Level::CRITICAL, module, message, errorCode);
}

void Logger::safeDebug(const std::string &module, const std::string &message) {
  getInstance().debug(module, message);
}

void Logger::safeInfo(const std::string &module, const std::string &message) {
  getInstance().info(module, message);
}

void Logger::safeWarning(const std::string &module,
                         const std::string &message) {
  getInstance().warning(module, message);
}

void Logger::safeError(const std::string &module, const std::string &message,
                       int errorCode) {
  getInstance().error(module, message, errorCode);
}
