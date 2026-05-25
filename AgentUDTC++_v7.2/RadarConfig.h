#pragma once
#include <Windows.h>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
/**
 * @class RadarConfig
 * @brief Singleton class that manages the configuration settings for the radar
 * agent.
 *
 * This class reads key-value pairs from a configuration file and exposes the
 * settings via getter methods. It provides default values and validates loaded
 * configurations.
 */
class RadarConfig {
public:
  // Disable copy and move construction/assignment.
  RadarConfig(const RadarConfig &) = delete;
  RadarConfig &operator=(const RadarConfig &) = delete;
  RadarConfig(RadarConfig &&) = delete;
  RadarConfig &operator=(RadarConfig &&) = delete;

  /**
   * @brief Accessor for the singleton instance.
   * @return Reference to the singleton instance of RadarConfig.
   */
  static RadarConfig &getInstance();
  struct ServerEndpoint {
    std::string host;
    int port;
  };

  const std::vector<ServerEndpoint> &getServerEndpoints() const {
    return serverEndpoints;
  }
  /// @brief Returns the configured data directory.
  std::vector<std::string> getDataDirs() const { return dataDirs; }
  /// @brief Returns the configured chunk size.
  DWORDLONG getChunkSize() const { return chunkSize; }
  bool isAdaptiveChunkEnabled() const { return adaptiveChunkEnabled; }
  DWORDLONG getAdaptiveChunkMinBytes() const { return adaptiveChunkMinBytes; }
  DWORDLONG getAdaptiveChunkMaxBytes() const { return adaptiveChunkMaxBytes; }
  DWORDLONG getAdaptiveChunkInitialBytes() const {
    return adaptiveChunkInitialBytes;
  }
  int getAdaptiveChunkTargetAckMillis() const {
    return adaptiveChunkTargetAckMillis;
  }
  /// @brief Returns the configured maximum retries.
  int getMaxRetries() const { return maxRetries; }
  /// @brief Returns the configured maximum bandwidth.
  int64_t getMaxBandwidth() const { return maxBandwidth; }
  /// @brief Returns the configured segment size.
  int getSegmentSize() const { return segmentSize; }
  /// @brief Returns the configured send buffer size.
  DWORDLONG getSendBuffer() const { return sendBuffer; }
  /// @brief Returns the configured receive buffer size.
  DWORDLONG getReceiveBuffer() const { return receiveBuffer; }
  /// @brief Returns the configured send timeout.
  int getSendTimeout() const { return sendTimeout; }
  /// @brief Returns the configured receive timeout.
  int getReceiveTimeout() const { return receiveTimeout; }
  /// @brief Returns the configured number of worker threads.
  int getNumThreads() const { return numThreads; }
  /// @brief Returns the configured stability check interval.
  int getStabilityCheckInterval() const { return stabilityCheckInterval; }
  /// @brief Returns the configured stability check count.
  int getStabilityCheckCount() const { return stabilityCheckCount; }
  /// @brief Returns the configured watcher check interval.
  int getWatcherCheckInterval() const { return watcherCheckInterval; }
  /// @brief Returns the configured pending check interval.
  int getPendingCheckInterval() const { return pendingCheckInterval; }
  /// @brief Returns the configured callback failure threshold.
  int getCBFailureThreshold() const { return cbFailureThreshold; }
  /// @brief Returns the configured callback reset timeout.
  int getCBResetTimeoutSeconds() const { return cbResetTimeoutSeconds; }
  /// @brief Returns the configured memory usage percentage limit.
  int getMemoryUsagePercentLimit() const { return memoryUsagePercentLimit; }
  /// @brief Returns the configured log file path.
  std::string getLogFilePath() const { return logFilePath; }
  /// @brief Returns the configured database file path.
  std::string getDBFilePath() const { return dbFilePath; }
  /// @brief Return the configured log file max size in MB.
  int getLogMaxSizeMB() const { return logMaxSizeMB; }
  /// @brief Return the configured log file backup count.
  int getLogBackupCount() const { return logBackupCount; }

  /// @brief Returns file names excluded by FileWatcher.
  const std::vector<std::string> &getWatcherExcludeFiles() const {
    return watcherExcludeFiles;
  }

  /// @brief Returns the configured log flush level.
  std::string getLogFlushLevel() const { return logFlushLevel; }

  /// @brief Returns configured UDT connection pool size.
  int getPoolSize() const { return poolSize; }

  /// @brief Returns max cumulative retries before abandoning a chunk.
  int getMaxRetryAbandoned() const { return maxRetryAbandon; }

  /// @brief Returns max attempts to process a failed file.
  int getMaxFileProcessingAttempts() const { return maxFileProcessingAttempts; }

  /// @brief Returns max random jitter added to retry delay in milliseconds.
  int getRetryJitterMaxMillis() const { return retryJitterMaxMillis; }

  /// @brief Returns whether initial UDT greeting validation is required.
  bool isUDTGreetingRequired() const { return udtRequireGreeting; }

  /// @brief Returns expected greeting payload when greeting is enabled.
  const std::string &getUDTExpectedGreeting() const {
    return udtExpectedGreeting;
  }

  /// @brief Returns timeout for greeting exchange in milliseconds.
  int getUDTGreetingTimeoutMs() const { return udtGreetingTimeoutMs; }

  /// @brief Returns whether pool keep-alive probes are enabled.
  bool isUDTKeepAliveEnabled() const { return udtKeepAliveEnabled; }

  /// @brief Returns keep-alive interval in seconds.
  int getUDTKeepAliveIntervalSeconds() const {
    return udtKeepAliveIntervalSeconds;
  }

  /// @brief Returns payload string used for keep-alive probes.
  const std::string &getUDTKeepAlivePayload() const {
    return udtKeepAlivePayload;
  }

  /// @brief Returns whether encrypted/authenticated packet transfer is enabled.
  bool isSecurityEnabled() const { return securityEnabled; }

  /// @brief Returns whether PSK challenge-response handshake is enabled.
  bool isSecurityHandshakeEnabled() const { return securityHandshakeEnabled; }

  /// @brief Returns the pre-shared key used by the secure packet envelope.
  const std::string &getSecurityPreSharedKey() const {
    return securityPreSharedKey;
  }
  const std::string &getSecurityClientId() const { return securityClientId; }
  const std::string &getSecurityIdentityMode() const {
    return securityIdentityMode;
  }
  bool isSignedIdentityMode() const {
    return securityIdentityMode == "signed_handshake";
  }
  const std::string &getSecurityClientPrivateKeyPath() const {
    return securityClientPrivateKeyPath;
  }
  const std::string &getSecurityServerPublicKeyPath() const {
    return securityServerPublicKeyPath;
  }
  bool isDashboardEnabled() const { return dashboardEnabled; }
  const std::string &getDashboardUrl() const { return dashboardUrl; }
  int getDashboardHeartbeatIntervalSeconds() const {
    return dashboardHeartbeatIntervalSeconds;
  }
  int getDashboardLogTailLines() const { return dashboardLogTailLines; }
  bool isChangedFilesResendEnabled() const {
    return changedFilesResendEnabled;
  }
  const std::string &getChangedFilesIdentity() const {
    return changedFilesIdentity;
  }

private:
  /// Private constructor.
  RadarConfig();
  /// Virtual destructor.
  virtual ~RadarConfig();

  // Default constants (timeout defaults adjusted for consistency).
  // Values use the same units expected in config.properties.
  const std::string DEFAULT_SERVER_HOST = "127.0.0.1";
  const int DEFAULT_SERVER_PORT = 50051;
  // Default values are expressed in the same unit expected in
  // config.properties.
  const int64_t DEFAULT_CHUNK_SIZE = 1024; // KB
  const std::string DEFAULT_DATA_DIRS = "data";
  const int64_t DEFAULT_MAX_BAND = 10; // MB/s
  const int DEFAULT_SEG_LEN = 1350;    // bytes
  const int64_t DEFAULT_SND_BUF = 64;  // MB
  const int64_t DEFAULT_RCV_BUF = 64;  // MB
  const int DEFAULT_SND_TIMEOUT = 20;  // seconds
  const int DEFAULT_RCV_TIMEOUT = 20;  // seconds
  const int DEFAULT_MAX_RETRIES = 3;
  const int DEFAULT_WORKER_THREADS = 4;
  const int DEFAULT_STABILITY_INTERVAL_S = 5;
  const int DEFAULT_STABILITY_CHECKS = 3;
  const int DEFAULT_WATCHER_INTERVAL_S = 5;
  const int DEFAULT_PENDING_INTERVAL_S = 15;
  const int DEFAULT_CB_THRESHOLD = 5;
  const int DEFAULT_CB_TIMEOUT_S = 60;
  const int DEFAULT_MEM_USAGE_PERCENT = 70;
  const std::string DEFAULT_DB_FILEPATH = "agent.db";
  const std::string DEFAULT_LOG_FILEPATH = "radar.log";
  const int DEFAULT_LOG_MAX_SIZE_MB = 50;
  const int DEFAULT_LOG_BACKUP_COUNT = 5;
  const int DEFAULT_POOL_SIZE = 4;
  const int DEFAULT_MAX_RETRY_ABANDON = 25;
  const bool DEFAULT_LOG_MOVE_ROTATE = true;
  const int MAX_FILE_PROCESSING_ATTEMPTS = 5;
  const int DEFAULT_RETRY_JITTER_MAX_MS = 1000;
  const bool DEFAULT_UDT_REQUIRE_GREETING = true;
  const std::string DEFAULT_UDT_EXPECTED_GREETING = "READY";
  const int DEFAULT_UDT_GREETING_TIMEOUT_MS = 2000;
  const bool DEFAULT_UDT_KEEPALIVE_ENABLED = false;
  const int DEFAULT_UDT_KEEPALIVE_INTERVAL_SECONDS = 60;
  const std::string DEFAULT_UDT_KEEPALIVE_PAYLOAD = "HEARTBEAT";
  const bool DEFAULT_SECURITY_ENABLED = true;
  const bool DEFAULT_SECURITY_HANDSHAKE_ENABLED = true;
  const bool DEFAULT_ALLOW_INSECURE_MODE = false;
  const std::string DEFAULT_SECURITY_PSK = "";
  const std::string DEFAULT_SECURITY_IDENTITY_MODE = "psk";
  const bool DEFAULT_ADAPTIVE_CHUNK_ENABLED = false;
  const int64_t DEFAULT_ADAPTIVE_CHUNK_MIN_KB = 32;
  const int64_t DEFAULT_ADAPTIVE_CHUNK_MAX_KB = 4 * 1024;
  const int64_t DEFAULT_ADAPTIVE_CHUNK_INITIAL_KB = 256;
  const int DEFAULT_ADAPTIVE_CHUNK_TARGET_ACK_MS = 700;
  const bool DEFAULT_DASHBOARD_ENABLED = false;
  const int DEFAULT_DASHBOARD_HEARTBEAT_INTERVAL_SECONDS = 15;
  const int DEFAULT_DASHBOARD_LOG_TAIL_LINES = 200;
  const bool DEFAULT_CHANGED_FILES_RESEND_ENABLED = true;
  const std::string DEFAULT_CHANGED_FILES_IDENTITY = "sha256";

  // Runtime configuration values
  std::vector<ServerEndpoint> serverEndpoints;
  std::vector<std::string> dataDirs;
  int64_t chunkSize;
  bool adaptiveChunkEnabled;
  int64_t adaptiveChunkMinBytes;
  int64_t adaptiveChunkMaxBytes;
  int64_t adaptiveChunkInitialBytes;
  int adaptiveChunkTargetAckMillis;
  int maxRetries;
  int64_t maxBandwidth;
  int segmentSize;
  int64_t sendBuffer;
  int64_t receiveBuffer;
  int sendTimeout;
  int receiveTimeout;
  int numThreads;
  int stabilityCheckInterval;
  int stabilityCheckCount;
  int watcherCheckInterval;
  int pendingCheckInterval;
  int cbFailureThreshold;
  int cbResetTimeoutSeconds;
  int memoryUsagePercentLimit;
  std::string logFilePath;
  std::string dbFilePath;
  int logMaxSizeMB;
  int logBackupCount;
  int poolSize;
  int maxRetryAbandon;
  std::string logFlushLevel;
  std::vector<std::string> watcherExcludeFiles;
  int maxFileProcessingAttempts;
  int retryJitterMaxMillis;
  bool udtRequireGreeting;
  std::string udtExpectedGreeting;
  int udtGreetingTimeoutMs;
  bool udtKeepAliveEnabled;
  int udtKeepAliveIntervalSeconds;
  std::string udtKeepAlivePayload;
  bool securityEnabled;
  bool securityHandshakeEnabled;
  bool allowInsecureMode;
  std::string securityPreSharedKey;
  std::string securityClientId;
  std::string securityIdentityMode;
  std::string securityClientPrivateKeyPath;
  std::string securityServerPublicKeyPath;
  bool dashboardEnabled;
  std::string dashboardUrl;
  int dashboardHeartbeatIntervalSeconds;
  int dashboardLogTailLines;
  bool changedFilesResendEnabled;
  std::string changedFilesIdentity;

  // Static utility helpers

  /**
   * @brief Removes leading and trailing whitespace from a string.
   * @param s The string to trim.
   * @return Trimmed string.
   */
  static std::string trim(const std::string &s) {
    size_t start = 0;
    while (start < s.size() &&
           std::isspace(static_cast<unsigned char>(s[start]))) {
      ++start;
    }

    if (start == s.size()) {
      return "";
    }

    size_t end = s.size();
    while (end > start &&
           std::isspace(static_cast<unsigned char>(s[end - 1]))) {
      --end;
    }

    return s.substr(start, end - start);
  }

  /**
   * @brief Loads configuration properties from a file.
   * @param configFile Path to the configuration file.
   * @return Map containing key-value configuration entries.
   */
  static std::map<std::string, std::string>
  loadConfigFile(const std::string &configFile) {
    std::map<std::string, std::string> configMap;
    std::ifstream config(configFile);
    if (!config) {
      std::cerr << "Unable to open configuration file: " << configFile
                << std::endl;
      return configMap;
    }
    std::string line;
    while (std::getline(config, line)) {
      if (line.empty() || line[0] == '#')
        continue;
      auto pos = line.find('=');
      if (pos == std::string::npos)
        continue;
      std::string key = trim(line.substr(0, pos));
      std::string value = trim(line.substr(pos + 1));
      configMap[key] = value;
    }
    return configMap;
  }

  static std::vector<std::string> split(const std::string &s, char delimiter) {
    std::vector<std::string> tokens;
    std::string token;
    std::istringstream tokenStream(s);
    while (std::getline(tokenStream, token, delimiter)) {
      tokens.push_back(trim(token)); // Reuse trim() for each token.
    }
    return tokens;
  }

  static std::vector<ServerEndpoint>
  parseServerTargets(const std::string &targets);

  /**
   * @brief Generic function to load a configuration parameter with optional
   * validation.
   * @tparam T Expected type of the parameter.
   * @param configMap Map of key-value pairs.
   * @param key Parameter key.
   * @param defaultValue Default value if key is missing or invalid.
   * @param errorMsg Error message for logging.
   * @param validator Optional validation function.
   * @return Loaded and validated parameter value.
   */
  template <typename T>
  static T loadConfigParam(
      const std::map<std::string, std::string> &configMap,
      const std::string &key, T defaultValue, const std::string &errorMsg,
      std::function<bool(T)> validator = [](T) { return true; }) {
    if (configMap.count(key) && !configMap.at(key).empty()) {
      try {
        T value = convertFromString<T>(configMap.at(key));
        if (validator(value)) {
          return value;
        } else {
          std::cerr << "Validation failed for key '" << key << "': " << errorMsg
                    << std::endl;
        }
      } catch (const std::exception &ex) {
        std::cerr << "Conversion error for key '" << key << "': " << ex.what()
                  << " - " << errorMsg << std::endl;
      }
    }
    return defaultValue;
  }

  // Specializations for convertFromString
  template <typename T> static T convertFromString(const std::string &str);
};

// Specializations for convertFromString.

template <>
inline std::string
RadarConfig::convertFromString<std::string>(const std::string &str) {
  return str;
}

template <>
inline int RadarConfig::convertFromString<int>(const std::string &str) {
  try {
    return std::stoi(str);
  } catch (const std::exception &e) {
    throw std::runtime_error("std::stoi conversion failed for value '" + str +
                             "': " + e.what());
  }
}

template <>
inline int64_t RadarConfig::convertFromString<int64_t>(const std::string &str) {
  try {
    return std::stoll(str);
  } catch (const std::exception &e) {
    throw std::runtime_error("std::stoll conversion failed for value '" + str +
                             "': " + e.what());
  }
}

template <>
inline double RadarConfig::convertFromString<double>(const std::string &str) {
  try {
    return std::stod(str);
  } catch (const std::exception &e) {
    throw std::runtime_error("std::stod conversion failed for value '" + str +
                             "': " + e.what());
  }
}
template <>
inline bool RadarConfig::convertFromString<bool>(const std::string &str) {
  std::string normalized = trim(str);
  std::transform(
      normalized.begin(), normalized.end(), normalized.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

  if (normalized == "true" || normalized == "1" || normalized == "yes" ||
      normalized == "on") {
    return true;
  }
  if (normalized == "false" || normalized == "0" || normalized == "no" ||
      normalized == "off") {
    return false;
  }

  throw std::runtime_error("Invalid boolean value '" + str + "'.");
}
