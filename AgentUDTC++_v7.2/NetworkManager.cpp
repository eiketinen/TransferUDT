#include "NetworkManager.h"
#include "ChunkPacketParser.h"
#include "RadarConfig.h"
#include "WatchedPathMapper.h"
#include <cmath>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <random>
#include <stdexcept>
#include <thread>


namespace fs = std::filesystem;

namespace {
constexpr const char *RESPONSE_SUCCESS = "SUCCESS";
constexpr const char *RESPONSE_SUCCESS_FILE_COMPLETE = "SUCCESS_FILE_COMPLETE";
constexpr const char *RESPONSE_FILE_ALREADY_EXISTS = "FILE_ALREADY_EXISTS";

std::string endpointKey(const NetworkManager::Endpoint &endpoint) {
  return endpoint.host + ":" + std::to_string(endpoint.port);
}

std::string directoryForChunk(const ChunkMetadata &chunk) {
  const std::string declaredDirectory = chunk.getDirectory();
  if (!declaredDirectory.empty()) {
    return declaredDirectory;
  }

  std::string relativeDirectory;
  if (WatchedPathMapper::TryBuildRelativeDirectory(
          chunk.getFilePath(), RadarConfig::getInstance().getDataDirs(),
          relativeDirectory)) {
    return relativeDirectory;
  }

  return chunk.getFilePath().parent_path().filename().u8string();
}

uint64_t maxSecureControlContentLength() {
  return SecurePacket::MaxSessionEncryptedContentLength(
      sizeof(uint32_t) + SecurePacket::kMaxControlMessageBytes);
}

bool waitInterruptible(const std::atomic<bool> &isRunning,
                       std::chrono::milliseconds delay) {
  if (delay <= std::chrono::milliseconds::zero()) {
    return isRunning.load();
  }

  constexpr auto slice = std::chrono::milliseconds(100);
  auto waited = std::chrono::milliseconds::zero();

  while (waited < delay) {
    if (!isRunning.load()) {
      return false;
    }

    const auto remaining = delay - waited;
    const auto currentSlice = (remaining < slice) ? remaining : slice;
    std::this_thread::sleep_for(currentSlice);
    waited += currentSlice;
  }

  return isRunning.load();
}

std::string secureKeyForConnection(PooledUDTConnection &connection,
                                   const std::string &fallbackKey) {
  const std::string &sessionKey = connection->getSecureSessionKey();
  return sessionKey.empty() ? fallbackKey : sessionKey;
}
} // namespace
/**
 * Constructs a NetworkManager object, initializing the network settings and
 * connection pools for every configured endpoint.
 *
 * @param endpoints List of server endpoints (host:port) that must receive each
 * chunk.
 * @param db Reference to the database.
 * @param maxRetries Maximum number of retries for operations.
 * @param circuitBreakerThreshold Failure threshold for circuit breaker.
 * @param resetTimeoutSeconds Reset timeout for circuit breaker in seconds.
 * @param poolSize Maximum number of connections in each pool.
 * @param maxBandwidth Maximum bandwidth in bytes/sec (0 for unlimited).
 * @param segmentSize UDT segment size.
 * @param sendBuffer Send buffer size in bytes.
 * @param receiveBuffer Receive buffer size in bytes.
 * @param sendTimeout Send timeout in milliseconds.
 * @param receiveTimeout Receive timeout in milliseconds.
 */
NetworkManager::NetworkManager(
    const std::vector<Endpoint> &endpoints, Database &db, int maxRetries,
    int circuitBreakerThreshold, int resetTimeoutSeconds, size_t poolSize,
    int64_t maxBandwidth, int segmentSize, int64_t sendBuffer,
    int64_t receiveBuffer, int sendTimeout, int receiveTimeout,
    int retryJitterMaxMillis, bool requireGreeting,
    std::string expectedGreeting, int greetingTimeoutMs, bool keepAliveEnabled,
    int keepAliveIntervalSeconds, std::string keepAlivePayload,
    bool securityEnabled, std::string securityPreSharedKey,
    bool securityHandshakeEnabled, std::string securityClientId,
    std::string securityIdentityMode, std::string securityClientPrivateKeyPath,
    std::string securityServerPublicKeyPath,
    bool adaptiveChunkEnabled, uint64_t adaptiveChunkMinBytes,
    uint64_t adaptiveChunkMaxBytes, uint64_t adaptiveChunkInitialBytes,
    int adaptiveChunkTargetAckMillis)
    : database(db), maxRetries(maxRetries),
      circuitBreakerThreshold(circuitBreakerThreshold),
      resetTimeoutSeconds(resetTimeoutSeconds), maxBandwidth(maxBandwidth),
      segmentSize(segmentSize), sendBuffer(sendBuffer),
      receiveBuffer(receiveBuffer), sendTimeout(sendTimeout),
      receiveTimeout(receiveTimeout),
      retryJitterMaxMillis(retryJitterMaxMillis),
      requireGreeting(requireGreeting),
      expectedGreeting(std::move(expectedGreeting)),
      greetingTimeoutMs(greetingTimeoutMs), keepAliveEnabled(keepAliveEnabled),
      keepAliveIntervalSeconds(keepAliveIntervalSeconds),
      keepAlivePayload(std::move(keepAlivePayload)),
      securityEnabled(securityEnabled),
      securityPreSharedKey(std::move(securityPreSharedKey)),
      securityHandshakeEnabled(securityHandshakeEnabled),
      securityClientId(std::move(securityClientId)),
      securityIdentityMode(std::move(securityIdentityMode)),
      securityClientPrivateKeyPath(std::move(securityClientPrivateKeyPath)),
      securityServerPublicKeyPath(std::move(securityServerPublicKeyPath)),
      adaptiveChunkSettings{adaptiveChunkEnabled, adaptiveChunkMinBytes,
                            adaptiveChunkMaxBytes, adaptiveChunkInitialBytes,
                            adaptiveChunkTargetAckMillis} {
  if (endpoints.empty()) {
    throw std::invalid_argument(
        "NetworkManager requires at least one endpoint.");
  }

  targets.reserve(endpoints.size());
  for (const auto &endpoint : endpoints) {
    auto circuitBreakerName = endpointKey(endpoint);
    auto circuitBreaker = std::make_unique<CircuitBreaker>(
        circuitBreakerName, circuitBreakerThreshold,
        std::chrono::seconds(resetTimeoutSeconds));

    auto connectionPool = std::make_unique<UDTConnectionPool>(
        poolSize, endpoint.host, endpoint.port, maxBandwidth, segmentSize,
        sendBuffer, receiveBuffer, sendTimeout, receiveTimeout, *circuitBreaker,
        requireGreeting, this->expectedGreeting, greetingTimeoutMs,
        keepAliveEnabled, keepAliveIntervalSeconds, this->keepAlivePayload,
        this->securityHandshakeEnabled, this->securityPreSharedKey,
        this->securityClientId, this->securityIdentityMode,
        this->securityClientPrivateKeyPath,
        this->securityServerPublicKeyPath, this->securityEnabled);

    auto adaptiveChunks =
        std::make_unique<AdaptiveChunkController>(adaptiveChunkSettings);

    if (adaptiveChunks->isEnabled()) {
      Logger::getInstance().info(
          "NetworkManager::NetworkManager",
          "Adaptive chunk telemetry enabled for " + circuitBreakerName +
              " initial=" +
              std::to_string(adaptiveChunks->suggestedChunkSize()) +
              " bytes target_ack_ms=" +
              std::to_string(adaptiveChunkSettings.targetAckMillis));
    }

    targets.push_back(TargetContext{endpoint, std::move(circuitBreaker),
                                    std::move(connectionPool),
                                    std::move(adaptiveChunks)});
  }
}
/**
 * Destructor for the NetworkManager class.
 *
 * Shuts down the connection pool if it exists.
 */
NetworkManager::~NetworkManager() {
  for (auto &target : targets) {
    if (target.connectionPool) {
      target.connectionPool->shutdown();
    }
  }
}

/**
 * Sends pending chunks to the server.
 *
 * This function retrieves pending chunks from the database, connects to the
 * server, and sends each chunk. It handles file operations, chunk positioning,
 * and error logging.
 *
 * @return true if all chunks were sent successfully, false otherwise
 *
 * @throws std::exception if an error occurs during the sending process
 */
bool NetworkManager::sendPendingFailedChunks(
    const std::atomic<bool> &is_running,
    std::unique_ptr<FileProcessor> &fileProcessor, int maxRetriesAbandon) {
  (void)fileProcessor;

  try {
    if (targets.empty()) {
      Logger::getInstance().error(
          "NetworkManager::sendPendingFailedChunks",
          "No configured targets available for pending resend.");
      return false;
    }

    std::vector<ChunkMetadata> pendingChunks =
        database.getPendingChunks(maxRetriesAbandon);
    if (pendingChunks.empty()) {
      return true;
    }

    std::map<const fs::path, std::vector<ChunkMetadata>> chunksPerFile;
    for (const auto &chunk : pendingChunks) {
      chunksPerFile[chunk.getFilePath()].push_back(chunk);
    }

    for (const auto &[filePath, chunks] : chunksPerFile) {
      if (!is_running) {
        Logger::getInstance().info(
            "NetworkManager::sendPendingFailedChunks",
            "Shutdown signaled, aborting pending chunk processing loop.");
        return false;
      }

      std::string filepathStr = filePath.u8string();
      std::ifstream file(filePath, std::ios::binary);
      if (!file.is_open()) {
        Logger::getInstance().warning("NetworkManager::sendPendingFailedChunks",
                                      "Could not open file '" + filepathStr +
                                          "' for resend. Will retry later.");
        try {
          database.markFileAsFailed(filePath);
        } catch (const std::exception &db_ex) {
          Logger::getInstance().error(
              "NetworkManager::sendPendingFailedChunks",
              "Failed to mark file as failed for '" + filepathStr +
                  "'. Error: " + std::string(db_ex.what()));
        }

        for (const auto &missingChunk : chunks) {
          try {
            database.updateChunkRetries(missingChunk.getFilePath(),
                                        missingChunk.getChunkNumber(),
                                        missingChunk.getRetries() + 1);
            database.updateChunkStatus(missingChunk.getFilePath(),
                                       missingChunk.getChunkNumber(), "failed");
          } catch (const std::exception &db_ex) {
            Logger::getInstance().error(
                "NetworkManager::sendPendingFailedChunks",
                "Failed to update retry/status for missing file chunk " +
                    missingChunk.getFilename() + ":" +
                    std::to_string(missingChunk.getChunkNumber()) +
                    ". Error: " + std::string(db_ex.what()));
          }
        }
        continue;
      }

      Logger::getInstance().info("NetworkManager::sendPendingFailedChunks",
                                 "Processing file '" + filepathStr +
                                     "' for pending chunks.");

      auto markChunkAsFailedSafe = [&](const ChunkMetadata &failedChunk,
                                       const std::string &reason) {
        Logger::getInstance().error("NetworkManager::sendPendingFailedChunks",
                                    reason);
        try {
          database.updateChunkRetries(failedChunk.getFilePath(),
                                      failedChunk.getChunkNumber(),
                                      failedChunk.getRetries() + 1);
          database.updateChunkStatus(failedChunk.getFilePath(),
                                     failedChunk.getChunkNumber(), "failed");
        } catch (const std::exception &db_ex) {
          Logger::getInstance().error(
              "NetworkManager::sendPendingFailedChunks",
              "Failed to mark chunk as failed for " +
                  failedChunk.getFilename() + ":" +
                  std::to_string(failedChunk.getChunkNumber()) +
                  ". Error: " + std::string(db_ex.what()));
        }
      };

      IfstreamRAII fileGuard(file);
      file.seekg(0, std::ios::end);
      std::streamoff fileSize = file.tellg();
      if (fileSize < 0) {
        Logger::getInstance().error(
            "NetworkManager::sendPendingFailedChunks",
            "Could not determine file size for resend: '" + filepathStr +
                "'. Marking file as failed.");
        try {
          database.markFileAsFailed(filePath);
        } catch (const std::exception &db_ex) {
          Logger::getInstance().error(
              "NetworkManager::sendPendingFailedChunks",
              "Failed to mark file as failed for '" + filepathStr +
                  "'. Error: " + std::string(db_ex.what()));
        }

        for (const auto &missingChunk : chunks) {
          try {
            database.updateChunkRetries(missingChunk.getFilePath(),
                                        missingChunk.getChunkNumber(),
                                        missingChunk.getRetries() + 1);
            database.updateChunkStatus(missingChunk.getFilePath(),
                                       missingChunk.getChunkNumber(), "failed");
          } catch (const std::exception &db_ex) {
            Logger::getInstance().error(
                "NetworkManager::sendPendingFailedChunks",
                "Failed to update retry/status for file chunk " +
                    missingChunk.getFilename() + ":" +
                    std::to_string(missingChunk.getChunkNumber()) +
                    ". Error: " + std::string(db_ex.what()));
          }
        }
        continue;
      }

      file.clear();
      file.seekg(0, std::ios::beg);
      if (!file) {
        Logger::getInstance().error(
            "NetworkManager::sendPendingFailedChunks",
            "Could not seek to beginning for resend: '" + filepathStr +
                "'. Marking file as failed.");
        try {
          database.markFileAsFailed(filePath);
        } catch (const std::exception &db_ex) {
          Logger::getInstance().error(
              "NetworkManager::sendPendingFailedChunks",
              "Failed to mark file as failed for '" + filepathStr +
                  "'. Error: " + std::string(db_ex.what()));
        }

        for (const auto &missingChunk : chunks) {
          try {
            database.updateChunkRetries(missingChunk.getFilePath(),
                                        missingChunk.getChunkNumber(),
                                        missingChunk.getRetries() + 1);
            database.updateChunkStatus(missingChunk.getFilePath(),
                                       missingChunk.getChunkNumber(), "failed");
          } catch (const std::exception &db_ex) {
            Logger::getInstance().error(
                "NetworkManager::sendPendingFailedChunks",
                "Failed to update retry/status for file chunk " +
                    missingChunk.getFilename() + ":" +
                    std::to_string(missingChunk.getChunkNumber()) +
                    ". Error: " + std::string(db_ex.what()));
          }
        }
        continue;
      }

      const uint64_t fileSizeBytes = static_cast<uint64_t>(fileSize);

      for (const auto &chunk : chunks) {
        if (!is_running) {
          Logger::getInstance().info(
              "NetworkManager::sendPendingFailedChunks",
              "Shutdown signaled during chunk processing.");
          return false;
        }

        if (chunk.getChunkNumber() < 0 || chunk.getChunkSize() == 0) {
          markChunkAsFailedSafe(
              chunk, "Invalid pending chunk metadata for resend: " +
                         chunk.getFilename() + ":" +
                         std::to_string(chunk.getChunkNumber()) +
                         " (chunk_size=" +
                         std::to_string(chunk.getChunkSize()) + ").");
          continue;
        }

        const uint64_t chunkSize = static_cast<uint64_t>(chunk.getChunkSize());
        const uint64_t chunkNumber =
            static_cast<uint64_t>(chunk.getChunkNumber());
        uint64_t offset = chunk.getChunkOffset();
        if (offset == 0 && chunkNumber > 0) {
          if (chunkNumber >
              ((std::numeric_limits<uint64_t>::max)() / chunkSize)) {
            markChunkAsFailedSafe(
                chunk, "Chunk offset overflow detected for resend: " +
                           chunk.getFilename() + ":" +
                           std::to_string(chunk.getChunkNumber()) + ".");
            continue;
          }
          offset = chunkNumber * chunkSize;
        }
        if (offset >= fileSizeBytes) {
          markChunkAsFailedSafe(
              chunk, "Invalid offset " + std::to_string(offset) +
                         " for file '" + filepathStr + "' with size " +
                         std::to_string(fileSizeBytes) + ".");
          continue;
        }

        file.clear();
        file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        if (!file) {
          markChunkAsFailedSafe(
              chunk, "Error seeking file for chunk resend: '" + filepathStr +
                         "' (chunk " + std::to_string(chunk.getChunkNumber()) +
                         ").");
          break;
        }

        const uint64_t remainingBytes = fileSizeBytes - offset;
        const uint64_t actualChunkSize =
            (chunkSize < remainingBytes) ? chunkSize : remainingBytes;
        if (actualChunkSize == 0 ||
            actualChunkSize >
                static_cast<uint64_t>(
                    (std::numeric_limits<std::streamsize>::max)())) {
          markChunkAsFailedSafe(
              chunk, "Invalid chunk size for resend after bounds check: " +
                         std::to_string(actualChunkSize) + " bytes.");
          continue;
        }

        std::vector<char> buffer(static_cast<size_t>(actualChunkSize));
        file.read(buffer.data(), static_cast<std::streamsize>(actualChunkSize));
        std::streamsize bytesRead = file.gcount();
        if (bytesRead <= 0) {
          markChunkAsFailedSafe(chunk,
                                "Error reading chunk data for resend: " +
                                    std::to_string(chunk.getChunkNumber()));
          continue;
        }
        buffer.resize(static_cast<size_t>(bytesRead));

        bool chunkFailed = false;
        bool chunkAlreadyExistsEverywhere = true;
        int retryAccumulator = chunk.getRetries();

        for (auto &target : targets) {
          if (!is_running) {
            Logger::getInstance().info(
                "NetworkManager::sendPendingFailedChunks",
                "Shutdown signaled during target resend loop.");
            return false;
          }

          auto result =
              sendPendingChunkToTarget(target, chunk, buffer, retryAccumulator,
                                       offset, fileSizeBytes, is_running);
          if (!is_running.load()) {
            Logger::getInstance().info(
                "NetworkManager::sendPendingFailedChunks",
                "Shutdown signaled during pending retry delay.");
            return false;
          }
          if (result == ChunkSendResult::Failure) {
            chunkFailed = true;
            break;
          }
          if (result != ChunkSendResult::AlreadyExists) {
            chunkAlreadyExistsEverywhere = false;
          }
        }

        if (chunkFailed) {
          try {
            database.updateChunkStatus(chunk.getFilePath(),
                                       chunk.getChunkNumber(), "failed");
          } catch (const std::exception &db_ex) {
            Logger::getInstance().error(
                "NetworkManager::sendPendingFailedChunks",
                "Failed to update status to failed for chunk " +
                    chunk.getFilename() + ": " +
                    std::string(db_ex.what()));
          }
          continue;
        }

        if (chunkAlreadyExistsEverywhere) {
          try {
            database.addProcessedFileAndCleanupChunks(chunk.getFilePath());
          } catch (const std::exception &db_ex) {
            Logger::getInstance().error(
                "NetworkManager::sendPendingFailedChunks",
                "Failed to clean up after existing file reported for '" +
                    filepathStr + "'. Error: " + std::string(db_ex.what()));
          }
          break;
        }

        try {
          database.updateChunkStatus(chunk.getFilePath(),
                                     chunk.getChunkNumber(), "success");
        } catch (const std::exception &db_ex) {
          Logger::getInstance().critical(
              "NetworkManager::sendPendingFailedChunks",
              "Failed to update status to success for chunk " +
                  chunk.getFilename() + ": " +
                  std::string(db_ex.what()));
        }
      }
    }
  } catch (const std::exception &e) {
    Logger::getInstance().critical("NetworkManager::sendPendingFailedChunks",
                                   "Error sending pending chunks: " +
                                       std::string(e.what()));
    database.logError("Error sending pending chunks", 0, "ERROR", e.what());
    return false;
  }
  return true;
}
/**
 * Sends a chunk of data over a UDT connection to the server and handles the
 * server response.
 *
 * This function serializes the chunk metadata and data, sends it to the server,
 * receives the server response, and returns true if the response indicates
 * success.
 *
 * @param connection_wrapper The UDT connection wrapper to use for sending the
 * chunk.
 * @param chunk The metadata of the chunk to be sent.
 * @param data The actual data of the chunk to be sent.
 *
 * @return ChunkSendResult indicating how the server handled the chunk.
 *
 * @throws std::exception if an error occurs during the sending process.
 */
NetworkManager::ChunkSendResult NetworkManager::_sendChunkInternal(
    PooledUDTConnection &connection_wrapper, TargetContext &target,
    const ChunkMetadata &chunk, const std::vector<char> &data,
    uint64_t chunkOffset, uint64_t totalFileSize) {
  auto &connection = connection_wrapper;

  const auto &endpoint = target.endpoint;
  const std::string serverAddress =
      endpoint.host + ":" + std::to_string(endpoint.port);

  if (chunk.getTransferId().size() !=
      ChunkPacketParser::kTransferIdHexLength) {
    Logger::getInstance().error(
        "NetworkManager::_sendChunkInternal",
        "Chunk " + std::to_string(chunk.getChunkNumber()) +
            " has no valid transfer identity; refusing to send it.");
    return ChunkSendResult::Failure;
  }

  if (target.adaptiveChunks && target.adaptiveChunks->isEnabled()) {
    Logger::getInstance().info(
        "NetworkManager::_sendChunkInternal",
        "Adaptive chunk recommendation for " + serverAddress +
            " before chunk " + std::to_string(chunk.getChunkNumber()) +
            ": current_chunk_bytes=" + std::to_string(data.size()) +
            " recommended_next_chunk_bytes=" +
            std::to_string(target.adaptiveChunks->suggestedChunkSize(
                totalFileSize > chunkOffset ? totalFileSize - chunkOffset
                                            : 0)));
  }

  // Serializa a mensagem
  std::vector<char> serializedMessage = serializeChunkMessage(
      chunk.getFilename(), directoryForChunk(chunk), serverAddress,
      chunk.getTransferId(), chunk.getChunkNumber(), chunk.getTotalChunk(),
      chunkOffset, totalFileSize, chunk.getHash(), data);

  if (securityEnabled) {
    try {
      const std::string secureKey =
          secureKeyForConnection(connection, securityPreSharedKey);
      if (!connection->hasSecureSessionId()) {
        throw std::runtime_error("Secure session was not established.");
      }
      serializedMessage = SecurePacket::EncryptPacket(
          serializedMessage, secureKey, connection->getSecureSessionId(),
          connection->takeNextSecureOutboundSequence());
    } catch (const std::exception &e) {
      Logger::getInstance().error(
          "NetworkManager::_sendChunkInternal",
          "Failed to encrypt chunk " +
              std::to_string(chunk.getChunkNumber()) + ": " + e.what());
      if (target.adaptiveChunks) {
        target.adaptiveChunks->recordFailure();
      }
      return ChunkSendResult::Failure;
    }
  }

  const auto sendStart = std::chrono::steady_clock::now();

  // Envia a mensagem
  if (!connection->sendAll(serializedMessage.data(),
                           static_cast<int>(serializedMessage.size()))) {
    Logger::getInstance().error(
        "NetworkManager::_sendChunkInternal",
        "sendAll failed for chunk " + std::to_string(chunk.getChunkNumber()) +
            ": " + std::string(UDT::getlasterror().getErrorMessage()));
    if (target.adaptiveChunks) {
      target.adaptiveChunks->recordFailure();
    }
    return ChunkSendResult::Failure; // Falhou no envio
  }

  // Recebe a resposta
  std::string serverResponse;
  if (!receiveControlMessage(connection, serverResponse)) {
    Logger::getInstance().error(
        "NetworkManager::_sendChunkInternal",
        "receive control message failed for chunk " +
            std::to_string(chunk.getChunkNumber()) + ": " +
            std::string(UDT::getlasterror().getErrorMessage()));
    if (target.adaptiveChunks) {
      target.adaptiveChunks->recordFailure();
    }
    return ChunkSendResult::Failure; // Falhou na recepÃƒÂ§ÃƒÂ£o
  }

  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - sendStart);

  // Processa a resposta
  if (serverResponse == RESPONSE_SUCCESS) {
    if (target.adaptiveChunks) {
      target.adaptiveChunks->recordSuccess(data.size(), elapsed);
      const auto snapshot = target.adaptiveChunks->snapshot();
      if (snapshot.enabled) {
        Logger::getInstance().info(
            "NetworkManager::_sendChunkInternal",
            "Adaptive chunk telemetry for " + serverAddress +
                " chunk=" + std::to_string(chunk.getChunkNumber()) +
                " bytes=" + std::to_string(data.size()) +
                " ack_ms=" + std::to_string(elapsed.count()) +
                " ewma_ack_ms=" + std::to_string(snapshot.ewmaAckMillis) +
                " ewma_throughput_Bps=" +
                std::to_string(snapshot.ewmaThroughputBytesPerSecond) +
                " recommended_next_chunk_bytes=" +
                std::to_string(snapshot.suggestedChunkSizeBytes));
      }
    }
    return ChunkSendResult::Success;
  } else if (serverResponse == RESPONSE_SUCCESS_FILE_COMPLETE) {
    if (target.adaptiveChunks) {
      target.adaptiveChunks->recordSuccess(data.size(), elapsed);
    }
    if (!sendControlMessage(connection, "CLOSE_NOW")) {
      Logger::getInstance().warning(
          "NetworkManager::_sendChunkInternal",
          "Failed to send CLOSE_NOW after SUCCESS_FILE_COMPLETE for chunk " +
              std::to_string(chunk.getChunkNumber()));
    }
    return ChunkSendResult::Success;
  } else if (serverResponse == RESPONSE_FILE_ALREADY_EXISTS) {
    if (target.adaptiveChunks) {
      target.adaptiveChunks->recordSuccess(data.size(), elapsed);
    }
    if (!sendControlMessage(connection, "CLOSE_NOW")) {
      Logger::getInstance().warning(
          "NetworkManager::_sendChunkInternal",
          "Failed to send CLOSE_NOW after FILE_ALREADY_EXISTS for chunk " +
              std::to_string(chunk.getChunkNumber()));
    }
    return ChunkSendResult::AlreadyExists;
  } else {
    Logger::getInstance().error("NetworkManager::_sendChunkInternal",
                                "Server responded with failure for chunk " +
                                    std::to_string(chunk.getChunkNumber()) +
                                    ". Response: " + serverResponse);
    if (target.adaptiveChunks) {
      target.adaptiveChunks->recordFailure();
    }
    return ChunkSendResult::Failure;
  }
}

uint64_t NetworkManager::getRecommendedChunkSize(uint64_t remainingBytes) const {
  uint64_t recommendation = 0;
  for (const auto &target : targets) {
    if (!target.adaptiveChunks || !target.adaptiveChunks->isEnabled()) {
      continue;
    }
    const uint64_t targetRecommendation =
        target.adaptiveChunks->suggestedChunkSize(remainingBytes);
    if (targetRecommendation == 0) {
      continue;
    }
    if (recommendation == 0 || targetRecommendation < recommendation) {
      recommendation = targetRecommendation;
    }
  }
  return recommendation;
}

bool NetworkManager::sendControlMessage(PooledUDTConnection &connection,
                                        const std::string &message) {
  if (!securityEnabled) {
    return connection->sendString(message);
  }

  try {
    const std::string secureKey =
        secureKeyForConnection(connection, securityPreSharedKey);
    if (!connection->hasSecureSessionId()) {
      throw std::runtime_error("Secure session was not established.");
    }
    std::vector<char> encrypted = SecurePacket::EncryptControlMessage(
        message, secureKey, connection->getSecureSessionId(),
        connection->takeNextSecureOutboundSequence());
    return connection->sendAll(encrypted.data(),
                               static_cast<int>(encrypted.size()));
  } catch (const std::exception &e) {
    Logger::getInstance().error("NetworkManager::sendControlMessage",
                                "Failed to encrypt control message: " +
                                    std::string(e.what()));
    connection.invalidate();
    return false;
  }
}

bool NetworkManager::receiveControlMessage(PooledUDTConnection &connection,
                                           std::string &message) {
  if (!securityEnabled) {
    return connection->recvString(message);
  }

  uint32_t totalLengthNetwork = 0;
  if (!connection->recvAll(reinterpret_cast<char *>(&totalLengthNetwork),
                           sizeof(totalLengthNetwork))) {
    connection.invalidate();
    return false;
  }

  const uint32_t totalLength = ntohl(totalLengthNetwork);
  if (totalLength == 0 || totalLength > maxSecureControlContentLength()) {
    Logger::getInstance().warning("NetworkManager::receiveControlMessage",
                                  "Invalid secure control message length.");
    connection.invalidate();
    return false;
  }

  std::vector<char> buffer(sizeof(uint32_t) + totalLength);
  std::memcpy(buffer.data(), &totalLengthNetwork, sizeof(totalLengthNetwork));
  if (!connection->recvAll(buffer.data() + sizeof(uint32_t), totalLength)) {
    connection.invalidate();
    return false;
  }

  if (!connection->hasSecureSessionId()) {
    Logger::getInstance().warning("NetworkManager::receiveControlMessage",
                                  "Secure session was not established.");
    connection.invalidate();
    return false;
  }

  try {
    const std::string secureKey =
        secureKeyForConnection(connection, securityPreSharedKey);
    SecurePacket::SessionMetadata metadata;
    message = SecurePacket::DecryptControlMessage(
        buffer, secureKey, connection->getSecureSessionId(), &metadata);
    if (!connection->acceptSecureInboundSequence(metadata.sequenceNumber)) {
      Logger::getInstance().warning("NetworkManager::receiveControlMessage",
                                    "Replayed or out-of-order secure control packet rejected.");
      connection.invalidate();
      return false;
    }
    return true;
  } catch (const std::exception &e) {
    Logger::getInstance().warning("NetworkManager::receiveControlMessage",
                                  "Failed to decrypt secure control message: " +
                                      std::string(e.what()));
    connection.invalidate();
    return false;
  }
}

// Sends one chunk to a specific target with retries and circuit-breaker checks.
NetworkManager::ChunkSendResult NetworkManager::sendChunkToTarget(
    TargetContext &target, const ChunkMetadata &chunk,
    const std::vector<char> &data, uint64_t chunkOffset, uint64_t totalFileSize,
    const std::atomic<bool> *is_running) {
  const std::string serverLabel =
      target.endpoint.host + ":" + std::to_string(target.endpoint.port);
  if (is_running != nullptr && !is_running->load()) {
    return ChunkSendResult::Failure;
  }
  if (!target.circuitBreaker->shouldAttempt()) {
    Logger::getInstance().warning(
        "NetworkManager::sendChunkToTarget",
        "Circuit breaker OPEN for " + serverLabel + ". Skipping chunk " +
            std::to_string(chunk.getChunkNumber()) + ".");
    if (target.adaptiveChunks) {
      target.adaptiveChunks->recordFailure();
    }
    return ChunkSendResult::Failure;
  }

  int currentRetry = 0;
  const std::string chunkFilename = chunk.getFilename();

  while (currentRetry < maxRetries) {
    if (is_running != nullptr && !is_running->load()) {
      return ChunkSendResult::Failure;
    }

    auto connectionWrapper = target.connectionPool->acquire();
    if (!connectionWrapper || !connectionWrapper->isValid()) {
      if (connectionWrapper) {
        connectionWrapper->invalidate();
      }
      currentRetry++;
      Logger::getInstance().warning(
          "NetworkManager::sendChunkToTarget",
          "Failed to acquire connection for " + serverLabel +
              " when sending chunk " + chunkFilename + ":" +
              std::to_string(chunk.getChunkNumber()) + " (attempt " +
              std::to_string(currentRetry) + "/" + std::to_string(maxRetries) +
              ").");
      if (target.adaptiveChunks) {
        target.adaptiveChunks->recordFailure();
      }
    } else {
      auto result =
          _sendChunkInternal(*connectionWrapper, target, chunk, data,
                             chunkOffset, totalFileSize);
      if (result == ChunkSendResult::Success) {
        target.circuitBreaker->reportSuccess();
        return ChunkSendResult::Success;
      }
      if (result == ChunkSendResult::AlreadyExists) {
        target.circuitBreaker->reportSuccess();
        connectionWrapper->invalidate();
        return ChunkSendResult::AlreadyExists;
      }

      Logger::getInstance().warning(
          "NetworkManager::sendChunkToTarget",
          "Send failed for " + serverLabel + " chunk " + chunkFilename + ":" +
              std::to_string(chunk.getChunkNumber()) + " on attempt " +
              std::to_string(currentRetry + 1) + "/" +
              std::to_string(maxRetries) + ". Retrying.");
      connectionWrapper->invalidate();
      currentRetry++;
    }

    if (currentRetry < maxRetries) {
      const int fixedDelaySeconds = 2;
      Logger::getInstance().info(
          "NetworkManager::sendChunkToTarget",
          "Waiting " + std::to_string(fixedDelaySeconds) +
              "s before retrying chunk " + chunkFilename + ":" +
              std::to_string(chunk.getChunkNumber()) + " for " + serverLabel +
              ".");
      if (is_running != nullptr) {
        if (!waitInterruptible(*is_running,
                               std::chrono::seconds(fixedDelaySeconds))) {
          return ChunkSendResult::Failure;
        }
      } else {
        std::this_thread::sleep_for(std::chrono::seconds(fixedDelaySeconds));
      }
    }
  }

  target.circuitBreaker->reportFailure();
  Logger::getInstance().error("NetworkManager::sendChunkToTarget",
                              "Exhausted retries for chunk " + chunkFilename +
                                  ":" + std::to_string(chunk.getChunkNumber()) +
                                  " on " + serverLabel + ".");
  return ChunkSendResult::Failure;
}

// Retries delivery for pending chunks and persists retry counters in the DB.
NetworkManager::ChunkSendResult NetworkManager::sendPendingChunkToTarget(
    TargetContext &target, const ChunkMetadata &chunk,
    const std::vector<char> &data, int &retryAccumulator, uint64_t chunkOffset,
    uint64_t totalFileSize, const std::atomic<bool> &is_running) {
  const std::string serverLabel =
      target.endpoint.host + ":" + std::to_string(target.endpoint.port);
  if (!is_running.load()) {
    return ChunkSendResult::Failure;
  }
  if (!target.circuitBreaker->shouldAttempt()) {
    Logger::getInstance().warning(
        "NetworkManager::sendPendingChunkToTarget",
        "Circuit breaker OPEN for " + serverLabel + ". Skipping chunk " +
            std::to_string(chunk.getChunkNumber()) + " from pending queue.");
    if (target.adaptiveChunks) {
      target.adaptiveChunks->recordFailure();
    }
    return ChunkSendResult::Failure;
  }

  int attemptsForTarget = 0;
  while (attemptsForTarget < maxRetries) {
    if (!is_running.load()) {
      return ChunkSendResult::Failure;
    }

    auto connectionWrapper = target.connectionPool->acquire();
    if (!connectionWrapper || !connectionWrapper->isValid()) {
      if (connectionWrapper) {
        connectionWrapper->invalidate();
      }

      attemptsForTarget++;
      retryAccumulator++;
      target.circuitBreaker->reportFailure();

      Logger::getInstance().warning(
          "NetworkManager::sendPendingChunkToTarget",
          "Failed to acquire connection for " + serverLabel +
              " when resending chunk " + chunk.getFilename() + ":" +
              std::to_string(chunk.getChunkNumber()) + " (attempt " +
              std::to_string(attemptsForTarget) + "/" +
              std::to_string(maxRetries) + ").");
      if (target.adaptiveChunks) {
        target.adaptiveChunks->recordFailure();
      }

      try {
        database.updateChunkRetries(chunk.getFilePath(), chunk.getChunkNumber(),
                                    retryAccumulator);
      } catch (const std::exception &db_ex) {
        Logger::getInstance().error(
            "NetworkManager::sendPendingChunkToTarget",
            "Failed to update retry count for chunk " +
                chunk.getFilename() + ":" +
                std::to_string(chunk.getChunkNumber()) +
                ". Error: " + std::string(db_ex.what()));
      }
    } else {
      auto sendResult =
          _sendChunkInternal(*connectionWrapper, target, chunk, data,
                             chunkOffset, totalFileSize);
      if (sendResult == ChunkSendResult::Success) {
        target.circuitBreaker->reportSuccess();
        return ChunkSendResult::Success;
      }
      if (sendResult == ChunkSendResult::AlreadyExists) {
        target.circuitBreaker->reportSuccess();
        connectionWrapper->invalidate();
        return ChunkSendResult::AlreadyExists;
      }

      Logger::getInstance().warning(
          "NetworkManager::sendPendingChunkToTarget",
          "Send attempt " + std::to_string(attemptsForTarget + 1) +
              " failed for " + serverLabel + " chunk " +
              chunk.getFilename() + ":" +
              std::to_string(chunk.getChunkNumber()) + ". Retrying.");
      connectionWrapper->invalidate();
      target.circuitBreaker->reportFailure();
      attemptsForTarget++;
      retryAccumulator++;

      try {
        database.updateChunkRetries(chunk.getFilePath(), chunk.getChunkNumber(),
                                    retryAccumulator);
      } catch (const std::exception &db_ex) {
        Logger::getInstance().error(
            "NetworkManager::sendPendingChunkToTarget",
            "Failed to update retry count for chunk " +
                chunk.getFilename() + ":" +
                std::to_string(chunk.getChunkNumber()) +
                ". Error: " + std::string(db_ex.what()));
      }
    }

    if (attemptsForTarget < maxRetries) {
      int delaySeconds = 2 * static_cast<int>(std::pow(2, attemptsForTarget));
      int jitterMilliseconds = 0;
      if (retryJitterMaxMillis > 0) {
        static thread_local std::mt19937 rng(std::random_device{}());
        std::uniform_int_distribution<int> dist(0, retryJitterMaxMillis);
        jitterMilliseconds = dist(rng);
      }
      auto totalDelay = std::chrono::seconds(delaySeconds) +
                        std::chrono::milliseconds(jitterMilliseconds);
      auto totalDelaySeconds =
          std::chrono::duration_cast<std::chrono::milliseconds>(totalDelay)
              .count() /
          1000.0;
      Logger::getInstance().info(
          "NetworkManager::sendPendingChunkToTarget",
          "Waiting " + std::to_string(totalDelaySeconds) +
              "s before retrying chunk " + chunk.getFilename() +
              ":" + std::to_string(chunk.getChunkNumber()) + " for " +
              serverLabel + ".");
      if (!waitInterruptible(
              is_running, std::chrono::duration_cast<std::chrono::milliseconds>(
                              totalDelay))) {
        return ChunkSendResult::Failure;
      }
    }
  }

  Logger::getInstance().error("NetworkManager::sendPendingChunkToTarget",
                              "Exhausted retries for pending chunk " +
                                  chunk.getFilename() + ":" +
                                  std::to_string(chunk.getChunkNumber()) +
                                  " on " + serverLabel + ".");
  return ChunkSendResult::Failure;
}

// Broadcasts one chunk to all targets and aggregates the final send outcome.
NetworkManager::ChunkSendResult
NetworkManager::sendChunk(const ChunkMetadata &chunk,
                          const std::vector<char> &data, uint64_t chunkOffset,
                          uint64_t totalFileSize,
                          const std::atomic<bool> *is_running) {
  if (targets.empty()) {
    Logger::getInstance().error("NetworkManager::sendChunk",
                                "No configured targets to send chunk " +
                                    std::to_string(chunk.getChunkNumber()) +
                                    ".");
    return ChunkSendResult::Failure;
  }

  bool anyFailure = false;
  bool allAlreadyExists = true;

  for (auto &target : targets) {
    if (is_running != nullptr && !is_running->load()) {
      return ChunkSendResult::Failure;
    }

    auto result = sendChunkToTarget(target, chunk, data, chunkOffset,
                                    totalFileSize, is_running);
    if (result == ChunkSendResult::Failure) {
      anyFailure = true;
    }
    if (result != ChunkSendResult::AlreadyExists) {
      allAlreadyExists = false;
    }
  }

  if (anyFailure) {
    return ChunkSendResult::Failure;
  }

  if (allAlreadyExists) {
    try {
      database.addProcessedFileAndCleanupChunks(chunk.getFilePath());
    } catch (const std::exception &db_ex) {
      Logger::getInstance().error("NetworkManager::sendChunk",
                                  "Failed to mark file as processed after all "
                                  "servers reported existing file: " +
                                      chunk.getFilePath().u8string() +
                                      ". Error: " + std::string(db_ex.what()));
    }
    return ChunkSendResult::AlreadyExists;
  }

  return ChunkSendResult::Success;
}

// Serializes one chunk message according to the wire protocol expected by the
// server.
std::vector<char> NetworkManager::serializeChunkMessage(
    const std::string &filename, const std::string &directoryPath,
    const std::string &serverAddress, const std::string &transferId,
    uint32_t chunkNumber,
    uint32_t totalChunks, uint64_t chunkOffset, uint64_t totalFileSize,
    const std::string &hash, const std::vector<char> &chunkData) {
  // Determine lengths for variable-size fields.
  uint32_t filenameLen = static_cast<uint32_t>(filename.size());
  uint32_t directoryPathLen = static_cast<uint32_t>(directoryPath.size());
  uint32_t serverAddressLen = static_cast<uint32_t>(serverAddress.size());
  uint32_t transferIdLen = static_cast<uint32_t>(transferId.size());
  uint32_t hashLen = static_cast<uint32_t>(hash.size());
  uint32_t dataLen = static_cast<uint32_t>(chunkData.size());

  // Compute full payload length (excluding outer length field itself).
  uint32_t contentLength =
      sizeof(uint32_t) + filenameLen + sizeof(uint32_t) + directoryPathLen +
      sizeof(uint32_t) + serverAddressLen +
      sizeof(uint32_t) + transferIdLen +                       // transferId
      sizeof(uint32_t) +                                       // chunkNumber
      sizeof(uint32_t) +                                       // totalChunks
      sizeof(uint64_t) +                                       // chunkOffset
      sizeof(uint64_t) +                                       // totalFileSize
      sizeof(uint32_t) + hashLen + sizeof(uint32_t) + dataLen;

  // Protocol starts with a 4-byte payload length header.
  uint32_t totalLengthField = contentLength;

  // Allocate buffer for the complete message.
  std::vector<char> buffer;
  buffer.resize(sizeof(uint32_t) + contentLength);
  size_t offset = 0;

  // Serialize totalLength field in network byte order.
  uint32_t totalLengthNetwork = htonl(totalLengthField);
  std::memcpy(buffer.data() + offset, &totalLengthNetwork,
              sizeof(totalLengthNetwork));
  offset += sizeof(totalLengthNetwork);

  // Serialize filename field.
  uint32_t filenameLenNetwork = htonl(filenameLen);
  std::memcpy(buffer.data() + offset, &filenameLenNetwork,
              sizeof(filenameLenNetwork));
  offset += sizeof(filenameLenNetwork);
  if (filenameLen > 0) {
    std::memcpy(buffer.data() + offset, filename.data(), filenameLen);
    offset += filenameLen;
  }

  // Serialize directoryPath field.
  uint32_t directoryPathLenNetwork = htonl(directoryPathLen);
  std::memcpy(buffer.data() + offset, &directoryPathLenNetwork,
              sizeof(directoryPathLenNetwork));
  offset += sizeof(directoryPathLenNetwork);
  if (directoryPathLen > 0) {
    std::memcpy(buffer.data() + offset, directoryPath.data(), directoryPathLen);
    offset += directoryPathLen;
  }

  // Serialize serverAddress field.
  uint32_t serverAddressLenNetwork = htonl(serverAddressLen);
  std::memcpy(buffer.data() + offset, &serverAddressLenNetwork,
              sizeof(serverAddressLenNetwork));
  offset += sizeof(serverAddressLenNetwork);
  if (serverAddressLen > 0) {
    std::memcpy(buffer.data() + offset, serverAddress.data(), serverAddressLen);
    offset += serverAddressLen;
  }

  // Serialize the stable content-derived transfer identity.
  uint32_t transferIdLenNetwork = htonl(transferIdLen);
  std::memcpy(buffer.data() + offset, &transferIdLenNetwork,
              sizeof(transferIdLenNetwork));
  offset += sizeof(transferIdLenNetwork);
  if (transferIdLen > 0) {
    std::memcpy(buffer.data() + offset, transferId.data(), transferIdLen);
    offset += transferIdLen;
  }

  // Serialize chunkNumber field.
  uint32_t chunkNumberNetwork = htonl(chunkNumber);
  std::memcpy(buffer.data() + offset, &chunkNumberNetwork,
              sizeof(chunkNumberNetwork));
  offset += sizeof(chunkNumberNetwork);

  // Serialize totalChunks field.
  uint32_t totalChunksNetwork = htonl(totalChunks);
  std::memcpy(buffer.data() + offset, &totalChunksNetwork,
              sizeof(totalChunksNetwork));
  offset += sizeof(totalChunksNetwork);

  // Serialize chunkOffset field as 2x uint32 in network byte order.
  uint32_t offsetHighNetwork = htonl(static_cast<uint32_t>(chunkOffset >> 32));
  uint32_t offsetLowNetwork =
      htonl(static_cast<uint32_t>(chunkOffset & 0xFFFFFFFFULL));
  std::memcpy(buffer.data() + offset, &offsetHighNetwork,
              sizeof(offsetHighNetwork));
  offset += sizeof(offsetHighNetwork);
  std::memcpy(buffer.data() + offset, &offsetLowNetwork,
              sizeof(offsetLowNetwork));
  offset += sizeof(offsetLowNetwork);

  // Serialize totalFileSize field as 2x uint32 in network byte order.
  uint32_t fileHighNetwork = htonl(static_cast<uint32_t>(totalFileSize >> 32));
  uint32_t fileLowNetwork =
      htonl(static_cast<uint32_t>(totalFileSize & 0xFFFFFFFFULL));
  std::memcpy(buffer.data() + offset, &fileHighNetwork,
              sizeof(fileHighNetwork));
  offset += sizeof(fileHighNetwork);
  std::memcpy(buffer.data() + offset, &fileLowNetwork,
              sizeof(fileLowNetwork));
  offset += sizeof(fileLowNetwork);

  // Serialize hash field.
  uint32_t hashLenNetwork = htonl(hashLen);
  std::memcpy(buffer.data() + offset, &hashLenNetwork, sizeof(hashLenNetwork));
  offset += sizeof(hashLenNetwork);
  if (hashLen > 0) {
    std::memcpy(buffer.data() + offset, hash.data(), hashLen);
    offset += hashLen;
  }

  // Serialize chunk payload field.
  uint32_t dataLenNetwork = htonl(dataLen);
  std::memcpy(buffer.data() + offset, &dataLenNetwork, sizeof(dataLenNetwork));
  offset += sizeof(dataLenNetwork);
  if (dataLen > 0) {
    std::memcpy(buffer.data() + offset, chunkData.data(), dataLen);
    offset += dataLen;
  }

  return buffer;
}

