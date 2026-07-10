#include "RadarAgent.h"
#include <chrono>
#include <filesystem>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
/**
 * Initializes a RadarAgent instance with the given database and file integrity
 * verifier.
 *
 * This constructor creates and configures the necessary components, including
 * the file watcher, file processor, network manager, and thread pool.
 *
 * @param database The database instance to use for storing and retrieving data.
 * @param verifier The file integrity verifier instance to use for verifying
 * file integrity.
 *
 * @return None
 *
 * @throws None
 */
RadarAgent::RadarAgent(Database &database, IFileIntegrityVerifier &verifier)
    : database(database), fileVerifier(verifier), running(false) {

  auto directoriesToWatch = RadarConfig::getInstance().getDataDirs();
  if (directoriesToWatch.empty()) {
    Logger::getInstance().error(
        "RadarAgent::init",
        "No directories configured to watch. Please check your configuration.");
    throw std::runtime_error("No directories configured to watch");
  }
  fileWatchers.clear();

  for (const auto &dir : directoriesToWatch) {
    fileWatchers.emplace_back(std::make_unique<FileWatcher>(
        dir, RadarConfig::getInstance().getWatcherCheckInterval()));
    Logger::getInstance().info("RadarAgent::init",
                               "Configured to watch directory: " + dir);
  }

  fileProcessor = std::make_unique<FileProcessor>(
      fileVerifier, database, RadarConfig::getInstance().getChunkSize(),
      RadarConfig::getInstance().getStabilityCheckInterval(),
      RadarConfig::getInstance().getStabilityCheckCount(),
      RadarConfig::getInstance().getMemoryUsagePercentLimit());

  std::vector<NetworkManager::Endpoint> networkEndpoints;
  for (const auto &endpoint : RadarConfig::getInstance().getServerEndpoints()) {
    networkEndpoints.push_back({endpoint.host, endpoint.port});
  }

  networkManager = std::make_unique<NetworkManager>(
      networkEndpoints, database, RadarConfig::getInstance().getMaxRetries(),
      RadarConfig::getInstance().getCBFailureThreshold(),
      RadarConfig::getInstance().getCBResetTimeoutSeconds(),
      RadarConfig::getInstance().getPoolSize(),
      RadarConfig::getInstance().getMaxBandwidth(),
      RadarConfig::getInstance().getSegmentSize(),
      RadarConfig::getInstance().getSendBuffer(),
      RadarConfig::getInstance().getReceiveBuffer(),
      RadarConfig::getInstance().getSendTimeout(),
      RadarConfig::getInstance().getReceiveTimeout(),
      RadarConfig::getInstance().getRetryJitterMaxMillis(),
      RadarConfig::getInstance().isUDTGreetingRequired(),
      RadarConfig::getInstance().getUDTExpectedGreeting(),
      RadarConfig::getInstance().getUDTGreetingTimeoutMs(),
      RadarConfig::getInstance().isUDTKeepAliveEnabled(),
      RadarConfig::getInstance().getUDTKeepAliveIntervalSeconds(),
      RadarConfig::getInstance().getUDTKeepAlivePayload(),
      RadarConfig::getInstance().isSecurityEnabled(),
      RadarConfig::getInstance().getSecurityPreSharedKey(),
      RadarConfig::getInstance().isSecurityHandshakeEnabled(),
      RadarConfig::getInstance().getSecurityClientId(),
      RadarConfig::getInstance().getSecurityIdentityMode(),
      RadarConfig::getInstance().getSecurityClientPrivateKeyPath(),
      RadarConfig::getInstance().getSecurityServerPublicKeyPath(),
      RadarConfig::getInstance().getSecurityClientCertificatePath(),
      RadarConfig::getInstance().getSecurityCaBundlePath(),
      RadarConfig::getInstance().getSecurityCrlPath(),
      RadarConfig::getInstance().getSecurityCertificateExpiryWarningDays(),
      RadarConfig::getInstance().getSecurityServerIdentity(),
      RadarConfig::getInstance().isAdaptiveChunkEnabled(),
      RadarConfig::getInstance().getAdaptiveChunkMinBytes(),
      RadarConfig::getInstance().getAdaptiveChunkMaxBytes(),
      RadarConfig::getInstance().getAdaptiveChunkInitialBytes(),
      RadarConfig::getInstance().getAdaptiveChunkTargetAckMillis());

  if (RadarConfig::getInstance().isAdaptiveChunkEnabled()) {
    fileProcessor->setDynamicChunkSizeProvider(
        [this](uint64_t remainingBytes) -> DWORDLONG {
          if (!networkManager) {
            return 0;
          }
          return static_cast<DWORDLONG>(
              networkManager->getRecommendedChunkSize(remainingBytes));
        });
  }

  dashboardHeartbeatClient =
      std::make_unique<DashboardHeartbeatClient>(database,
                                                 RadarConfig::getInstance());

  // Create thread pool with configured number of threads
  int numThreads = RadarConfig::getInstance().getNumThreads();
  threadPool = std::make_unique<ThreadPool>(numThreads);

  Logger::getInstance().info(
      "RadarAgent::init",
      "Agent initialized with " + std::to_string(numThreads) + " workers and " +
          std::to_string(networkEndpoints.size()) + " network target(s)");
}
/**
 * Starts the RadarAgent, initiating its main functionality.
 *
 * This includes checking if the agent is already running, starting a separate
 * thread for sending pending chunks, and beginning the file watcher with a
 * callback for handling new files.
 *
 * @return None
 *
 * @throws None
 */
void RadarAgent::start() {
  // Check if already running
  if (running.exchange(true)) {
    Logger::getInstance().warning("RadarAgent::start",
                                  "Agent is already running");
    return;
  }
  // Start sending pending chunks in a separate thread
  pendingChunksThread = std::thread(&RadarAgent::pendingChunksThreadFunc, this);
  if (dashboardHeartbeatClient) {
    dashboardHeartbeatClient->start();
  }

  // Start file watcher with callback to our handleNewFile method
  for (auto &watcher : fileWatchers) {

    if (!running)
      break; // Sai se o serviÃ§o foi parado

    watcher->startWatching([this](const fs::path &filePath) {
      // Check if file is already being processed
      if (fileProcessor->isFileBeingProcessed(filePath)) {
        Logger::getInstance().info("RadarAgent::handleNewFile",
                                   "File is already being processed; deferring "
                                   "change check: " +
                                       filePath.string());
        threadPool->addTask([this, filePath]() {
          constexpr int maxWaitAttempts = 240;
          for (int attempt = 0;
               running && attempt < maxWaitAttempts &&
               fileProcessor->isFileBeingProcessed(filePath);
               ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
          }

          if (!running) {
            return;
          }

          this->handleNewFile(filePath);
        });
        return;
      }

      this->handleNewFile(filePath);
    });
  }

  Logger::getInstance().info("RadarAgent::start", "Agent started successfully");
}
/**
 * Stops the RadarAgent, halting all its activities and releasing resources.
 *
 * This method checks if the agent is already stopped, and if not, it stops the
 * file watcher, waits for the pending chunks thread to finish, and stops the
 * thread pool.
 *
 * @return None
 *
 * @throws None
 */
void RadarAgent::stop() {
  // Check if already stopped
  if (!running.exchange(false)) {
    return; // JÃ¡ estava parado
  }

  // Wake the pending thread immediately to avoid waiting the full poll
  // interval.
  shutdownCv.notify_all();

  if (dashboardHeartbeatClient) {
    dashboardHeartbeatClient->stop();
  }

  // Stop file watcher
  for (auto &watcher : fileWatchers) {
    watcher->stopWatching();
  }

  // Wait for pending chunks thread to finish
  if (pendingChunksThread.joinable()) {
    pendingChunksThread.join();
  }

  // Stop thread pool
  threadPool->stop();

  Logger::getInstance().info("RadarAgent::stop", "Agent stopped successfully");
}
/**
 * Handles a new file by checking if it's already being processed, marking it as
 * being processed, and adding a task to the thread pool to process the file and
 * send chunks to the server.
 *
 * @param filePath The path to the new file.
 *
 * @return None
 *
 * @throws std::exception If an error occurs while queuing the file for
 * processing or processing the file.
 */
void RadarAgent::handleNewFile(const fs::path &filePath) {

  std::string filepath = filePath.u8string();

  // Add task to thread pool
  try {
    threadPool->addTask([this, filePath]() {
      std::string filepath = filePath.u8string();
      Logger::getInstance().info("RadarAgent::handleNewFile",
                                 "Starting file processing: " + filepath);

      // Process the file and send chunks
      bool success = fileProcessor->processFile(
          filePath,
          [this](const ChunkMetadata &chunk, const std::vector<char> &data,
                 uint64_t chunkOffset, uint64_t totalFileSize) {
            NetworkManager::ChunkSendResult sendResult =
                networkManager->sendChunk(chunk, data, chunkOffset,
                                          totalFileSize, &running);
            switch (sendResult) {
            case NetworkManager::ChunkSendResult::Success:
              return FileProcessor::ChunkProcessingResult::Sent;
            case NetworkManager::ChunkSendResult::AlreadyExists:
              return FileProcessor::ChunkProcessingResult::SkipRemainingFile;
            default:
              return FileProcessor::ChunkProcessingResult::Failed;
            }
          });
      if (success) {
        Logger::getInstance().info("RadarAgent::handleNewFile",
                                   "File processing completed: " + filepath);
      } else {
        Logger::getInstance().error("RadarAgent::handleNewFile",
                                    "File processing failed: " + filepath);
        fileProcessor->markAsFileFailed(filePath);
      }
    });
  } catch (const std::exception &e) {
    Logger::getInstance().error("RadarAgent::handleNewFile",
                                "Error queuing file for processing: " +
                                    std::string(e.what()));
    fileProcessor->markAsFileFailed(filePath);
  }
}
/**
 * Runs the pending chunks thread, continuously sending pending chunks and
 * handling any exceptions that occur.
 *
 * @return None
 *
 * @throws std::exception Any exception that occurs during the sending of
 * pending chunks.
 */
void RadarAgent::pendingChunksThreadFunc() {
  Logger::getInstance().info("RadarAgent::pendingChunksThreadFunc",
                             "Pending chunks thread started");

  while (running) {
    try {
      Logger::getInstance().info("RadarAgent::pendingChunksThreadFunc",
                                 "Checking files for reprocessing...");
      std::vector<fs::path> filesToRetry = fileProcessor->getFilesToRetry();

      if (!filesToRetry.empty()) {
        Logger::getInstance().info("RadarAgent::pendingChunksThreadFunc",
                                   "Found {} file(s) for retry.",
                                   filesToRetry.size());
        for (const auto &filePath : filesToRetry) {

          if (!running)
            break; // Sai se o serviÃ§o foi parado

          if (fileProcessor->isFileBeingProcessed(filePath)) {
            Logger::getInstance().warning(
                "RadarAgent::pendingChunksThreadFunc",
                "Skipping retry for {} as it is already being processed.",
                filePath.string());
            fileProcessor->removeFileFromRetry(filePath);
            continue;
          }

          if (fileProcessor->isFileFullyProcessed(filePath)) {
            Logger::getInstance().warning(
                "RadarAgent::pendingChunksThreadFunc",
                "Skipping retry for {} as it is already Fully processed",
                filePath.string());
            fileProcessor->removeFileFromRetry(filePath);
            continue;
          }

          handleNewFile(filePath);
        }
      }

      int maxRetryAbandon = RadarConfig::getInstance().getMaxRetryAbandoned();

      // Send pending chunks
      networkManager->sendPendingFailedChunks(running, fileProcessor,
                                              maxRetryAbandon);

      cleanupSentFiles();

      if (!running)
        break;

    } catch (const std::exception &e) {
      Logger::getInstance().error("RadarAgent::pendingChunksThreadFunc",
                                  "Error in pending chunks thread: " +
                                      std::string(e.what()));
    }

    // Replace sleep_for with a condition-variable wait to allow prompt
    // shutdown.
    std::unique_lock<std::mutex> lock(shutdownMutex);
    shutdownCv.wait_for(
        lock,
        std::chrono::seconds(
            RadarConfig::getInstance().getPendingCheckInterval()),
        [this] { return !running.load(); });
  }

  Logger::getInstance().info("RadarAgent::pendingChunksThreadFunc",
                             "Pending chunks thread stopped");
}
/**
 * Destructor for the RadarAgent class.
 *
 * Stops the agent if it is currently running.
 *
 * @return None
 *
 * @throws None
 */
RadarAgent::~RadarAgent() { stop(); }
/**
 * Cleans up fully sent files by adding them to the processed files and cleaning
 * up chunks.
 *
 * @throws std::exception if an error occurs during the cleanup process.
 */
void RadarAgent::cleanupSentFiles() {
  try {
    std::vector<fs::path> filesToClean = database.getFullySentFiles();
    if (filesToClean.empty()) {
      return; // Nenhum arquivo para limpar, sai mais cedo.
    }

    Logger::getInstance().info("RadarAgent::cleanup",
                               "Found " + std::to_string(filesToClean.size()) +
                                   " fully sent file(s) to clean up.");

    for (const auto &file_path : filesToClean) {
      try {
        database.addProcessedFileAndCleanupChunks(file_path);
      } catch (const std::exception &e) {
        Logger::getInstance().error("RadarAgent::cleanup",
                                    "Failed to clean up file " +
                                        file_path.u8string() +
                                        ". Error: " + std::string(e.what()));
      }
    }
  } catch (const std::exception &e) {
    Logger::getInstance().error(
        "RadarAgent::cleanup",
        "An error occurred during the cleanup process: " +
            std::string(e.what()));
  }
}
