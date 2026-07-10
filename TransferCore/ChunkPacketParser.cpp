#include "ChunkPacketParser.h"

#include <cstddef>
#include <cstring>
#include <filesystem>

#include <winsock2.h>

namespace {
namespace fs = std::filesystem;

constexpr uint32_t kMaxFilenameLen = 255U;
constexpr uint32_t kMaxDirectoryLen = 4096U;
constexpr uint32_t kMaxServerAddressLen = 128U;
constexpr uint32_t kSha256HexLen = 64U;

bool IsLowerHex(const std::string &value) {
  if (value.empty()) {
    return false;
  }
  for (unsigned char ch : value) {
    const bool isDigit = (ch >= '0' && ch <= '9');
    const bool isLowerHexLetter = (ch >= 'a' && ch <= 'f');
    if (!isDigit && !isLowerHexLetter) {
      return false;
    }
  }
  return true;
}

void SetError(std::string *out, const std::string &message) {
  if (out != nullptr) {
    *out = message;
  }
}

bool HasControlChars(const std::string &value) {
  for (unsigned char ch : value) {
    if (ch < 32) {
      return true;
    }
  }
  return false;
}

bool EnsureAvailable(size_t offset, size_t totalSize, size_t bytesToRead,
                     std::string *errorMessage, const char *fieldName) {
  if (offset > totalSize || bytesToRead > (totalSize - offset)) {
    SetError(errorMessage,
             std::string("Buffer too small while reading ") + fieldName);
    return false;
  }
  return true;
}

bool ReadUint32(const std::vector<char> &buffer, size_t &offset,
                uint32_t &outValue, std::string *errorMessage,
                const char *fieldName) {
  if (!EnsureAvailable(offset, buffer.size(), sizeof(uint32_t), errorMessage,
                       fieldName)) {
    return false;
  }
  std::memcpy(&outValue, buffer.data() + offset, sizeof(uint32_t));
  outValue = ntohl(outValue);
  offset += sizeof(uint32_t);
  return true;
}

bool ReadUint64(const std::vector<char> &buffer, size_t &offset,
                uint64_t &outValue, std::string *errorMessage,
                const char *fieldName) {
  if (!EnsureAvailable(offset, buffer.size(), sizeof(uint64_t), errorMessage,
                       fieldName)) {
    return false;
  }
  uint32_t high_part, low_part;
  std::memcpy(&high_part, buffer.data() + offset, sizeof(uint32_t));
  std::memcpy(&low_part, buffer.data() + offset + sizeof(uint32_t),
              sizeof(uint32_t));
  outValue = (static_cast<uint64_t>(ntohl(high_part)) << 32) | ntohl(low_part);
  offset += sizeof(uint64_t);
  return true;
}

} // namespace

namespace ChunkPacketParser {

bool IsSafeFilename(const std::string &filename) {
  if (filename.empty() || filename.size() > kMaxFilenameLen) {
    return false;
  }
  if (HasControlChars(filename)) {
    return false;
  }
  if (filename.find_first_of("<>:\"/\\|?*") != std::string::npos) {
    return false;
  }

  const fs::path filePath(filename);
  if (filePath.has_root_name() || filePath.has_root_directory() ||
      filePath.has_parent_path()) {
    return false;
  }

  const std::string normalizedName = filePath.lexically_normal().string();
  return normalizedName != "." && normalizedName != "..";
}

bool IsSafeRelativeDirectory(const std::string &directory) {
  if (directory.empty()) {
    return true;
  }
  if (directory.size() > kMaxDirectoryLen || HasControlChars(directory)) {
    return false;
  }

  const fs::path dirPath(directory);
  if (dirPath.has_root_name() || dirPath.has_root_directory() ||
      dirPath.is_absolute()) {
    return false;
  }

  for (const auto &segment : dirPath) {
    const std::string token = segment.string();
    if (token.empty() || token == "." || token == "..") {
      return false;
    }
  }
  return true;
}

bool Parse(const std::vector<char> &buffer, ChunkMetadata &chunk,
           std::string *errorMessage) {
  size_t offset = 0;
  const size_t bufferSize = buffer.size();

  if (bufferSize < sizeof(uint32_t)) {
    SetError(errorMessage, "Buffer too small to contain totalLength");
    return false;
  }

  uint32_t totalLength = 0;
  if (!ReadUint32(buffer, offset, totalLength, errorMessage, "totalLength")) {
    return false;
  }

  if (totalLength > kMaxPacketSizeBytes) {
    SetError(errorMessage, "Packet total length exceeds maximum allowed size");
    return false;
  }

  if (bufferSize != offset + totalLength) {
    SetError(errorMessage, "Buffer size does not match packet totalLength");
    return false;
  }

  uint32_t filenameLen = 0;
  if (!ReadUint32(buffer, offset, filenameLen, errorMessage, "filenameLen")) {
    return false;
  }
  if (filenameLen == 0 || filenameLen > kMaxFilenameLen) {
    SetError(errorMessage, "Invalid filename length");
    return false;
  }

  if (!EnsureAvailable(offset, bufferSize, filenameLen, errorMessage,
                       "filename")) {
    return false;
  }

  std::string filename(buffer.data() + offset, filenameLen);
  if (!IsSafeFilename(filename)) {
    SetError(errorMessage, "Unsafe filename received");
    return false;
  }
  chunk.setFilename(filename);
  offset += filenameLen;

  uint32_t directoryLen = 0;
  if (!ReadUint32(buffer, offset, directoryLen, errorMessage, "directoryLen")) {
    return false;
  }
  if (directoryLen > kMaxDirectoryLen) {
    SetError(errorMessage, "Invalid directory length");
    return false;
  }

  if (!EnsureAvailable(offset, bufferSize, directoryLen, errorMessage,
                       "directory")) {
    return false;
  }

  std::string directoryName(buffer.data() + offset, directoryLen);
  if (!IsSafeRelativeDirectory(directoryName)) {
    SetError(errorMessage, "Unsafe directory path received");
    return false;
  }
  chunk.setDirectory(directoryName);
  offset += directoryLen;

  uint32_t serverAddressLen = 0;
  if (!ReadUint32(buffer, offset, serverAddressLen, errorMessage,
                  "serverAddressLen")) {
    return false;
  }
  if (serverAddressLen > kMaxServerAddressLen) {
    SetError(errorMessage, "Invalid server address length");
    return false;
  }

  if (!EnsureAvailable(offset, bufferSize, serverAddressLen, errorMessage,
                       "serverAddress")) {
    return false;
  }

  std::string serverAddress(buffer.data() + offset, serverAddressLen);
  chunk.setServerAddress(serverAddress);
  offset += serverAddressLen;

  uint32_t transferIdLen = 0;
  if (!ReadUint32(buffer, offset, transferIdLen, errorMessage,
                  "transferIdLen")) {
    return false;
  }
  if (transferIdLen != kTransferIdHexLength) {
    SetError(errorMessage, "Invalid transfer id length");
    return false;
  }
  if (!EnsureAvailable(offset, bufferSize, transferIdLen, errorMessage,
                       "transferId")) {
    return false;
  }
  std::string transferId(buffer.data() + offset, transferIdLen);
  if (!IsLowerHex(transferId)) {
    SetError(errorMessage, "Invalid transfer id format");
    return false;
  }
  chunk.setTransferId(transferId);
  offset += transferIdLen;

  uint32_t chunkNumber = 0;
  if (!ReadUint32(buffer, offset, chunkNumber, errorMessage, "chunkNumber")) {
    return false;
  }
  chunk.setChunkNumber(static_cast<int>(chunkNumber));

  uint32_t totalChunks = 0;
  if (!ReadUint32(buffer, offset, totalChunks, errorMessage, "totalChunks")) {
    return false;
  }
  if (totalChunks > kMaxTotalChunks) {
    SetError(errorMessage, "totalChunks exceeds maximum allowed count");
    return false;
  }
  if (totalChunks > 0 && chunkNumber >= totalChunks) {
    SetError(errorMessage, "chunkNumber out of range");
    return false;
  }
  chunk.setTotalChunk(static_cast<int>(totalChunks));

  uint64_t chunkOffset = 0;
  if (!ReadUint64(buffer, offset, chunkOffset, errorMessage, "chunkOffset")) {
    return false;
  }
  chunk.setChunkOffset(chunkOffset);

  uint64_t totalFileSize = 0;
  if (!ReadUint64(buffer, offset, totalFileSize, errorMessage,
                  "totalFileSize")) {
    return false;
  }
  if (totalFileSize == 0 || totalFileSize > kMaxFileSizeBytes) {
    SetError(errorMessage, "Invalid totalFileSize received");
    return false;
  }
  chunk.setTotalFileSize(totalFileSize);

  uint32_t hashLen = 0;
  if (!ReadUint32(buffer, offset, hashLen, errorMessage, "hashLen")) {
    return false;
  }
  if (hashLen != kSha256HexLen) {
    SetError(errorMessage, "Invalid hash length");
    return false;
  }

  if (!EnsureAvailable(offset, bufferSize, hashLen, errorMessage, "hash")) {
    return false;
  }

  std::string hash(buffer.data() + offset, hashLen);
  if (!IsLowerHex(hash)) {
    SetError(errorMessage, "Invalid hash format");
    return false;
  }
  chunk.setHash(hash);
  offset += hashLen;

  uint32_t dataLen = 0;
  if (!ReadUint32(buffer, offset, dataLen, errorMessage, "dataLen")) {
    return false;
  }
  if (dataLen == 0 || dataLen > kMaxChunkSizeBytes) {
    SetError(errorMessage, "Invalid chunk data size");
    return false;
  }
  if (chunkOffset >= totalFileSize ||
      static_cast<uint64_t>(dataLen) > totalFileSize - chunkOffset) {
    SetError(errorMessage, "Chunk byte range exceeds total file size");
    return false;
  }

  if (!EnsureAvailable(offset, bufferSize, dataLen, errorMessage,
                       "chunkData")) {
    return false;
  }

  std::vector<char> chunkData(
      buffer.begin() + static_cast<std::ptrdiff_t>(offset),
      buffer.begin() + static_cast<std::ptrdiff_t>(offset + dataLen));
  chunk.setData(chunkData);
  offset += dataLen;

  if (offset != bufferSize) {
    SetError(errorMessage, "Packet contains unexpected trailing bytes");
    return false;
  }

  return true;
}

} // namespace ChunkPacketParser
