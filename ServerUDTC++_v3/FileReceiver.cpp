#include "FileReceiver.h"
#include <cerrno>
#include <chrono>
#include <cstring>
#include <limits>
#include <thread>

#ifdef _WIN32
#include <windows.h>
// Converte uma string UTF-8 para wstring (UTF-16), que o Windows usa
// nativamente
std::wstring utf8_to_wstring(const std::string &str) {
  if (str.empty()) {
    return L"";
  }
  int size_needed =
      MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), NULL, 0);
  std::wstring wstrTo(size_needed, 0);
  MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &wstrTo[0],
                      size_needed);
  return wstrTo;
}
#endif

// Cria um fs::path de forma portável a partir de uma string que você sabe ser
// UTF-8
fs::path path_from_utf8(const std::string &s) {
#ifdef _WIN32
  return fs::path(utf8_to_wstring(s));
#else
  // Em Linux/macOS, fs::path já espera UTF-8, então nenhuma conversão é
  // necessária.
  return fs::path(s);
#endif
}

fs::path normalizePathForFs(const fs::path &inputPath) {
#ifdef _WIN32
  if (inputPath.empty()) {
    return inputPath;
  }

  std::error_code ec;
  fs::path absolutePath = inputPath;
  if (!absolutePath.is_absolute()) {
    absolutePath = fs::absolute(absolutePath, ec);
    if (ec) {
      absolutePath = inputPath;
    }
  }

  absolutePath.make_preferred();
  std::wstring winPath = absolutePath.wstring();
  if (winPath.rfind(L"\\\\?\\", 0) == 0) {
    return fs::path(winPath);
  }

  if (winPath.rfind(L"\\\\", 0) == 0) {
    return fs::path(L"\\\\?\\UNC\\" + winPath.substr(2));
  }

  return fs::path(L"\\\\?\\" + winPath);
#else
  return inputPath;
#endif
}

std::string buildReconstructionKey(const std::string &clientAddress,
                                   const std::string &filename) {
  return clientAddress + "|" + filename;
}

FileReceiver::FileReceiver(Database &db,
                           std::unique_ptr<IFileIntegrityVerifier> verifierArg,
                           ServerConfig &config, ThreadPool &pool)
    : database(db), verifier(std::move(verifierArg)), config(config),
      threadPool(pool), storagePath(config.getStoragePath()),
      reconstructedPath(config.getReconstructedPath()) {
  if (!verifier) {
    Logger::getInstance().critical("FileReceiver::FileReceiver",
                                   "File integrity verifier is null!");
    throw std::runtime_error("Verifier cannot be null");
  }
  Logger::getInstance().info(
      "FileReceiver::FileReceiver",
      "FileReceiver initialized. Storage: '{}', Reconstructed: '{}'",
      storagePath.string(), reconstructedPath.string());

  // Pre-populate remaining-chunk counters from the database so that
  // transfers interrupted by a server restart can resume correctly.
  loadPendingCounters();
}
/**
 * Ensures that a directory exists at the specified path, creating it if
 * necessary.
 *
 * @param dirPath The path to the directory to be ensured.
 *
 * @return True if the directory exists or was successfully created, false
 * otherwise.
 *
 * @throws fs::filesystem_error If a filesystem error occurs while checking or
 * creating the directory.
 */
bool FileReceiver::ensureDirectoryExists(const fs::path &dirPath) {
  try {
    const fs::path normalizedDirPath = normalizePathForFs(dirPath);
    if (!fs::exists(normalizedDirPath)) {
      if (fs::create_directories(normalizedDirPath)) {
        Logger::getInstance().debug("FileReceiver::ensureDirectoryExists",
                                    "Created directory: {}", dirPath.string());
        return true;
      }

      Logger::getInstance().error("FileReceiver::ensureDirectoryExists",
                                  "Failed to create directory: {}",
                                  dirPath.string());
      return false;
    }

    if (!fs::is_directory(normalizedDirPath)) {
      Logger::getInstance().error("FileReceiver::ensureDirectoryExists",
                                  "Path exists but is not a directory: {}",
                                  dirPath.string());
      return false;
    }

    return true;
  } catch (const fs::filesystem_error &e) {
    Logger::getInstance().error(
        "FileReceiver::ensureDirectoryExists",
        "Filesystem error checking/creating directory {}: {}", dirPath.string(),
        e.what());
    return false;
  }
}
fs::path FileReceiver::getChunkFolder(const std::string &clientAddress,
                                      const std::string &filename) {
  // Create a subdirectory for the file's chunks
  fs::path chunkFolder = path_from_utf8(storagePath.string()) / clientAddress /
                         path_from_utf8(filename);
  return chunkFolder;
}
/**
 * Returns the file path for a chunk of a file.
 *
 * @param filename The name of the file.
 * @param chunkNumber The number of the chunk.
 *
 * @return The file path for the chunk.
 */
fs::path FileReceiver::getChunkFilePath(const std::string &clientAddress,
                                        const std::string &filename,
                                        int chunkNumber) {
  fs::path chunkFolder =
      storagePath / clientAddress / path_from_utf8(filename); // Corrigido
  std::string chunkFilename = filename + ".part" + std::to_string(chunkNumber);
  return chunkFolder / path_from_utf8(chunkFilename);
}
/**
 * Returns the reconstructed file path for a given client address and filename.
 *
 * @param clientAddress The address of the client.
 * @param filename The name of the file.
 *
 * @return The reconstructed file path.
 */
fs::path
FileReceiver::getReconstructedFilePath(const std::string &clientAddress,
                                       const std::string &filename) {
  fs::path reconstructedFolder = reconstructedPath / clientAddress;
  return reconstructedFolder / path_from_utf8(filename);
}

/**
 * Receives and processes a single file chunk.
 * Verifies the chunk's hash, saves the chunk to disk, and updates the database.
 *
 * @param meta Metadata of the received chunk.
 * @param chunkData Raw byte data of the chunk.
 *
 * @return ReceiveResult describing whether the chunk was stored, the file
 * already existed, or processing failed.
 *
 * @throws DatabaseException If a database error occurs during chunk processing.
 * @throws std::exception If a generic error occurs during chunk processing.
 */
FileReceiver::ReceiveResult
FileReceiver::receiveChunk(const ChunkMetadata &chunk,
                           const std::vector<char> &chunkData) {
  const std::string &filename = chunk.getFilename();
  const std::string clDirKey = chunk.getClientAddress() + "/" + chunk.getDirectory();
  const std::string reconKey = buildReconstructionKey(clDirKey, filename);
  fs::path reconstructedFilePath = getReconstructedFilePath(
      clDirKey, chunk.getFilename());

  // Check whether reconstruction has already been completed for this file.
  // NOTE: We cannot rely on fs::exists(reconstructedFilePath) alone because chunks
  // are written directly to the reconstructed path via seekp(chunkOffset).
  // After the very first chunk is written, the file exists but is NOT
  // fully reconstructed.
  //
  // Completion is detected by:
  // 1) in-memory reconstruction state for this process;
  // 2) reconstructed_files persistence in the database;
  // 3) existing output file with zero chunk metadata in DB (external/prebuilt file).
  bool isReconstructedInDb = false;
  bool hasChunkRows = false;
  try {
    isReconstructedInDb = database.isFileReconstructed(clDirKey, filename);
    if (isReconstructedInDb) {
      Logger::getInstance().warningC(
          "FileReceiver::receiveChunk", filename,
          "File '{}' is already marked as reconstructed. Skipping chunk {}.",
          chunk.getFilename(), chunk.getChunkNumber());
      return ReceiveResult::AlreadyCompleted;
    }

    hasChunkRows = database.getTotalChunkCount(clDirKey, filename) > 0;
    const bool fileExistsOnDisk =
        fs::exists(normalizePathForFs(reconstructedFilePath));
    Logger::getInstance().debugC(
        "FileReceiver::receiveChunk", filename,
        "Completion pre-check for '{}': reconstructed_in_db={}, "
        "has_chunk_rows={}, file_exists_on_disk={}.",
        chunk.getFilename(), isReconstructedInDb, hasChunkRows, fileExistsOnDisk);
    if (!hasChunkRows && fileExistsOnDisk) {
      Logger::getInstance().warningC(
          "FileReceiver::receiveChunk", filename,
          "File '{}' already exists with no pending chunk metadata. "
          "Skipping chunk {}.",
          chunk.getFilename(), chunk.getChunkNumber());
      return ReceiveResult::AlreadyCompleted;
    }
  } catch (const std::exception &e) {
    Logger::getInstance().errorC(
        "FileReceiver::receiveChunk", filename,
        "Failed to check completion state for '{}': {}",
        chunk.getFilename(), e.what());
    return ReceiveResult::Failed;
  }

  {
    std::lock_guard<std::mutex> lock(reconstructionMutex);
    auto it = reconstructionStatus.find(reconKey);
    if (it != reconstructionStatus.end() && it->second) {
      Logger::getInstance().warningC("FileReceiver::receiveChunk", filename,
                                     "Chunk {} for file '{}' already exists. "
                                     "Skipping processing of this chunk.",
                                     chunk.getChunkNumber(),
                                     chunk.getFilename());
      return ReceiveResult::AlreadyCompleted;
    }
  }

  if (!verifier) {
    Logger::getInstance().criticalC(
        "FileReceiver::receiveChunk", filename,
        "Cannot receive chunk for '{}', verifier is not initialized.",
        chunk.getFilename());
    return ReceiveResult::Failed;
  }
  // 1. Verify Hash
  std::string calculatedHash = verifier->calculateChunkHash(chunkData);
  if (calculatedHash != chunk.getHash()) {
    Logger::getInstance().errorC(
        "FileReceiver::receiveChunk", filename,
        "Hash mismatch for chunk {} of file '{}'. Expected: {}, Calculated: "
        "{}. Discarding chunk.",
        chunk.getChunkNumber(), chunk.getFilename(), chunk.getHash(),
        calculatedHash);
    return ReceiveResult::Failed; // Indicate failure due to hash mismatch
  }

  // 2. Determine Path and Ensure Directory
  fs::path reconstructedFolder = reconstructedFilePath.parent_path();
  if (!ensureDirectoryExists(reconstructedFolder)) {
    Logger::getInstance().criticalC(
        "FileReceiver::receiveChunk", filename,
        "Failed to ensure directory exists for file: {}",
        reconstructedFilePath.string());
    return ReceiveResult::Failed;
  }

  const std::string clientDirectory =
      chunk.getClientAddress() + "/" + chunk.getDirectory();
  bool chunkKnownInDb = false;
  try {
    if (database.isChunkSuccessful(clientDirectory, chunk.getFilename(),
                                   chunk.getChunkNumber())) {
      Logger::getInstance().warningC(
          "FileReceiver::receiveChunk", filename,
          "Chunk {} for file '{}' was already stored successfully. Skipping "
          "duplicate without updating counters.",
          chunk.getChunkNumber(), chunk.getFilename());
      return ReceiveResult::Stored;
    }

    chunkKnownInDb = database.getChunkStatus(clientDirectory,
                                             chunk.getFilename(),
                                             chunk.getChunkNumber()) != 0;
    if (chunkKnownInDb) {
      Logger::getInstance().warningC(
          "FileReceiver::receiveChunk", filename,
          "Chunk {} for file '{}' already exists in DB but is not successful. "
          "Retrying disk write and status update.",
          chunk.getChunkNumber(), chunk.getFilename());
    }
  } catch (const DatabaseException &e) {
    Logger::getInstance().criticalC(
        "FileReceiver::receiveChunk", filename,
        "Database error processing chunk {} for file '{}': {}",
        chunk.getChunkNumber(), chunk.getFilename(), e.what());
    return ReceiveResult::Failed;
  } catch (const std::exception &e) {
    Logger::getInstance().criticalC(
        "FileReceiver::receiveChunk", filename,
        "Generic error processing chunk {} for file '{}': {}",
        chunk.getChunkNumber(), chunk.getFilename(), e.what());
    return ReceiveResult::Failed;
  }

  // 3. Write Chunk to File
  try {
    std::string fileKey = buildReconstructionKey(clDirKey, chunk.getFilename());
    std::mutex &fileMutex = getFileMutex(fileKey);

    std::lock_guard<std::mutex> lock(fileMutex);

    const uintmax_t targetFileSize =
        chunk.getTotalFileSize() > 0
            ? static_cast<uintmax_t>(chunk.getTotalFileSize())
            : static_cast<uintmax_t>(chunkData.size());
    const fs::path normalizedReconstructedPath =
        normalizePathForFs(reconstructedFilePath);
    if (!prepareOutputFileForChunk(chunk, chunkData.size(),
                                   reconstructedFolder,
                                   normalizedReconstructedPath,
                                   targetFileSize)) {
      throw std::runtime_error("File storage budget or disk space rejected chunk");
    }

    constexpr int kMaxWriteAttempts = 5;
    constexpr auto kRetryDelay = std::chrono::milliseconds(50);

    bool writeSucceeded = false;
    std::string lastError;

    for (int attempt = 1; attempt <= kMaxWriteAttempts; ++attempt) {
      std::ofstream ofs;

      ofs.open(normalizedReconstructedPath,
               std::ios::binary | std::ios::in | std::ios::out);
      if (!ofs.is_open()) {
        lastError = "Failed to open file for direct writing";
        if (attempt < kMaxWriteAttempts) {
          char errnoBuffer[128] = {};
          strerror_s(errnoBuffer, sizeof(errnoBuffer), errno);
          Logger::getInstance().warningC(
              "FileReceiver::receiveChunk", filename,
              "Open attempt {}/{} failed for '{}'. Retrying in {} ms. errno={} "
              "({})",
              attempt, kMaxWriteAttempts, reconstructedFilePath.string(),
              kRetryDelay.count(), errno, errnoBuffer);
          std::this_thread::sleep_for(kRetryDelay);
          continue;
        }

        Logger::getInstance().errorC(
            "FileReceiver::receiveChunk", filename,
            "Failed to open chunk file for writing after {} attempts: {}",
            kMaxWriteAttempts, reconstructedFilePath.string());
        throw std::runtime_error(lastError);
      }

      OfstreamRAII ofsGuard(ofs); // RAII wrapper
      ofs.seekp(chunk.getChunkOffset(), std::ios::beg);
      ofs.write(chunkData.data(), chunkData.size());
      if (!ofs.good()) {
        lastError = "Error writing data to file";
        if (attempt < kMaxWriteAttempts) {
          Logger::getInstance().warningC(
              "FileReceiver::receiveChunk", filename,
              "Write attempt {}/{} failed for '{}'. Retrying in {} ms.",
              attempt, kMaxWriteAttempts, reconstructedFilePath.string(),
              kRetryDelay.count());
          std::this_thread::sleep_for(kRetryDelay);
          continue;
        }

        Logger::getInstance().errorC(
            "FileReceiver::receiveChunk", filename,
            "Error writing data to file after {} attempts: {}",
            kMaxWriteAttempts, reconstructedFilePath.string());
        throw std::runtime_error(lastError);
      }

      ofs.close();
      if (ofs.fail() && !ofs.eof()) {
        lastError = "Error occurred during closing of file";
        if (attempt < kMaxWriteAttempts) {
          Logger::getInstance().warningC(
              "FileReceiver::receiveChunk", filename,
              "Close attempt {}/{} failed for '{}'. Retrying in {} ms.",
              attempt, kMaxWriteAttempts, reconstructedFilePath.string(),
              kRetryDelay.count());
          std::this_thread::sleep_for(kRetryDelay);
          continue;
        }

        Logger::getInstance().errorC(
            "FileReceiver::receiveChunk", filename,
            "Error occurred during closing of file after {} attempts: {}",
            kMaxWriteAttempts, reconstructedFilePath.string());
        throw std::runtime_error(lastError);
      }

      writeSucceeded = true;
      break;
    }

    if (!writeSucceeded) {
      throw std::runtime_error(lastError.empty() ? "Chunk write failed"
                                                 : lastError);
    }

  } catch (const std::exception &e) {
    Logger::getInstance().criticalC("FileReceiver::receiveChunk", filename,
                                    "Exception writing chunk to file {}: {}",
                                    reconstructedFilePath.string(), e.what());
    return ReceiveResult::Failed;
  }

  // 4. Update Database
  try {
    if (!chunkKnownInDb) {
      chunkKnownInDb = database.getChunkStatus(clientDirectory,
                                               chunk.getFilename(),
                                               chunk.getChunkNumber()) != 0;
    }
    if (!chunkKnownInDb) {
      database.insertChunk(clientDirectory, chunk.getFilename(),
                           chunk.getChunkNumber(), chunk.getTotalChunk(),
                           chunk.getHash(), reconstructedFilePath);
      Logger::getInstance().infoC(
          "FileReceiver::receiveChunk", filename,
          "Successfully saved chunk {} for file '{}' to DB.",
          chunk.getChunkNumber(), chunk.getFilename());
    }
    database.updateChunkStatus(clientDirectory, chunk.getFilename(),
                               chunk.getChunkNumber(), "success");
    Logger::getInstance().infoC(
        "FileReceiver::receiveChunk", filename,
        "Successfully saved chunk {} for file '{}' to DB and disk.",
        chunk.getChunkNumber(), chunk.getFilename());
  } catch (const DatabaseException &e) {
    Logger::getInstance().errorC(
        "FileReceiver::receiveChunk", filename,
        "Database error processing chunk {} for file '{}': {}",
        chunk.getChunkNumber(), chunk.getFilename(), e.what());
    // Attempt to clean up the DB
    return ReceiveResult::Failed;
  } catch (const std::exception &e) {
    Logger::getInstance().criticalC(
        "FileReceiver::receiveChunk", filename,
        "Generic error processing chunk {} for file '{}': {}",
        chunk.getChunkNumber(), chunk.getFilename(), e.what());
    return ReceiveResult::Failed;
  }

  // 5. Atomic Counter decrement
  std::string clDir = chunk.getClientAddress() + "/" + chunk.getDirectory();
  std::string fileKey = buildReconstructionKey(clDir, chunk.getFilename());
  int chunksLeft = -1;

  {
    std::lock_guard<std::mutex> lockCounters(remainingChunksMutex);
    auto it = remainingChunks.find(fileKey);
    if (it == remainingChunks.end()) {
      // First chunk seen for this file in RAM.
      // Initialize from total and subtract 1 for the current chunk.
      remainingChunks[fileKey] = chunk.getTotalChunk() - 1;
    } else {
      it->second--;
    }
    chunksLeft = remainingChunks[fileKey];
  }

  if (chunksLeft <= 0) {
    Logger::getInstance().infoC(
        "FileReceiver::receiveChunk", filename,
        "All {} chunks received for '{}'. Enqueuing finalization.",
        chunk.getTotalChunk(), chunk.getFilename());

    const std::string key = fileKey;
    {
      std::lock_guard<std::mutex> lock(reconstructionMutex);
      if (reconstructionStatus.find(key) == reconstructionStatus.end() ||
          !reconstructionStatus[key]) {
        reconstructionStatus[key] = true;
        try {
          threadPool.addTask(
              [this, cAddress = clDir, fname = chunk.getFilename()]() {
                this->tryReconstructFile(cAddress, fname);
              });
        } catch (...) {
          reconstructionStatus.erase(key);
        }
      }
    }

    {
      std::lock_guard<std::mutex> lockCounters(remainingChunksMutex);
      remainingChunks.erase(fileKey);
    }
    return ReceiveResult::JustCompleted;
  }

  return ReceiveResult::Stored; // Chunk processed successfully
}

/**
 * Attempts to reconstruct a file from its chunks stored in the database.
 *
 * This function retrieves the chunks for the specified file, validates their
 * sequence and count, and then reconstructs the file by merging the chunks. It
 * also updates the file status in the database and cleans up the chunk
 * directory after successful reconstruction.
 *
 * @param filename The name of the file to be reconstructed.
 *
 * @return None
 *
 * @throws DatabaseException If a database error occurs during reconstruction
 * preparation or status update.
 * @throws std::exception If any other error occurs during reconstruction
 * attempt.
 */
void FileReceiver::tryReconstructFile(const std::string &clientAddress,
                                      const std::string &filename) {
  Logger::getInstance().infoC("FileReconstructor::tryReconstructFile", filename,
                              "Attempting reconstruction for file: {}",
                              filename);
  bool success = false;
  std::vector<ChunkMetadata> chunks;

  try {
    chunks = database.getChunksForFile(clientAddress, filename);
    if (chunks.empty()) {
      Logger::getInstance().warningC("FileReconstructor::tryReconstructFile",
                                     filename,
                                     "No successful chunks found for file '{}' "
                                     "during reconstruction attempt.",
                                     filename);
      return; // Exit if no chunks found
    }

    // Basic validation: check if sequence is contiguous and matches total count
    int expectedTotal = chunks[0].getTotalChunk();
    if (static_cast<int>(chunks.size()) != expectedTotal) {
      Logger::getInstance().warningC(
          "FileReconstructor::tryReconstructFile", filename,
          "Mismatch in chunk count for '{}'. Expected {}, found {}. Aborting "
          "finalization.",
          filename, expectedTotal, chunks.size());
      return; // Don't finalize if count mismatch
    }
    for (size_t i = 0; i < chunks.size(); ++i) {
      if (chunks[i].getChunkNumber() != static_cast<int>(i)) {
        Logger::getInstance().warningC(
            "FileReconstructor::tryReconstructFile", filename,
            "Chunk sequence error for '{}'. Expected chunk {}, found {}. "
            "Aborting finalization.",
            filename, i, chunks[i].getChunkNumber());
        return; // Don't finalize if sequence isn't 0, 1, 2...
      }
    }
    success = true;

  } catch (const DatabaseException &e) {
    Logger::getInstance().errorC(
        "FileReconstructor::tryReconstructFile", filename,
        "Database error during finalization preparation for '{}': {}", filename,
        e.what());
    success = false;
  } catch (const std::exception &e) {
    Logger::getInstance().errorC(
        "FileReconstructor::tryReconstructFile", filename,
        "Error during finalization attempt for '{}': {}", filename, e.what());
    success = false;
  }

  // Update final status based on finalization result
  try {
    if (success) {
      Logger::getInstance().infoC("FileReconstructor", filename,
                                  "Successfully finalized file: {}", filename);

      fs::path reconstructedPath =
          getReconstructedFilePath(clientAddress, filename);
      database.beginTransaction();
      database.addReconstructedFile(clientAddress, filename, reconstructedPath);
      database.deleteChunksByFile(clientAddress, filename);
      database.commitTransaction();

    } else {
      Logger::getInstance().errorC("FileReconstructor::tryReconstructFile",
                                   filename, "Failed to finalize file: {}",
                                   filename);
    }
  } catch (const DatabaseException &e) {
    database.rollbackTransaction(); // Rollback on error
    Logger::getInstance().criticalC(
        "FileReconstructor::tryReconstructFile", filename,
        "Database error updating final file status for '{}': {}", filename,
        e.what());
  }

  try {
    std::lock_guard<std::mutex> lock(reconstructionMutex);
    std::string key = buildReconstructionKey(clientAddress, filename);
    auto it = reconstructionStatus.find(key);
    if (it != reconstructionStatus.end()) {
      reconstructionStatus.erase(it);
      Logger::getInstance().infoC(
          "FileReconstructor", filename,
          "Finalization status for file '{}' has been cleared.", filename);
    }
  } catch (const std::exception &e) {
    Logger::getInstance().errorC(
        "FileReconstructor", filename,
        "Failed to clear finalization status for '{}': {}", filename, e.what());
  }
}

uintmax_t FileReceiver::calculateDirectorySize(const fs::path &path) const {
  std::error_code ec;
  const fs::path normalizedPath = normalizePathForFs(path);
  const bool exists = fs::exists(normalizedPath, ec);
  if (ec) {
    return (std::numeric_limits<uintmax_t>::max)();
  }
  if (!exists) {
    return 0;
  }
  const bool isDirectory = fs::is_directory(normalizedPath, ec);
  if (ec || !isDirectory) {
    return (std::numeric_limits<uintmax_t>::max)();
  }

  uintmax_t total = 0;
  constexpr auto options = fs::directory_options::skip_permission_denied;
  fs::recursive_directory_iterator it(normalizedPath, options, ec);
  const fs::recursive_directory_iterator end;
  while (it != end) {
    if (ec) {
      return (std::numeric_limits<uintmax_t>::max)();
    }

    const fs::file_status status = it->symlink_status(ec);
    if (ec) {
      return (std::numeric_limits<uintmax_t>::max)();
    }
    if (fs::is_symlink(status)) {
      return (std::numeric_limits<uintmax_t>::max)();
    }
    if (fs::is_regular_file(status)) {
      const uintmax_t fileSize = it->file_size(ec);
      if (ec ||
          total > (std::numeric_limits<uintmax_t>::max)() - fileSize) {
        return (std::numeric_limits<uintmax_t>::max)();
      }
      total += fileSize;
    }

    it.increment(ec);
  }

  return total;
}

bool FileReceiver::hasClientStorageBudget(const std::string &clientAddress,
                                          uintmax_t additionalBytes) const {
  const uintmax_t maxClientStorage =
      static_cast<uintmax_t>(config.getMaxClientStorageBytes());
  if (additionalBytes > maxClientStorage) {
    return false;
  }

  const fs::path clientRoot = reconstructedPath / clientAddress;
  const uintmax_t usedBytes = calculateDirectorySize(clientRoot);
  if (usedBytes == (std::numeric_limits<uintmax_t>::max)()) {
    Logger::getInstance().error(
        "FileReceiver::hasClientStorageBudget",
        "Failed to calculate storage usage for client '{}'", clientAddress);
    return false;
  }

  return usedBytes <= maxClientStorage - additionalBytes;
}

bool FileReceiver::prepareOutputFileForChunk(
    const ChunkMetadata &chunk, size_t chunkDataSize,
    const fs::path &reconstructedFolder,
    const fs::path &normalizedReconstructedPath, uintmax_t targetFileSize) {
  const std::string filename = chunk.getFilename();
  if (targetFileSize > static_cast<uintmax_t>(config.getMaxFileSizeBytes())) {
    Logger::getInstance().warningC(
        "FileReceiver::prepareOutputFileForChunk", filename,
        "Rejecting file '{}' because declared size {} exceeds configured "
        "server.max_file_size_mb budget.",
        filename, targetFileSize);
    return false;
  }

  if (chunk.getTotalFileSize() > 0) {
    const uint64_t declaredSize = chunk.getTotalFileSize();
    const uint64_t chunkOffset = chunk.getChunkOffset();
    if (chunkOffset > declaredSize ||
        static_cast<uint64_t>(chunkDataSize) > declaredSize - chunkOffset) {
      Logger::getInstance().warningC(
          "FileReceiver::prepareOutputFileForChunk", filename,
          "Rejecting chunk {} for '{}' because offset/size exceeds declared "
          "file size.",
          chunk.getChunkNumber(), filename);
      return false;
    }
  }

  std::lock_guard<std::mutex> budgetLock(storageBudgetMutex);
  std::error_code existsEc;
  const bool isNewFile = !fs::exists(normalizedReconstructedPath, existsEc);
  if (existsEc) {
    Logger::getInstance().errorC(
        "FileReceiver::prepareOutputFileForChunk", filename,
        "Failed to check output file existence for '{}': {}", filename,
        existsEc.message());
    return false;
  }

  if (isNewFile && !hasClientStorageBudget(chunk.getClientAddress(),
                                           targetFileSize)) {
    Logger::getInstance().warningC(
        "FileReceiver::prepareOutputFileForChunk", filename,
        "Rejecting file '{}' because client '{}' would exceed configured "
        "server.max_client_storage_mb budget.",
        filename, chunk.getClientAddress());
    return false;
  }

  const uintmax_t requiredSpaceBytes =
      isNewFile ? targetFileSize : static_cast<uintmax_t>(chunkDataSize);
  if (!hasSufficientDiskSpace(reconstructedFolder, requiredSpaceBytes)) {
    Logger::getInstance().criticalC(
        "FileReceiver::prepareOutputFileForChunk", filename,
        "Insufficient disk space for file '{}': requires {} bytes.",
        normalizedReconstructedPath.string(), requiredSpaceBytes);
    return false;
  }

  if (!isNewFile) {
    return true;
  }

  std::ofstream ofs(normalizedReconstructedPath,
                    std::ios::binary | std::ios::out);
  if (!ofs.is_open()) {
    Logger::getInstance().errorC(
        "FileReceiver::prepareOutputFileForChunk", filename,
        "Failed to create output file '{}'.",
        normalizedReconstructedPath.string());
    return false;
  }
  ofs.close();

  std::error_code resizeEc;
  fs::resize_file(normalizedReconstructedPath, targetFileSize, resizeEc);
  if (resizeEc) {
    Logger::getInstance().errorC(
        "FileReceiver::prepareOutputFileForChunk", filename,
        "Failed to pre-allocate file '{}': {}",
        normalizedReconstructedPath.string(), resizeEc.message());
    std::error_code removeEc;
    fs::remove(normalizedReconstructedPath, removeEc);
    return false;
  }

  return true;
}
/**
 * Checks if the specified path has sufficient disk space available.
 *
 * @param path the path to check for available disk space
 * @param requiredSpaceBytes the minimum amount of disk space required
 *
 * @return true if the path has sufficient disk space, false otherwise
 *
 * @throws fs::filesystem_error if an error occurs while accessing disk space
 * information
 */
bool FileReceiver::hasSufficientDiskSpace(const fs::path &path,
                                          uintmax_t requiredSpaceBytes) {
  try {
    // Recupera as informações de espaço do diretório especificado.
    fs::space_info spaceInfo = fs::space(normalizePathForFs(path));
    // spaceInfo.available indica o número de bytes disponíveis para utilização.
    return spaceInfo.available >= requiredSpaceBytes;
  } catch (const fs::filesystem_error &ex) {
    Logger::getInstance().error("FileReceiver::hasSufficientDiskSpace",
                                "Error accessing disk space for path '{}': {}",
                                path.string(), ex.what());
    // Em caso de erro, é aconselhável retornar false.
    return false;
  }
}

/**
 * @brief Pre-populates the remainingChunks map from the database.
 *
 * Called once at construction time. Queries all in-progress file transfers
 * (files that have chunks in the DB but are not yet in reconstructed_files)
 * and initializes the remaining-chunk counter for each one.
 *
 * This makes the server resilient to restarts mid-transfer: when the server
 * comes back up, it knows exactly how many chunks are still needed for each
 * file that was being transferred.
 */
void FileReceiver::loadPendingCounters() {
  try {
    auto counters = database.getInProgressFileCounters();
    if (counters.empty()) {
      Logger::getInstance().info("FileReceiver::loadPendingCounters",
                                 "No in-progress transfers to recover.");
      return;
    }

    std::lock_guard<std::mutex> lock(remainingChunksMutex);
    for (const auto &info : counters) {
      std::string fileKey =
          buildReconstructionKey(info.clientAddress, info.filename);
      int remaining = info.totalChunks - info.successfulCount;
      if (remaining > 0) {
        remainingChunks[fileKey] = remaining;
        Logger::getInstance().info(
            "FileReceiver::loadPendingCounters",
            "Recovered counter for '{}': {}/{} chunks remaining.", fileKey,
            remaining, info.totalChunks);
      } else {
        // All chunks are in the DB as 'success' but the file was never
        // finalized (e.g. server crashed right before reconstruction).
        // Trigger reconstruction now.
        Logger::getInstance().info(
            "FileReceiver::loadPendingCounters",
            "File '{}' has all {}/{} chunks. Enqueuing finalization.", fileKey,
            info.successfulCount, info.totalChunks);
        try {
          std::lock_guard<std::mutex> rlock(reconstructionMutex);
          if (reconstructionStatus.find(fileKey) ==
                  reconstructionStatus.end() ||
              !reconstructionStatus[fileKey]) {
            reconstructionStatus[fileKey] = true;
            threadPool.addTask(
                [this, cAddress = info.clientAddress, fname = info.filename]() {
                  this->tryReconstructFile(cAddress, fname);
                });
          }
        } catch (const std::exception &ex) {
          Logger::getInstance().error(
              "FileReceiver::loadPendingCounters",
              "Failed to enqueue reconstruction for '{}': {}", fileKey,
              ex.what());
        }
      }
    }

    Logger::getInstance().info(
        "FileReceiver::loadPendingCounters",
        "Recovered {} in-progress transfer counter(s) from database.",
        counters.size());
  } catch (const std::exception &e) {
    Logger::getInstance().error(
        "FileReceiver::loadPendingCounters",
        "Failed to load pending counters from database: {}. "
        "In-progress transfers from before the restart will re-initialize "
        "their counters when the next chunk arrives.",
        e.what());
  }
}



