#include "FileProcessor.h"
#include "RadarConfig.h"
#include <algorithm>
#include <cwctype>

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

    // Calculate optimal chunk size
    DWORDLONG chunkSize = MemoryManager::calculateOptimalChunkSize(
        fileSize, defaultChunkSize, memoryUsagePercentLimit);

    // Process the file
    return splitFile(file, filePath, fileSize, chunkSize, chunkProcessorFunc);
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
    DWORDLONG chunkSize,
    std::function<FileProcessor::ChunkProcessingResult(
        const ChunkMetadata &, const std::vector<char> &, uint64_t, uint64_t)>
        chunkProcessorFunc) {
  // Extract filepath from path
  std::string filepath = filePath.u8string();
  bool success = true;

  try {
    // Calculate total number of chunks
    int totalChunks = static_cast<int>(fileSize / chunkSize +
                                       ((fileSize % chunkSize) ? 1 : 0));
    Logger::getInstance().info(
        "FileProcessor::splitFile",
        "Filepath: " + filepath + " Size: " + std::to_string(fileSize) +
            " bytes; Total number of chunks: " + std::to_string(totalChunks));

    // Buffer for reading file
    std::vector<char> buffer(chunkSize);

    // Process each chunk
    int chunkNumber = 0;
    while (success && file) {
      // Read a chunk of data
      std::streamsize bytesRead =
          file.read(buffer.data(), static_cast<std::streamsize>(chunkSize))
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

      // Create metadata for this chunk
      ChunkMetadata chunk(filePath.filename(), chunkNumber, totalChunks,
                          static_cast<int>(chunkSize), hash, filePath, 0);

      uint64_t chunkOffset =
          static_cast<uint64_t>(chunkNumber) * static_cast<uint64_t>(chunkSize);
      uint64_t totalFileSize = static_cast<uint64_t>(fileSize);

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
          database.insertChunk(filePath.filename(), chunkNumber, totalChunks,
                               static_cast<int>(bytesRead),
                               static_cast<int>(chunkSize), hash, filePath,
                               chunkSent ? "success" : "failed");
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
      buffer.resize(chunkSize);
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
