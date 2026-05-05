#pragma once

#include "ChunkMetadata.h"
#include "Database.h"
#include "Interfaces.h" // For IFileIntegrityVerifier
#include "Logger.h"
#include "ResourceManagers.h" // For RAII wrappers
#include "ServerConfig.h"
#include "ThreadPool.h" // For async reconstruction
#include <algorithm>    // for std::sort
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#include <windows.h>

namespace fs = std::filesystem;

class FileReceiver {
public:
  enum class ReceiveResult {
    Stored,           /**< Chunk processed and stored successfully */
    AlreadyCompleted, /**< File already reconstructed on server; skip further
                         chunks */
    Failed,           /**< Chunk processing failed */
    JustCompleted /**< The file has just finished receiving its last chunk */
  };
  /**
   * @brief Constructor for FileReceiver.
   * @param db Reference to the Database instance.
   * @param verifier Unique pointer to a file integrity verifier implementation.
   * @param config Reference to the ServerConfig instance.
   * @param pool Reference to the ThreadPool for asynchronous tasks.
   */
  FileReceiver(Database &db,
               std::unique_ptr<IFileIntegrityVerifier> verifierArg,
               ServerConfig &config, ThreadPool &pool);

  /**
   * @brief Receives and processes a single file chunk.
   * Verifies hash, saves the chunk to disk, and updates the database.
   * @param meta Metadata of the received chunk.
   * @param chunkData Raw byte data of the chunk.
   * @return ReceiveResult indicating whether the chunk was stored, should skip
   * further chunks, or failed.
   */
  ReceiveResult receiveChunk(const ChunkMetadata &meta,
                             const std::vector<char> &chunkData);

private:
  Database &database;
  std::unique_ptr<IFileIntegrityVerifier> verifier;
  ServerConfig &config;
  ThreadPool &threadPool;
  fs::path storagePath;
  fs::path reconstructedPath;
  std::mutex reconstructionMutex; // Mutex to protect reconstruction triggering
                                  // per file
  std::unordered_map<std::string, bool>
      reconstructionStatus; // Tracks files being/already reconstructed

  std::mutex
      remainingChunksMutex; // Mutex for atomic operations on remaining chunks
  std::unordered_map<std::string, int>
      remainingChunks; // key: client+filename, value: remaining chunks count

  std::mutex fileWriteMutexesMutex;
  std::unordered_map<std::string, std::unique_ptr<std::mutex>> fileWriteMutexes;
  std::mutex storageBudgetMutex;

  std::mutex &getFileMutex(const std::string &key) {
    std::lock_guard<std::mutex> lock(fileWriteMutexesMutex);
    if (fileWriteMutexes.find(key) == fileWriteMutexes.end()) {
      fileWriteMutexes[key] = std::make_unique<std::mutex>();
    }
    return *fileWriteMutexes[key];
  }

  /**
   * @brief Ensures the target directory for chunks exists.
   * @param dirPath Path to the directory.
   * @return true if the directory exists or was created successfully, false
   * otherwise.
   */
  bool ensureDirectoryExists(const fs::path &dirPath);

  /**
   * @brief Gets the full path for storing a specific chunk file.
   * @param filename The original filename.
   * @param chunkNumber The chunk number.
   * @return The filesystem path for the chunk file.
   */
  fs::path getChunkFilePath(const std::string &clientAddress,
                            const std::string &filename, int chunkNumber);

  fs::path getChunkFolder(const std::string &clientAddress,
                          const std::string &filename);

  fs::path getReconstructedFilePath(const std::string &clientAddress,
                                    const std::string &filename);

  /**
   * @brief Attempts to reconstruct a file from its successfully received
   * chunks. This method is intended to be run asynchronously.
   * @param filename The name of the file to reconstruct.
   */
  void tryReconstructFile(const std::string &clientAddress,
                          const std::string &filename);

  bool hasSufficientDiskSpace(const fs::path &path,
                              uintmax_t requiredSpaceBytes);
  uintmax_t calculateDirectorySize(const fs::path &path) const;
  bool hasClientStorageBudget(const std::string &clientAddress,
                              uintmax_t additionalBytes) const;
  bool prepareOutputFileForChunk(const ChunkMetadata &chunk,
                                 size_t chunkDataSize,
                                 const fs::path &reconstructedFolder,
                                 const fs::path &normalizedReconstructedPath,
                                 uintmax_t targetFileSize);

  /// Called once at construction to pre-populate remainingChunks from the
  /// database, enabling resilience to server restarts mid-transfer.
  void loadPendingCounters();
};
