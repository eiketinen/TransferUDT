#pragma once
#include <Windows.h>
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
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
#include <vector>


namespace fs = std::filesystem;
/**
 * @class ServerConfig
 * @brief Singleton class that manages the configuration settings for the radar
 * agent.
 *
 * This class reads key-value pairs from a configuration file and exposes the
 * settings via getter methods. It provides default values and validates loaded
 * configurations.
 */
class ServerConfig {
public:
  // Deleção de construtores e operadores de cópia/movimento
  ServerConfig(const ServerConfig &) = delete;
  ServerConfig &operator=(const ServerConfig &) = delete;
  ServerConfig(ServerConfig &&) = delete;
  ServerConfig &operator=(ServerConfig &&) = delete;

  /**
   * @brief Accessor for the singleton instance.
   * @return Reference to the singleton instance of ServerConfig.
   */
  static ServerConfig &getInstance();
  /// @brief Returns the configured server port.
  int getServerPort() const { return serverPort; }
  /// @brief Returns the configured bind address.
  std::string getBindAddress() const { return bindAddress; }
  /// @brief Returns the configured data directory.
  std::string getStoragePath() const { return dataDir; }
  //@brief Returns the configured number of worker threads.
  int getNumThreads() const { return numThreads; }
  /// @brief Returns the configured maximum queued worker tasks.
  size_t getMaxPendingTasks() const { return maxPendingTasks; }
  /// @brief Returns the configured callback failure threshold.
  int getCBFailureThreshold() const { return cbFailureThreshold; }
  /// @brief Returns the configured callback reset timeout.
  int getCBResetTimeoutSeconds() const { return cbResetTimeoutSeconds; }
  /// @brief Returns the configured log file path.
  std::string getLogFilePath() const { return logFilePath; }
  /// @brief Returns the configured database file path.
  std::string getDBFilePath() const { return dbFilePath; }
  /// @brief Return the configured log file max size in MB.
  int getLogMaxSizeMB() const { return logMaxSizeMB; }
  /// @brief Return the configured log file backup count.
  int getLogBackupCount() const { return logBackupCount; }
  /// @brief Return the max connection.
  int getMaxConnection() const { return maxConnection; }
  /// @brief Return the reconstructed directory.
  std::string getReconstructedPath() const { return reconstructedDir; }
  /// @brief Returns the maximum accepted logical file size in bytes.
  uint64_t getMaxFileSizeBytes() const { return maxFileSizeBytes; }
  /// @brief Returns the maximum reconstructed storage budget per client.
  uint64_t getMaxClientStorageBytes() const { return maxClientStorageBytes; }
  /// @brief Returns the configured send buffer size.
  DWORDLONG getSendBuffer() const { return sendBuffer; }
  /// @brief Returns the configured receive buffer size.
  DWORDLONG getReceiveBuffer() const { return receiveBuffer; }
  /// @brief Returns the configured maximum bandwidth.
  int64_t getMaxBandwidth() const { return maxBandwidth; }
  /// @brief Returns the configured segment size.
  int getSegmentSize() const { return segmentSize; }
  /// @brief Returns the configured send timeout.
  int getSendTimeout() const { return sendTimeout; }
  /// @brief Returns the configured receive timeout.
  int getReceiveTimeout() const { return receiveTimeout; }
  /// @brief Returns the configured linger on/off.
  int getLingerOnOff() const { return lingerOnOff; }
  /// @brief Returns the configured linger time.
  int getLingerTime() const { return lingerTime; }
  /// @brief Returns the configured log flush level.
  std::string getLogFlushLevel() const { return logFlushLevel; }
  /// @brief Returns whether encrypted/authenticated packet transfer is enabled.
  bool isSecurityEnabled() const { return securityEnabled; }
  /// @brief Returns whether PSK challenge-response handshake is enabled.
  bool isSecurityHandshakeEnabled() const { return securityHandshakeEnabled; }
  bool isInsecureModeAllowed() const { return allowInsecureMode; }
  bool isClientAllowed(const std::string &clientIp) const {
    if (allowedClients.empty()) {
      return true;
    }
    return std::find(allowedClients.begin(), allowedClients.end(), clientIp) !=
           allowedClients.end();
  }
  bool isClientIdentityAllowed(const std::string &clientId) const {
    if (!clientPreSharedKeys.empty() &&
        clientPreSharedKeys.find(clientId) == clientPreSharedKeys.end()) {
      return false;
    }
    if (allowedClientIds.empty()) {
      return true;
    }
    return std::find(allowedClientIds.begin(), allowedClientIds.end(),
                     clientId) != allowedClientIds.end();
  }
  /// @brief Returns the pre-shared key used by the secure packet envelope.
  const std::string &getSecurityPreSharedKey() const {
    return securityPreSharedKey;
  }
  const std::string &
  getSecurityPreSharedKeyForClient(const std::string &clientId) const {
    const auto it = clientPreSharedKeys.find(clientId);
    if (it != clientPreSharedKeys.end()) {
      return it->second;
    }
    return securityPreSharedKey;
  }
  bool hasClientPreSharedKeys() const { return !clientPreSharedKeys.empty(); }
  /**
   * @brief Returns a string representation of key configuration settings.
   * @return Summary string of current configuration.
   */
  std::string getRadarConfigToSring() const {
    return std::string(
        "Current Settings: serverPort=" + std::to_string(serverPort) +
        ", dataDir=" + dataDir +
        ", maxConnection=" + std::to_string(maxConnection) +
        ", threads=" + std::to_string(numThreads) +
        ", cbThreshold=" + std::to_string(cbFailureThreshold) +
        ", cbTimeout=" + std::to_string(cbResetTimeoutSeconds) +
        ", logFilePath=" + logFilePath + ", dbFilePath=" + dbFilePath +
        ", logMaxSizeMB=" + std::to_string(logMaxSizeMB) +
        ", logBackupCount=" + std::to_string(logBackupCount));
  }

private:
  /// Private constructor.
  ServerConfig();
  /// Virtual destructor.
  virtual ~ServerConfig();

  // Constantes padrão (valores padrão de timeout foram ajustados para
  // consistência)
  const int DEFAULT_LISTEN_PORT = 50051;
  const std::string DEFAULT_BIND_ADDRESS = "127.0.0.1";
  const std::string DEFAULT_STORAGE_PATH = "C:/Data";
  const int DEFAULT_MAX_CONNECTION = 100;
  const int DEFAULT_WORKER_THREADS = 4;
  const int64_t DEFAULT_MAX_PENDING_TASKS = 256;
  const int64_t DEFAULT_MAX_FILE_SIZE_MB = 10240;      // 10 GiB
  const int64_t DEFAULT_MAX_CLIENT_STORAGE_MB = 20480; // 20 GiB
  const std::string DEFAULT_DB_FILEPATH = "radar.db";
  const std::string DEFAULT_LOG_FILEPATH = "radar_server.log";
  const int DEFAULT_LOG_MAX_SIZE_MB = 50;
  const int DEFAULT_LOG_BACKUP_COUNT = 5;
  const int DEFAULT_CB_THRESHOLD = 5;
  const int DEFAULT_CB_TIMEOUT_S = 60;
  const std::string DEFAULT_RECONSTRUCTED_PATH = "C:/Data/Reconstructed";
  const int64_t DEFAULT_SND_BUF = 64;  // MB
  const int64_t DEFAULT_RCV_BUF = 64;  // MB
  const int64_t DEFAULT_MAX_BAND = 10; // MB/s
  const int DEFAULT_SEG_LEN = 1350;    // bytes
  const int DEFAULT_SND_TIMEOUT = 20;  // seconds
  const int DEFAULT_RCV_TIMEOUT = 20;  // seconds
  const int DEFAULT_LINGER_ONOFF = 1;
  const int DEFAULT_LINGER_TIME = 5; // 5 segundos
  const bool DEFAULT_SECURITY_ENABLED = true;
  const bool DEFAULT_SECURITY_HANDSHAKE_ENABLED = true;
  const bool DEFAULT_ALLOW_INSECURE_MODE = false;
  const std::string DEFAULT_SECURITY_PSK = "";

  // Variáveis de configuração
  int sendTimeout;
  int receiveTimeout;
  int serverPort;
  std::string bindAddress;
  std::string dataDir;
  int maxConnection;
  int numThreads;
  size_t maxPendingTasks;
  uint64_t maxFileSizeBytes;
  uint64_t maxClientStorageBytes;
  int cbFailureThreshold;
  int cbResetTimeoutSeconds;
  std::string logFilePath;
  std::string dbFilePath;
  int logMaxSizeMB;
  int logBackupCount;
  std::string reconstructedDir;
  DWORDLONG sendBuffer;
  DWORDLONG receiveBuffer;
  int64_t maxBandwidth;
  int segmentSize;
  int lingerOnOff;
  int lingerTime;
  std::string logFlushLevel;
  bool securityEnabled;
  bool securityHandshakeEnabled;
  bool allowInsecureMode;
  std::string securityPreSharedKey;
  std::map<std::string, std::string> clientPreSharedKeys;
  std::vector<std::string> allowedClients;
  std::vector<std::string> allowedClientIds;

  // Funções utilitárias estáticas

  /**
   * @brief Removes leading and trailing whitespace from a string.
   * @param s The string to trim.
   * @return Trimmed string.
   */
  static std::string trim(const std::string &s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
      return std::string();
    }

    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
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

  static std::vector<std::string> splitList(const std::string &s) {
    std::vector<std::string> tokens;
    std::string token;
    std::istringstream tokenStream(s);
    while (std::getline(tokenStream, token, ',')) {
      token = trim(token);
      if (!token.empty()) {
        tokens.push_back(token);
      }
    }
    return tokens;
  }

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

// Especializações para convertFromString

template <>
inline std::string
ServerConfig::convertFromString<std::string>(const std::string &str) {
  return str;
}

template <>
inline int ServerConfig::convertFromString<int>(const std::string &str) {
  try {
    return std::stoi(str);
  } catch (const std::exception &) {
    throw std::runtime_error("std::stoi conversion failed");
  }
}

template <>
inline int64_t
ServerConfig::convertFromString<int64_t>(const std::string &str) {
  try {
    return std::stoll(str);
  } catch (const std::exception &) {
    throw std::runtime_error("std::stoll conversion failed");
  }
}

template <>
inline double ServerConfig::convertFromString<double>(const std::string &str) {
  try {
    return std::stod(str);
  } catch (const std::exception &) {
    throw std::runtime_error("std::stod conversion failed");
  }
}

template <>
inline bool ServerConfig::convertFromString<bool>(const std::string &str) {
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
