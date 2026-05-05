#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace SecurePacket {

inline constexpr const char *kMagic = "TUDTSEC1";
inline constexpr unsigned int kMagicSize = 8;
inline constexpr unsigned int kVersion = 1;
inline constexpr unsigned int kNonceSize = 12;
inline constexpr unsigned int kTagSize = 16;
inline constexpr uint32_t kMaxControlMessageBytes = 64U * 1024U;

bool IsEncryptedPacket(const std::vector<char> &packet);

bool TryGetNonceHex(const std::vector<char> &encryptedPacket,
                    std::string &nonceHex);

std::vector<char> EncryptPacket(const std::vector<char> &plainPacket,
                                 const std::string &preSharedKey);

std::vector<char> DecryptPacket(const std::vector<char> &encryptedPacket,
                                 const std::string &preSharedKey);

std::vector<char> EncryptControlMessage(const std::string &message,
                                        const std::string &preSharedKey);

std::string DecryptControlMessage(const std::vector<char> &encryptedPacket,
                                  const std::string &preSharedKey);

} // namespace SecurePacket
