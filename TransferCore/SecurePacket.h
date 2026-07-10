#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace SecurePacket {

inline constexpr const char *kMagic = "TUDTSEC1";
inline constexpr unsigned int kMagicSize = 8;
inline constexpr unsigned int kLegacyVersion = 1;
inline constexpr unsigned int kVersion = 2;
inline constexpr unsigned int kNonceSize = 12;
inline constexpr unsigned int kTagSize = 16;
inline constexpr unsigned int kSessionIdBytes = 16;
inline constexpr unsigned int kSessionIdHexChars = kSessionIdBytes * 2;
inline constexpr uint32_t kMaxControlMessageBytes = 64U * 1024U;
inline constexpr uint32_t kLegacyEncryptedContentOverhead =
    kMagicSize + (sizeof(uint32_t) * 4U) + kNonceSize + kTagSize;
inline constexpr uint32_t kSessionEncryptedContentOverhead =
    kMagicSize + (sizeof(uint32_t) * 5U) + sizeof(uint64_t) +
    kSessionIdBytes + kNonceSize + kTagSize;

struct SessionMetadata {
  std::string sessionId;
  uint64_t sequenceNumber = 0;
};

bool IsEncryptedPacket(const std::vector<char> &packet);
bool IsValidSessionId(const std::string &sessionId);

bool TryGetNonceHex(const std::vector<char> &encryptedPacket,
                    std::string &nonceHex);
bool TryGetSessionMetadata(const std::vector<char> &encryptedPacket,
                           SessionMetadata &metadata);

uint64_t MaxSessionEncryptedContentLength(uint64_t plainPacketLength);

std::vector<char> EncryptPacket(const std::vector<char> &plainPacket,
                                 const std::string &preSharedKey);

std::vector<char> DecryptPacket(const std::vector<char> &encryptedPacket,
                                 const std::string &preSharedKey);

std::vector<char> EncryptPacket(const std::vector<char> &plainPacket,
                                const std::string &preSharedKey,
                                const std::string &sessionId,
                                uint64_t sequenceNumber);

std::vector<char> DecryptPacket(const std::vector<char> &encryptedPacket,
                                const std::string &preSharedKey,
                                const std::string &expectedSessionId,
                                SessionMetadata *metadata = nullptr);

std::vector<char> EncryptControlMessage(const std::string &message,
                                        const std::string &preSharedKey);

std::string DecryptControlMessage(const std::vector<char> &encryptedPacket,
                                  const std::string &preSharedKey);

std::vector<char> EncryptControlMessage(const std::string &message,
                                        const std::string &preSharedKey,
                                        const std::string &sessionId,
                                        uint64_t sequenceNumber);

std::string DecryptControlMessage(const std::vector<char> &encryptedPacket,
                                  const std::string &preSharedKey,
                                  const std::string &expectedSessionId,
                                  SessionMetadata *metadata = nullptr);

} // namespace SecurePacket
