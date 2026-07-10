#pragma once
#include <filesystem> // Para std::filesystem::path in
#include <string>
#include <vector>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;

inline std::string wideToUtf8(const std::wstring &value) {
#ifdef _WIN32
  if (value.empty()) {
    return {};
  }

  const int utf8Size =
      WideCharToMultiByte(CP_UTF8, 0, value.data(),
                          static_cast<int>(value.size()), nullptr, 0, nullptr,
                          nullptr);
  if (utf8Size <= 0) {
    return {};
  }

  std::string utf8(static_cast<size_t>(utf8Size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                      utf8.data(), utf8Size, nullptr, nullptr);
  return utf8;
#else
  return std::string(value.begin(), value.end());
#endif
}

inline std::wstring utf8ToWide(const std::string &value) {
#ifdef _WIN32
  if (value.empty()) {
    return {};
  }

  const int wideSize =
      MultiByteToWideChar(CP_UTF8, 0, value.data(),
                          static_cast<int>(value.size()), nullptr, 0);
  if (wideSize <= 0) {
    return {};
  }

  std::wstring wide(static_cast<size_t>(wideSize), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, value.data(),
                      static_cast<int>(value.size()), wide.data(), wideSize);
  return wide;
#else
  return std::wstring(value.begin(), value.end());
#endif
}
/**
 * @class ChunkMetadata
 * @brief Represents the metadata of a file chunk, containing all information
 * necessary for identification and reconstruction.
 */
class ChunkMetadata {
public:
  /**
   * @brief Constructs a ChunkMetadata instance.
   * @param filename Name of the original file to which this chunk belongs.
   * @param chunkNumber Index of the current chunk (starting from 0 or 1,
   * depending on the system).
   * @param totalChunk Total number of chunks that form the original file.
   * @param hash Hash value (e.g., SHA-256) used to verify the integrity of the
   * chunk.
   * @param filePath File system path where this chunk is stored.
   */
  ChunkMetadata(const std::string &filename, int chunkNumber, int totalChunk,
                const std::string &hash, const fs::path &filePath)
      : filename_(filename), chunkNumber_(chunkNumber), totalChunk_(totalChunk),
        hash_(hash), filePath_(filePath), dataSize_(0), chunkOffset_(0),
        totalFileSize_(0), chunkSize_(0), retries_(0) {}

  ChunkMetadata(const std::wstring &filename, int chunkNumber, int totalChunk,
                int chunkSize, const std::string &hash,
                const std::wstring &filePath, int retries)
      : filename_(wideToUtf8(filename)),
        chunkNumber_(chunkNumber), totalChunk_(totalChunk), hash_(hash),
        filePath_(fs::path(filePath)), dataSize_(0), chunkOffset_(0),
        totalFileSize_(0), chunkSize_(chunkSize), retries_(retries) {}
  /**
   *   Constructs a ChunkMetadata object.
   *
   *   @param clientAddress The IP address of the client.
   *   @param filename The name of the file being transferred.
   *   @param chunkNumber The number of the current chunk.
   *   @param totalChunk The total number of chunks.
   *   @param hash The hash of the file.
   *   @param filePath The path to the file.
   *
   *   @return None
   */
  ChunkMetadata(const std::string &clientAddress, const std::string &filename,
                int chunkNumber, int totalChunk, const std::string &hash,
                const fs::path &filePath)
      : clientAddress_(clientAddress), filename_(filename),
        chunkNumber_(chunkNumber), totalChunk_(totalChunk), hash_(hash),
        filePath_(filePath), dataSize_(0), chunkOffset_(0),
        totalFileSize_(0), chunkSize_(0), retries_(0) {}
  /**
   * @brief Default constructor.
   */
  ChunkMetadata()
      : filename_(""), chunkNumber_(0), totalChunk_(0), hash_(""),
        filePath_(""), dataSize_(0), chunkOffset_(0), totalFileSize_(0),
        chunkSize_(0), retries_(0) {}

  void setFilename(const std::string &filename) { filename_ = filename; }

  void setDirectory(const std::string &directory) { directory_ = directory; }

  void setChunkNumber(int chunkNumber) { chunkNumber_ = chunkNumber; }

  void setTotalChunk(int totalChunk) { totalChunk_ = totalChunk; }

  void setHash(const std::string &hash) { hash_ = hash; }

  void setFilePath(const std::string &filePath) { filePath_ = filePath; }

  void setChunkOffset(uint64_t offset) { chunkOffset_ = offset; }

  void setTotalFileSize(uint64_t size) { totalFileSize_ = size; }

  void setClientAddress(const std::string &clientAddress) {
    clientAddress_ = clientAddress;
  }

  void setServerAddress(const std::string &serverAddress) {
    serverAddress_ = serverAddress;
  }

  void setTransferId(const std::string &transferId) {
    transferId_ = transferId;
  }

  void setData(const std::vector<char> &data) {
    data_ = data;
    dataSize_ = data.size();
  }

  std::vector<char> &getData() { return data_; }

  std::string getClientAddress() const { return clientAddress_; }

  std::string getServerAddress() const { return serverAddress_; }
  const std::string &getTransferId() const { return transferId_; }
  /**
   * @brief Gets the name of the original file.
   * @return Name of the file.
   */
  std::string getFilename() const { return filename_; }

  /**
   * Retrieves the filename as a wide string.
   *
   * @return The filename as a wide string.
   */
  std::wstring getFilenameW() const { return utf8ToWide(filename_); }

  /**
   * @brief Gets the current chunk number.
   * @return Chunk index.
   */
  int getChunkNumber() const { return chunkNumber_; }

  /**
   * @brief Gets the hash value associated with the chunk.
   * @return Hash string.
   */
  std::string getHash() const { return hash_; }

  /**
   * @brief Gets the file path where the chunk is stored.
   * @return File path string.
   */
  fs::path getFilePath() const { return filePath_; }

  /**
   * @brief Gets the total number of chunks in the original file.
   * @return Total chunk count.
   */
  int getTotalChunk() const { return totalChunk_; }

  /**
   * @brief Gets the size of the data in the chunk.
   * @return Size of the data.
   */
  size_t getDataSize() const { return dataSize_; }

  uint64_t getChunkOffset() const { return chunkOffset_; }

  uint64_t getTotalFileSize() const { return totalFileSize_; }

  std::string getDirectory() const { return directory_; }

  std::wstring getDirectoryW() const { return utf8ToWide(directory_); }

  int getChunkSize() const { return chunkSize_; }
  void setChunkSize(int size) { chunkSize_ = size; }

  int getRetries() const { return retries_; }
  void setRetries(int retries) { retries_ = retries; }

private:
  std::string clientAddress_; /**< Address of the client that sent the chunk */
  std::string serverAddress_; /**< Address of the server that sent the chunk */
  std::string transferId_;   /**< Stable SHA-256 identity of the transfer */
  std::string filename_;      /**< Name of the original file */
  int chunkNumber_;           /**< Index of the current chunk */
  int totalChunk_;            /**< Total number of chunks in the file */
  std::string hash_;          /**< Hash value for integrity verification */
  fs::path filePath_;         /**< File path of the chunk */
  std::vector<char> data_;    /**< Data of the chunk */
  size_t dataSize_;           /**< Size of the data in the chunk */
  std::string directory_;     /**< Directory of the file */
  uint64_t chunkOffset_;      /**< Byte offset in the original file */
  uint64_t totalFileSize_;    /**< Total file size in bytes */
  int chunkSize_;             /**< Configured size of the chunks */
  int retries_;               /**< Transfer retry count */
};
