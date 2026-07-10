#include "FileProcessor.h"
#include "ChunkPacketParser.h"
#include "RadarConfig.h"
#include "WatchedPathMapper.h"
#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <limits>
#include <utility>

namespace fs = std::filesystem;

#ifdef _WIN32
static std::wstring normalizePathElement(const fs::path &path) {
  std::wstring value = path.wstring();
  std::transform(value.begin(), value.end(), value.begin(),
                 [](wchar_t ch) {
                   return static_cast<wchar_t>(std::towlower(ch));
                 });
  return value;
}
#else
static std::string normalizePathElement(const fs::path &path) {
  return path.string();
}
#endif

static bool samePathElement(const fs::path &left, const fs::path &right) {
  return normalizePathElement(left) == normalizePathElement(right);
}

static bool pathStartsWith(const fs::path &root, const fs::path &candidate) {
  auto rootIt = root.begin();
  auto candidateIt = candidate.begin();
  for (; rootIt != root.end(); ++rootIt, ++candidateIt) {
    if (candidateIt == candidate.end() ||
        !samePathElement(*rootIt, *candidateIt)) {
      return false;
    }
  }
  return true;
}

static fs::path absoluteNormalPath(const fs::path &path) {
  std::error_code ec;
  fs::path absolute = fs::absolute(path, ec);
  if (ec) {
    absolute = path;
  }
  return absolute.lexically_normal();
}

static std::wstring processingLockKey(const fs::path &path) {
#ifdef _WIN32
  return normalizePathElement(absoluteNormalPath(path));
#else
  return absoluteNormalPath(path).wstring();
#endif
}

static fs::path weaklyCanonicalPath(const fs::path &path, std::error_code &ec) {
  fs::path canonical = fs::weakly_canonical(path, ec);
  if (ec) {
    return {};
  }
  return canonical.lexically_normal();
}

static bool isReparsePointOrUnknown(const fs::path &path) {
#ifdef _WIN32
  const DWORD attrs = GetFileAttributesW(path.c_str());
  if (attrs == INVALID_FILE_ATTRIBUTES) {
    return true;
  }
  return (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
  std::error_code ec;
  const auto status = fs::symlink_status(path, ec);
  return ec || fs::is_symlink(status);
#endif
}

static bool hasReparsePointInPath(const fs::path &root,
                                  const fs::path &candidate) {
  const fs::path rootAbs = absoluteNormalPath(root);
  const fs::path candidateAbs = absoluteNormalPath(candidate);
  if (!pathStartsWith(rootAbs, candidateAbs)) {
    return true;
  }

  fs::path current = rootAbs;
  if (isReparsePointOrUnknown(current)) {
    return true;
  }

  auto candidateIt = candidateAbs.begin();
  for (auto rootIt = rootAbs.begin(); rootIt != rootAbs.end(); ++rootIt) {
    if (candidateIt != candidateAbs.end()) {
      ++candidateIt;
    }
  }

  for (; candidateIt != candidateAbs.end(); ++candidateIt) {
    current /= *candidateIt;
    if (isReparsePointOrUnknown(current)) {
      return true;
    }
  }
  return false;
}

static bool isSafeConfiguredFilePath(const fs::path &filePath) {
  std::error_code ec;
  const fs::path canonicalFile = weaklyCanonicalPath(filePath, ec);
  if (ec) {
    Logger::getInstance().warning(
        "FileProcessor::processFile",
        "Rejected file because its path could not be canonicalized.");
    return false;
  }

  for (const auto &rootString : RadarConfig::getInstance().getDataDirs()) {
    const fs::path root(rootString);
    const fs::path canonicalRoot = weaklyCanonicalPath(root, ec);
    if (ec) {
      continue;
    }

    if (pathStartsWith(canonicalRoot, canonicalFile) &&
        !samePathElement(canonicalRoot, canonicalFile) &&
        !hasReparsePointInPath(root, filePath)) {
      const auto status = fs::symlink_status(filePath, ec);
      return !ec && fs::is_regular_file(status);
    }
  }

  Logger::getInstance().warning(
      "FileProcessor::processFile",
      "Rejected file outside configured data directories or through a link.");
  return false;
}
/**
 * Constructor
 * @param verifier File integrity verifier
 * @param db Database for storing chunk metadata
 * @param defaultChunkSize Default chunk size in bytes
 */
FileProcessor::FileProcessor(IFileIntegrityVerifier &verifier, Database &db,
                             DWORDLONG defaultChunkSize,
                             int stabilityCheckInterval,
                             int stabilityCheckCount,
                             int memoryUsagePercentLimit)
    : fileVerifier(verifier), database(db),
      stabilityCheckInterval(stabilityCheckInterval),
      stabilityCheckCount(stabilityCheckCount),
      memoryUsagePercentLimit(memoryUsagePercentLimit),
      defaultChunkSize(defaultChunkSize),
      stabilityChecker(stabilityCheckInterval, stabilityCheckCount) {}
/**
 * Process a file by splitting it into chunks
 * @param filePath Path to the file to process
 * @param chunkProcessorFunc Function called for each chunk
 * @return true if file was processed successfully, false otherwise
 */
bool FileProcessor::processFile(
    const fs::path &filePath,
    std::function<FileProcessor::ChunkProcessingResult(
        const ChunkMetadata &, const std::vector<char> &, uint64_t, uint64_t)>
        chunkProcessorFunc) {
  std::string filename = filePath.filename().u8string();
  std::shared_ptr<std::mutex> activeFileMutex;
  {
    std::lock_guard<std::mutex> lock(activeFileMutexesMutex_);
    auto &entry = activeFileMutexes_[processingLockKey(filePath)];
    if (!entry) {
      entry = std::make_shared<std::mutex>();
    }
    activeFileMutex = entry;
  }
  std::unique_lock<std::mutex> activeFileLock(*activeFileMutex);

  try {
    if (!isSafeConfiguredFilePath(filePath)) {
      throw std::runtime_error("Unsafe file path rejected: " + filename);
    }

    // Wait for file to stabilize
    stabilityChecker.waitForFileStability(filePath);

    if (!isSafeConfiguredFilePath(filePath)) {
      throw std::runtime_error("Unsafe file path rejected after stability check: " +
                               filename);
    }

    // Open the file in binary mode
    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) {
      std::string errorMsg = "Error opening file: " + filename;
      Logger::getInstance().error("FileProcessor::processFile",
                                  "Error opening file: " + filename);
      throw std::runtime_error(errorMsg);
    }

    // Determine file size
    file.seekg(0, std::ios::end);
    std::streamsize fileSize = file.tellg();
    if (fileSize <= 0) {
      std::string errorMsg =
          "File is empty or size could not be determined: " + filename;
      Logger::getInstance().error("FileProcessor::processFile", errorMsg);
      throw std::runtime_error(errorMsg);
    }
    file.seekg(0, std::ios::beg);

    const auto &config = RadarConfig::getInstance();
    const std::string transferId =
        fileVerifier.calculateFileHash(filePath.u8string());
    if (transferId.size() != ChunkPacketParser::kTransferIdHexLength) {
      throw std::runtime_error("Error calculating transfer identity: " +
                               filename);
    }
    if (config.isChangedFilesResendEnabled()) {
      if (config.getChangedFilesIdentity() != "sha256") {
        throw std::runtime_error("Unsupported changed-file identity: " +
                                 config.getChangedFilesIdentity());
      }

      const int64_t lastWriteTime =
          fs::last_write_time(filePath).time_since_epoch().count();
      const bool shouldProcess = database.markFileAsProcessingIfChanged(
          filePath, static_cast<int64_t>(fileSize), lastWriteTime, transferId);
      if (!shouldProcess) {
        Logger::getInstance().info(
            "FileProcessor::processFile",
            "File content already processed, skipping resend: " + filename);
        return true;
      }
    } else {
      if (database.isFileFullyProcessed(filePath)) {
        Logger::getInstance().info(
            "FileProcessor::processFile",
            "File path already processed and changed-file resend is disabled: " +
                filename);
        return true;
      }
      database.markFileAsProcessing(filePath);
    }

    database.deleteFileChunks(filePath);

    // Calculate optimal chunk size
    DWORDLONG chunkSize = MemoryManager::calculateOptimalChunkSize(
        fileSize, defaultChunkSize, memoryUsagePercentLimit);

    // Process the file
    return splitFile(file, filePath, fileSize, chunkSize, transferId,
                     chunkProcessorFunc);
  } catch (const std::exception &e) {
    Logger::getInstance().error("FileProcessor",
                                "Exception processing file: " + filename +
                                    ": " + std::string(e.what()));
    return false;
  }
}
/**
 * Check if a file has been processed
 * @param filePath Path to the file
 * @return true if file is fully processed, false otherwise
 */
bool FileProcessor::isFileFullyProcessed(const fs::path &filePath) {
  return database.isFileFullyProcessed(filePath);
}

void FileProcessor::setDynamicChunkSizeProvider(
    std::function<DWORDLONG(uint64_t)> chunkSizeProvider) {
  dynamicChunkSizeProvider = std::move(chunkSizeProvider);
}

/**
 * Split a file into chunks
 * @param file Open file stream
 * @param filePath Path to the file
 * @param fileSize Size of the file
 * @param chunkSize Size of each chunk
 * @param chunkProcessorFunc Function called for each chunk
 * @return true if all chunks were processed successfully
 */
bool FileProcessor::splitFile(
    std::ifstream &file, const fs::path &filePath, std::streamsize fileSize,
    DWORDLONG chunkSize, const std::string &transferId,
    std::function<FileProcessor::ChunkProcessingResult(
        const ChunkMetadata &, const std::vector<char> &, uint64_t, uint64_t)>
        chunkProcessorFunc) {
  // Extract filepath from path
  std::string filepath = filePath.u8string();
  std::string relativeDirectory;
  WatchedPathMapper::TryBuildRelativeDirectory(
      filePath, RadarConfig::getInstance().getDataDirs(), relativeDirectory);
  bool success = true;

  try {
    constexpr DWORDLONG minChunkSize = 32ULL * 1024ULL;
    constexpr DWORDLONG maxChunkSize = 4ULL * 1024ULL * 1024ULL;
    const bool dynamicChunking = static_cast<bool>(dynamicChunkSizeProvider);

    // Fixed mode keeps the original total count semantics. Dynamic mode uses
    // total_chunk=0 until the final chunk, where the actual count is known.
    int totalChunks = 0;
    if (!dynamicChunking) {
      totalChunks = static_cast<int>(fileSize / chunkSize +
                                     ((fileSize % chunkSize) ? 1 : 0));
    }
    Logger::getInstance().info(
        "FileProcessor::splitFile",
        "Filepath: " + filepath + " Size: " + std::to_string(fileSize) +
            " bytes; Chunk mode: " +
            (dynamicChunking ? "adaptive" : "fixed") +
            (dynamicChunking ? "" : "; Total number of chunks: " +
                                      std::to_string(totalChunks)));

    // Process each chunk
    int chunkNumber = 0;
    uint64_t currentOffset = 0;
    const uint64_t totalFileSize = static_cast<uint64_t>(fileSize);
    while (success && file && currentOffset < totalFileSize) {
      uint64_t remainingBytes = totalFileSize - currentOffset;
      DWORDLONG readChunkSize = chunkSize;
      if (dynamicChunking) {
        readChunkSize = dynamicChunkSizeProvider(remainingBytes);
        if (readChunkSize == 0) {
          readChunkSize = chunkSize;
        }
      }
      readChunkSize =
          (std::max)(minChunkSize, (std::min)(readChunkSize, maxChunkSize));
      readChunkSize =
          (std::min)(readChunkSize, static_cast<DWORDLONG>(remainingBytes));
      if (readChunkSize == 0 ||
          readChunkSize >
              static_cast<DWORDLONG>(
                  (std::numeric_limits<std::streamsize>::max)())) {
        Logger::getInstance().critical(
            "FileProcessor::splitFile",
            "Invalid chunk size selected: " + std::to_string(readChunkSize));
        success = false;
        break;
      }

      std::vector<char> buffer(static_cast<size_t>(readChunkSize));

      // Read a chunk of data
      std::streamsize bytesRead =
          file.read(buffer.data(), static_cast<std::streamsize>(readChunkSize))
              .gcount();
      if (bytesRead <= 0) {
        break; // No more data to read
      }

      // Resize buffer to actual bytes read
      buffer.resize(bytesRead);

      // Calculate hash of the chunk
      std::string hash = fileVerifier.calculateChunkHash(buffer);
      if (hash.empty()) {
        Logger::getInstance().critical("FileProcessor::splitFile",
                                       "Error calculating hash for chunk " +
                                           std::to_string(chunkNumber));
        success = false;
        break;
      }

      const uint64_t chunkOffset = currentOffset;
      currentOffset += static_cast<uint64_t>(bytesRead);
      const bool isFinalChunk = currentOffset >= totalFileSize;
      const int totalChunksForPacket =
          dynamicChunking ? (isFinalChunk ? chunkNumber + 1 : 0) : totalChunks;

      // Create metadata for this chunk.
      ChunkMetadata chunk(filePath.filename(), chunkNumber, totalChunksForPacket,
                          static_cast<int>(bytesRead), hash, filePath, 0);
      chunk.setTransferId(transferId);
      chunk.setDirectory(relativeDirectory);
      chunk.setChunkOffset(chunkOffset);
      chunk.setTotalFileSize(totalFileSize);

      // Process the chunk - Network::sendChunk(chunk, buffer);
      ChunkProcessingResult sendResult =
          chunkProcessorFunc(chunk, buffer, chunkOffset, totalFileSize);
      bool chunkSent = (sendResult == ChunkProcessingResult::Sent);
      bool skipRemainingFile =
          (sendResult == ChunkProcessingResult::SkipRemainingFile);

      if (sendResult == ChunkProcessingResult::Failed) {
        Logger::getInstance().warning("FileProcessor::splitFile",
                                      "Error sending chunk " +
                                          std::to_string(chunkNumber) +
                                          " of filepath " + filepath);
      } else if (skipRemainingFile) {
        Logger::getInstance().info("FileProcessor::splitFile",
                                   "Server already has file for '" + filepath +
                                       "'. Stopping further chunk processing.");
      }

      // Store chunk metadata in database
      if (!skipRemainingFile) {
        try {
          database.insertChunk(filePath.filename(), chunkNumber,
                               totalChunksForPacket,
                               static_cast<int>(bytesRead),
                               static_cast<int>(bytesRead), hash, filePath,
                               chunkSent ? "success" : "failed", chunkOffset,
                               transferId);
        } catch (const std::exception &e) {
          Logger::getInstance().critical("FileProcessor::splitFile",
                                         "Error inserting chunk in database: " +
                                             std::string(e.what()));
          success = false;
          break;
        }
      }

      if (!skipRemainingFile) {
        Logger::getInstance().info("FileProcessor::splitFile",
                                   "Chunk " + std::to_string(chunkNumber) +
                                       " created for filepath " + filepath);
      }

      // Prepare for next chunk
      ++chunkNumber;
      if (skipRemainingFile) {
        break;
      }
    }
  } catch (const std::exception &e) {
    Logger::getInstance().error("FileProcessor::splitFile",
                                "Exception splitting file: " + filepath + ": " +
                                    std::string(e.what()));
    success = false;
  }

  if (!success) {
    Logger::getInstance().error("FileProcessor::splitFile",
                                "Error processing filepath: " + filepath);
  }

  return success;
}

/**
 * Checks if a file is currently being processed.
 *
 * @param filePath The path of the file to check.
 *
 * @return True if the file is being processed, false otherwise.
 *
 * @throws None
 */
bool FileProcessor::isFileBeingProcessed(const fs::path &filePath) {
  return database.isFileProcessing(filePath);
}
/**
 * Marks a file as being processed.
 *
 * @param filePath Path to the file being processed.
 *
 * @return None.
 *
 * @throws None.
 */
void FileProcessor::markAsProcessing(const fs::path &filePath) {
  database.markFileAsProcessing(filePath);
}
/**
 * Retrieves a list of files that have failed processing and need to be retried.
 *
 * @return A vector of file paths that need to be retried.
 *
 * @throws None.
 */
std::vector<fs::path> FileProcessor::getFilesToRetry() {
  return database.getFailedFiles();
}
/**
 * Removes a file from the retry list.
 *
 * @param filePath The path of the file to be removed from the retry list.
 *
 * @return void
 *
 * @throws None
 */
void FileProcessor::removeFileFromRetry(const fs::path &filePath) {
  std::lock_guard<std::mutex> lock(failedFilesMutex_);
  failedFileRetries_.erase(filePath.wstring());
}

/**
 * Marks a file as failed by adding it to the set of files in failed state.
 *
 * @param filePath The path of the file that failed processing.
 *
 * @return None.
 */
void FileProcessor::markAsFileFailed(const fs::path &filePath) {
  std::lock_guard<std::mutex> lock(failedFilesMutex_);

  int currentRetries = 0;
  auto it = failedFileRetries_.find(filePath.wstring());
  if (it != failedFileRetries_.end()) {
    currentRetries = it->second;
  }

  currentRetries++;

  try {
    database.markFileAsFailed(filePath);
  } catch (const std::exception &) {
    Logger::getInstance().critical(
        "FileProcessor", "Failed to mark file '{}' as failed in database.",
        filePath.string());
  }

  int maxFileProcessing =
      RadarConfig::getInstance().getMaxFileProcessingAttempts();

  if (currentRetries >= maxFileProcessing) {
    Logger::getInstance().info(
        "FileProcessor",
        "File '{}' has exceeded the limit of {} attempts. Aborting processing.",
        filePath.string(), maxFileProcessing);
    failedFileRetries_.erase(filePath.wstring());

    try {
      database.logPermanentlyFailedFile(filePath, "Retry limit exceeded.");
    } catch (const std::exception &) {
      Logger::getInstance().critical(
          "FileProcessor", "Failed to log permanent failure for file '{}'.",
          filePath.string());
    }
  } else {
    Logger::getInstance().warning(
        "FileProcessor", "Failed to process file '{}'. Attempt {} of {}.",
        filePath.string(), currentRetries, maxFileProcessing);
    failedFileRetries_[filePath.wstring()] = currentRetries;
  }
}
