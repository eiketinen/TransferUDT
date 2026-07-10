#pragma once
#include "ChunkMetadata.h"
#include "Database.h"
#include "FileStabilityChecker.h"
#include "Interfaces.h"
#include "Logger.h"
#include "MemoryManager.h"
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>


/**
 * Class responsible for processing files, splitting them into chunks,
 * and maintaining metadata
 */
class FileProcessor {
public:
  enum class ChunkProcessingResult {
    Sent,             /**< Chunk was transmitted successfully */
    Failed,           /**< Chunk transmission failed */
    SkipRemainingFile /**< Server already has the file; stop sending further
                         chunks */
  };

  FileProcessor(IFileIntegrityVerifier &verifier, Database &db,
                DWORDLONG defaultChunkSize = 10 * 1024 * 1024, // 10 MB
                int stabilityCheckInterval = 5, int stabilityCheckCount = 3,
                int memoryUsagePercentLimit = 70);

  bool processFile(
      const fs::path &filePath,
      std::function<ChunkProcessingResult(
          const ChunkMetadata &, const std::vector<char> &, uint64_t, uint64_t)>
          chunkProcessorFunc);

  void setDynamicChunkSizeProvider(
      std::function<DWORDLONG(uint64_t)> chunkSizeProvider);

  bool isFileFullyProcessed(const fs::path &filePath);
  bool isFileBeingProcessed(const fs::path &filePath);
  void markAsProcessing(const fs::path &filePath);
  void markAsProcessed(const fs::path &filePath);
  void markAsFileFailed(const fs::path &filePath);

  void removeFileFromRetry(const fs::path &filePath);

  std::vector<fs::path> getFilesToRetry();

private:
  // File integrity verifier
  IFileIntegrityVerifier &fileVerifier;

  // Database reference
  Database &database;

  // Default chunk size
  DWORDLONG defaultChunkSize;

  // File stability checker
  FileStabilityChecker stabilityChecker;

  int stabilityCheckInterval;

  int stabilityCheckCount;

  int memoryUsagePercentLimit;

  std::function<DWORDLONG(uint64_t)> dynamicChunkSizeProvider;

  mutable std::mutex failedFilesMutex_;
  std::map<std::wstring, int> failedFileRetries_;
  mutable std::mutex activeFileMutexesMutex_;
  std::map<std::wstring, std::shared_ptr<std::mutex>> activeFileMutexes_;

  bool splitFile(
      std::ifstream &file, const fs::path &filePath, std::streamsize fileSize,
      DWORDLONG chunkSize, const std::string &transferId,
      std::function<ChunkProcessingResult(
          const ChunkMetadata &, const std::vector<char> &, uint64_t, uint64_t)>
          chunkProcessorFunc);
};
