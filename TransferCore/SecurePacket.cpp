#include "SecurePacket.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>

#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/rand.h>

namespace {

constexpr size_t kOuterLengthSize = sizeof(uint32_t);
constexpr size_t kLegacyHeaderFieldsSize =
    SecurePacket::kMagicSize + sizeof(uint32_t) * 4U;
constexpr size_t kSessionHeaderFieldsSize =
    SecurePacket::kMagicSize + sizeof(uint32_t) * 5U + sizeof(uint64_t);

struct ParsedEnvelope {
  uint32_t version = 0;
  std::string sessionId;
  uint64_t sequenceNumber = 0;
  const unsigned char *nonce = nullptr;
  size_t nonceSize = 0;
  const unsigned char *tag = nullptr;
  size_t tagSize = 0;
  size_t cipherOffset = 0;
  size_t cipherSize = 0;
  size_t authenticatedHeaderSize = 0;
};

void AppendUint32(std::vector<char> &out, uint32_t value) {
  out.push_back(static_cast<char>((value >> 24) & 0xFF));
  out.push_back(static_cast<char>((value >> 16) & 0xFF));
  out.push_back(static_cast<char>((value >> 8) & 0xFF));
  out.push_back(static_cast<char>(value & 0xFF));
}

void AppendUint64(std::vector<char> &out, uint64_t value) {
  AppendUint32(out, static_cast<uint32_t>(value >> 32));
  AppendUint32(out, static_cast<uint32_t>(value & 0xFFFFFFFFULL));
}

uint32_t ReadUint32(const std::vector<char> &buffer, size_t &offset) {
  if (offset > buffer.size() || buffer.size() - offset < sizeof(uint32_t)) {
    throw std::runtime_error("Secure packet is truncated.");
  }
  const auto b0 = static_cast<unsigned char>(buffer[offset]);
  const auto b1 = static_cast<unsigned char>(buffer[offset + 1]);
  const auto b2 = static_cast<unsigned char>(buffer[offset + 2]);
  const auto b3 = static_cast<unsigned char>(buffer[offset + 3]);
  offset += sizeof(uint32_t);
  return (static_cast<uint32_t>(b0) << 24) |
         (static_cast<uint32_t>(b1) << 16) |
         (static_cast<uint32_t>(b2) << 8) | static_cast<uint32_t>(b3);
}

uint64_t ReadUint64(const std::vector<char> &buffer, size_t &offset) {
  const uint64_t high = ReadUint32(buffer, offset);
  const uint64_t low = ReadUint32(buffer, offset);
  return (high << 32) | low;
}

void ValidatePacketLength(const std::vector<char> &packet) {
  if (packet.size() < kOuterLengthSize) {
    throw std::runtime_error("Secure packet is too small.");
  }
  size_t offset = 0;
  const uint32_t contentLength = ReadUint32(packet, offset);
  if (static_cast<uint64_t>(packet.size()) !=
      static_cast<uint64_t>(kOuterLengthSize) + contentLength) {
    throw std::runtime_error("Secure packet length mismatch.");
  }
}

bool IsLowerHex(const std::string &value) {
  for (unsigned char ch : value) {
    const bool isDigit = ch >= '0' && ch <= '9';
    const bool isHexLetter = ch >= 'a' && ch <= 'f';
    if (!isDigit && !isHexLetter) {
      return false;
    }
  }
  return true;
}

unsigned char HexValue(char value) {
  if (value >= '0' && value <= '9') {
    return static_cast<unsigned char>(value - '0');
  }
  return static_cast<unsigned char>(value - 'a' + 10);
}

std::vector<unsigned char> FromHex(const std::string &value) {
  if (value.size() % 2 != 0 || !IsLowerHex(value)) {
    throw std::invalid_argument("Invalid lowercase hexadecimal value.");
  }

  std::vector<unsigned char> bytes;
  bytes.reserve(value.size() / 2);
  for (size_t index = 0; index < value.size(); index += 2) {
    bytes.push_back(static_cast<unsigned char>((HexValue(value[index]) << 4) |
                                               HexValue(value[index + 1])));
  }
  return bytes;
}

std::string ToHex(const unsigned char *data, size_t size) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(size * 2);
  for (size_t i = 0; i < size; ++i) {
    out.push_back(kHex[(data[i] >> 4) & 0x0F]);
    out.push_back(kHex[data[i] & 0x0F]);
  }
  return out;
}

void CheckIntSize(size_t value, const char *fieldName) {
  if (value > static_cast<size_t>((std::numeric_limits<int>::max)())) {
    throw std::runtime_error(std::string(fieldName) + " is too large.");
  }
}

std::array<unsigned char, 32> DeriveKey(const std::string &preSharedKey) {
  if (preSharedKey.empty()) {
    throw std::runtime_error("Pre-shared key is empty.");
  }

  std::array<unsigned char, 32> key{};
  EVP_PKEY_CTX *rawCtx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr);
  if (rawCtx == nullptr) {
    throw std::runtime_error("Failed to allocate HKDF context.");
  }

  const unsigned char salt[] = "TransferUDT SecurePacket v1";
  const unsigned char info[] = "packet-encryption";
  const auto cleanup = [&]() { EVP_PKEY_CTX_free(rawCtx); };
  size_t keyLen = key.size();
  if (EVP_PKEY_derive_init(rawCtx) <= 0 ||
      EVP_PKEY_CTX_set_hkdf_md(rawCtx, EVP_sha256()) <= 0 ||
      EVP_PKEY_CTX_set1_hkdf_salt(rawCtx, salt,
                                  static_cast<int>(sizeof(salt) - 1)) <= 0 ||
      EVP_PKEY_CTX_set1_hkdf_key(
          rawCtx, reinterpret_cast<const unsigned char *>(preSharedKey.data()),
          static_cast<int>(preSharedKey.size())) <= 0 ||
      EVP_PKEY_CTX_add1_hkdf_info(rawCtx, info,
                                  static_cast<int>(sizeof(info) - 1)) <= 0 ||
      EVP_PKEY_derive(rawCtx, key.data(), &keyLen) <= 0 ||
      keyLen != key.size()) {
    cleanup();
    throw std::runtime_error("Failed to derive encryption key.");
  }
  cleanup();
  return key;
}

std::vector<char> Aes256GcmEncrypt(
    const std::vector<char> &plainText,
    const std::array<unsigned char, 32> &key,
    const std::array<unsigned char, SecurePacket::kNonceSize> &nonce,
    const std::vector<char> &authenticatedData,
    std::array<unsigned char, SecurePacket::kTagSize> &tag) {
  CheckIntSize(plainText.size(), "Plaintext");
  CheckIntSize(authenticatedData.size(), "Authenticated data");

  EVP_CIPHER_CTX *rawCtx = EVP_CIPHER_CTX_new();
  if (rawCtx == nullptr) {
    throw std::runtime_error("Failed to allocate encryption context.");
  }

  std::vector<char> cipherText(plainText.size());
  int aadLen = 0;
  int outLen = 0;
  int finalLen = 0;
  const auto cleanup = [&]() { EVP_CIPHER_CTX_free(rawCtx); };

  if (EVP_EncryptInit_ex(rawCtx, EVP_aes_256_gcm(), nullptr, nullptr,
                         nullptr) != 1 ||
      EVP_CIPHER_CTX_ctrl(rawCtx, EVP_CTRL_GCM_SET_IVLEN,
                          static_cast<int>(nonce.size()), nullptr) != 1 ||
      EVP_EncryptInit_ex(rawCtx, nullptr, nullptr, key.data(), nonce.data()) !=
          1 ||
      (!authenticatedData.empty() &&
       EVP_EncryptUpdate(
           rawCtx, nullptr, &aadLen,
           reinterpret_cast<const unsigned char *>(authenticatedData.data()),
           static_cast<int>(authenticatedData.size())) != 1) ||
      EVP_EncryptUpdate(rawCtx,
                        reinterpret_cast<unsigned char *>(cipherText.data()),
                        &outLen,
                        reinterpret_cast<const unsigned char *>(
                            plainText.data()),
                        static_cast<int>(plainText.size())) != 1 ||
      EVP_EncryptFinal_ex(
          rawCtx,
          reinterpret_cast<unsigned char *>(cipherText.data()) + outLen,
          &finalLen) != 1 ||
      EVP_CIPHER_CTX_ctrl(rawCtx, EVP_CTRL_GCM_GET_TAG,
                          static_cast<int>(tag.size()), tag.data()) != 1) {
    cleanup();
    throw std::runtime_error("AES-256-GCM encryption failed.");
  }

  cleanup();
  cipherText.resize(static_cast<size_t>(outLen + finalLen));
  return cipherText;
}

std::vector<char> Aes256GcmDecrypt(const std::vector<char> &cipherText,
                                   const std::array<unsigned char, 32> &key,
                                   const unsigned char *nonce,
                                   size_t nonceSize,
                                   const unsigned char *tag,
                                   size_t tagSize,
                                   const std::vector<char> &authenticatedData) {
  CheckIntSize(cipherText.size(), "Ciphertext");
  CheckIntSize(nonceSize, "Nonce");
  CheckIntSize(tagSize, "Tag");
  CheckIntSize(authenticatedData.size(), "Authenticated data");

  EVP_CIPHER_CTX *rawCtx = EVP_CIPHER_CTX_new();
  if (rawCtx == nullptr) {
    throw std::runtime_error("Failed to allocate decryption context.");
  }

  std::vector<char> plainText(cipherText.size());
  int aadLen = 0;
  int outLen = 0;
  int finalLen = 0;
  const auto cleanup = [&]() { EVP_CIPHER_CTX_free(rawCtx); };

  if (EVP_DecryptInit_ex(rawCtx, EVP_aes_256_gcm(), nullptr, nullptr,
                         nullptr) != 1 ||
      EVP_CIPHER_CTX_ctrl(rawCtx, EVP_CTRL_GCM_SET_IVLEN,
                          static_cast<int>(nonceSize), nullptr) != 1 ||
      EVP_DecryptInit_ex(rawCtx, nullptr, nullptr, key.data(), nonce) != 1 ||
      (!authenticatedData.empty() &&
       EVP_DecryptUpdate(
           rawCtx, nullptr, &aadLen,
           reinterpret_cast<const unsigned char *>(authenticatedData.data()),
           static_cast<int>(authenticatedData.size())) != 1) ||
      EVP_DecryptUpdate(rawCtx,
                        reinterpret_cast<unsigned char *>(plainText.data()),
                        &outLen,
                        reinterpret_cast<const unsigned char *>(
                            cipherText.data()),
                        static_cast<int>(cipherText.size())) != 1 ||
      EVP_CIPHER_CTX_ctrl(rawCtx, EVP_CTRL_GCM_SET_TAG,
                          static_cast<int>(tagSize),
                          const_cast<unsigned char *>(tag)) != 1) {
    cleanup();
    throw std::runtime_error("AES-256-GCM decryption setup failed.");
  }

  const int finalResult = EVP_DecryptFinal_ex(
      rawCtx, reinterpret_cast<unsigned char *>(plainText.data()) + outLen,
      &finalLen);
  cleanup();

  if (finalResult != 1) {
    throw std::runtime_error("Secure packet authentication failed.");
  }

  plainText.resize(static_cast<size_t>(outLen + finalLen));
  return plainText;
}

ParsedEnvelope ParseEnvelope(const std::vector<char> &encryptedPacket) {
  ValidatePacketLength(encryptedPacket);
  if (!SecurePacket::IsEncryptedPacket(encryptedPacket)) {
    throw std::runtime_error("Packet is not encrypted.");
  }

  size_t offset = kOuterLengthSize + SecurePacket::kMagicSize;
  ParsedEnvelope envelope;
  envelope.version = ReadUint32(encryptedPacket, offset);

  uint32_t nonceLen = 0;
  uint32_t tagLen = 0;
  uint32_t cipherLen = 0;
  if (envelope.version == SecurePacket::kLegacyVersion) {
    nonceLen = ReadUint32(encryptedPacket, offset);
    tagLen = ReadUint32(encryptedPacket, offset);
    cipherLen = ReadUint32(encryptedPacket, offset);
  } else if (envelope.version == SecurePacket::kVersion) {
    const uint32_t sessionIdLen = ReadUint32(encryptedPacket, offset);
    envelope.sequenceNumber = ReadUint64(encryptedPacket, offset);
    nonceLen = ReadUint32(encryptedPacket, offset);
    tagLen = ReadUint32(encryptedPacket, offset);
    cipherLen = ReadUint32(encryptedPacket, offset);

    if (sessionIdLen != SecurePacket::kSessionIdBytes ||
        envelope.sequenceNumber == 0) {
      throw std::runtime_error("Invalid secure session metadata.");
    }
    if (encryptedPacket.size() - offset < sessionIdLen) {
      throw std::runtime_error("Secure session id is truncated.");
    }
    envelope.sessionId = ToHex(
        reinterpret_cast<const unsigned char *>(encryptedPacket.data() + offset),
        sessionIdLen);
    offset += sessionIdLen;
    envelope.authenticatedHeaderSize = offset;
  } else {
    throw std::runtime_error("Unsupported secure packet version.");
  }

  if (nonceLen != SecurePacket::kNonceSize ||
      tagLen != SecurePacket::kTagSize) {
    throw std::runtime_error("Invalid secure packet nonce or tag length.");
  }

  const uint64_t expectedPayloadSize = static_cast<uint64_t>(nonceLen) +
                                       static_cast<uint64_t>(tagLen) +
                                       static_cast<uint64_t>(cipherLen);
  if (static_cast<uint64_t>(encryptedPacket.size() - offset) !=
      expectedPayloadSize) {
    throw std::runtime_error("Secure packet payload length mismatch.");
  }

  envelope.nonce = reinterpret_cast<const unsigned char *>(
      encryptedPacket.data() + offset);
  envelope.nonceSize = nonceLen;
  offset += nonceLen;
  envelope.tag = reinterpret_cast<const unsigned char *>(
      encryptedPacket.data() + offset);
  envelope.tagSize = tagLen;
  offset += tagLen;
  envelope.cipherOffset = offset;
  envelope.cipherSize = cipherLen;
  return envelope;
}

std::vector<char> DecryptEnvelope(const std::vector<char> &encryptedPacket,
                                  const std::string &preSharedKey,
                                  const ParsedEnvelope &envelope,
                                  const std::vector<char> &authenticatedData) {
  std::vector<char> cipherText(
      encryptedPacket.begin() +
          static_cast<std::ptrdiff_t>(envelope.cipherOffset),
      encryptedPacket.begin() + static_cast<std::ptrdiff_t>(
                                     envelope.cipherOffset +
                                     envelope.cipherSize));

  auto key = DeriveKey(preSharedKey);
  std::vector<char> plainPacket = Aes256GcmDecrypt(
      cipherText, key, envelope.nonce, envelope.nonceSize, envelope.tag,
      envelope.tagSize, authenticatedData);
  ValidatePacketLength(plainPacket);
  return plainPacket;
}

std::vector<char> EncryptLegacyPacket(const std::vector<char> &plainPacket,
                                      const std::string &preSharedKey) {
  if (plainPacket.size() > (std::numeric_limits<uint32_t>::max)()) {
    throw std::runtime_error("Secure packet plaintext exceeds maximum length.");
  }

  std::array<unsigned char, SecurePacket::kNonceSize> nonce{};
  if (RAND_bytes(nonce.data(), static_cast<int>(nonce.size())) != 1) {
    throw std::runtime_error("Failed to generate secure packet nonce.");
  }

  auto key = DeriveKey(preSharedKey);
  std::array<unsigned char, SecurePacket::kTagSize> tag{};
  const std::vector<char> emptyAuthenticatedData;
  std::vector<char> cipherText =
      Aes256GcmEncrypt(plainPacket, key, nonce, emptyAuthenticatedData, tag);

  const uint64_t contentLength = kLegacyHeaderFieldsSize + nonce.size() +
                                 tag.size() + cipherText.size();
  if (contentLength > (std::numeric_limits<uint32_t>::max)()) {
    throw std::runtime_error("Secure packet exceeds maximum length.");
  }

  std::vector<char> packet;
  packet.reserve(kOuterLengthSize + static_cast<size_t>(contentLength));
  AppendUint32(packet, static_cast<uint32_t>(contentLength));
  packet.insert(packet.end(), SecurePacket::kMagic,
                SecurePacket::kMagic + SecurePacket::kMagicSize);
  AppendUint32(packet, SecurePacket::kLegacyVersion);
  AppendUint32(packet, static_cast<uint32_t>(nonce.size()));
  AppendUint32(packet, static_cast<uint32_t>(tag.size()));
  AppendUint32(packet, static_cast<uint32_t>(cipherText.size()));
  packet.insert(packet.end(), reinterpret_cast<const char *>(nonce.data()),
                reinterpret_cast<const char *>(nonce.data() + nonce.size()));
  packet.insert(packet.end(), reinterpret_cast<const char *>(tag.data()),
                reinterpret_cast<const char *>(tag.data() + tag.size()));
  packet.insert(packet.end(), cipherText.begin(), cipherText.end());
  return packet;
}

std::vector<char> BuildControlPacket(const std::string &message) {
  if (message.size() > SecurePacket::kMaxControlMessageBytes) {
    throw std::runtime_error("Secure control message exceeds maximum length.");
  }

  std::vector<char> plainPacket;
  plainPacket.reserve(kOuterLengthSize + message.size());
  AppendUint32(plainPacket, static_cast<uint32_t>(message.size()));
  plainPacket.insert(plainPacket.end(), message.begin(), message.end());
  return plainPacket;
}

std::string ReadControlPacket(const std::vector<char> &plainPacket) {
  size_t offset = 0;
  const uint32_t messageLen = ReadUint32(plainPacket, offset);
  if (messageLen > SecurePacket::kMaxControlMessageBytes) {
    throw std::runtime_error("Secure control message exceeds maximum length.");
  }
  if (plainPacket.size() - offset != messageLen) {
    throw std::runtime_error("Secure control message length mismatch.");
  }
  return std::string(
      plainPacket.begin() + static_cast<std::ptrdiff_t>(offset),
      plainPacket.end());
}

} // namespace

namespace SecurePacket {

bool IsEncryptedPacket(const std::vector<char> &packet) {
  if (packet.size() < kOuterLengthSize + kMagicSize) {
    return false;
  }
  return std::memcmp(packet.data() + kOuterLengthSize, kMagic, kMagicSize) ==
         0;
}

bool IsValidSessionId(const std::string &sessionId) {
  return sessionId.size() == kSessionIdHexChars && IsLowerHex(sessionId);
}

bool TryGetNonceHex(const std::vector<char> &encryptedPacket,
                    std::string &nonceHex) {
  try {
    const ParsedEnvelope envelope = ParseEnvelope(encryptedPacket);
    nonceHex = ToHex(envelope.nonce, envelope.nonceSize);
    return true;
  } catch (const std::exception &) {
    return false;
  }
}

bool TryGetSessionMetadata(const std::vector<char> &encryptedPacket,
                           SessionMetadata &metadata) {
  try {
    const ParsedEnvelope envelope = ParseEnvelope(encryptedPacket);
    if (envelope.version != kVersion) {
      return false;
    }
    metadata.sessionId = envelope.sessionId;
    metadata.sequenceNumber = envelope.sequenceNumber;
    return true;
  } catch (const std::exception &) {
    return false;
  }
}

uint64_t MaxSessionEncryptedContentLength(uint64_t plainPacketLength) {
  return plainPacketLength + kSessionEncryptedContentOverhead;
}

std::vector<char> EncryptPacket(const std::vector<char> &plainPacket,
                                const std::string &preSharedKey) {
  ValidatePacketLength(plainPacket);
  return EncryptLegacyPacket(plainPacket, preSharedKey);
}

std::vector<char> DecryptPacket(const std::vector<char> &encryptedPacket,
                                const std::string &preSharedKey) {
  const ParsedEnvelope envelope = ParseEnvelope(encryptedPacket);
  if (envelope.version != kLegacyVersion) {
    throw std::runtime_error(
        "Session-bound secure packet requires an expected session id.");
  }
  const std::vector<char> emptyAuthenticatedData;
  return DecryptEnvelope(encryptedPacket, preSharedKey, envelope,
                         emptyAuthenticatedData);
}

std::vector<char> EncryptPacket(const std::vector<char> &plainPacket,
                                const std::string &preSharedKey,
                                const std::string &sessionId,
                                uint64_t sequenceNumber) {
  ValidatePacketLength(plainPacket);
  if (!IsValidSessionId(sessionId) || sequenceNumber == 0) {
    throw std::invalid_argument("Invalid secure session id or sequence number.");
  }
  if (plainPacket.size() > (std::numeric_limits<uint32_t>::max)()) {
    throw std::runtime_error("Secure packet plaintext exceeds maximum length.");
  }

  const std::vector<unsigned char> rawSessionId = FromHex(sessionId);
  std::array<unsigned char, kNonceSize> nonce{};
  if (RAND_bytes(nonce.data(), static_cast<int>(nonce.size())) != 1) {
    throw std::runtime_error("Failed to generate secure packet nonce.");
  }

  const uint64_t contentLength = kSessionHeaderFieldsSize + rawSessionId.size() +
                                 nonce.size() + kTagSize + plainPacket.size();
  if (contentLength > (std::numeric_limits<uint32_t>::max)()) {
    throw std::runtime_error("Secure packet exceeds maximum length.");
  }

  std::vector<char> packet;
  packet.reserve(kOuterLengthSize + static_cast<size_t>(contentLength));
  AppendUint32(packet, static_cast<uint32_t>(contentLength));
  packet.insert(packet.end(), kMagic, kMagic + kMagicSize);
  AppendUint32(packet, kVersion);
  AppendUint32(packet, static_cast<uint32_t>(rawSessionId.size()));
  AppendUint64(packet, sequenceNumber);
  AppendUint32(packet, kNonceSize);
  AppendUint32(packet, kTagSize);
  AppendUint32(packet, static_cast<uint32_t>(plainPacket.size()));
  packet.insert(packet.end(), reinterpret_cast<const char *>(rawSessionId.data()),
                reinterpret_cast<const char *>(rawSessionId.data() +
                                                rawSessionId.size()));

  const std::vector<char> authenticatedData(packet.begin(), packet.end());
  const auto key = DeriveKey(preSharedKey);
  std::array<unsigned char, kTagSize> tag{};
  std::vector<char> cipherText =
      Aes256GcmEncrypt(plainPacket, key, nonce, authenticatedData, tag);
  if (cipherText.size() != plainPacket.size()) {
    throw std::runtime_error("Unexpected AES-GCM ciphertext length.");
  }

  packet.insert(packet.end(), reinterpret_cast<const char *>(nonce.data()),
                reinterpret_cast<const char *>(nonce.data() + nonce.size()));
  packet.insert(packet.end(), reinterpret_cast<const char *>(tag.data()),
                reinterpret_cast<const char *>(tag.data() + tag.size()));
  packet.insert(packet.end(), cipherText.begin(), cipherText.end());
  return packet;
}

std::vector<char> DecryptPacket(const std::vector<char> &encryptedPacket,
                                const std::string &preSharedKey,
                                const std::string &expectedSessionId,
                                SessionMetadata *metadata) {
  if (!IsValidSessionId(expectedSessionId)) {
    throw std::invalid_argument("Invalid expected secure session id.");
  }

  const ParsedEnvelope envelope = ParseEnvelope(encryptedPacket);
  if (envelope.version != kVersion) {
    throw std::runtime_error("Session-bound secure packet version is required.");
  }
  if (envelope.sessionId != expectedSessionId) {
    throw std::runtime_error("Secure packet session id mismatch.");
  }

  const std::vector<char> authenticatedData(
      encryptedPacket.begin(), encryptedPacket.begin() +
                                   static_cast<std::ptrdiff_t>(
                                       envelope.authenticatedHeaderSize));
  std::vector<char> plainPacket = DecryptEnvelope(
      encryptedPacket, preSharedKey, envelope, authenticatedData);
  if (metadata != nullptr) {
    metadata->sessionId = envelope.sessionId;
    metadata->sequenceNumber = envelope.sequenceNumber;
  }
  return plainPacket;
}

std::vector<char> EncryptControlMessage(const std::string &message,
                                        const std::string &preSharedKey) {
  return EncryptPacket(BuildControlPacket(message), preSharedKey);
}

std::string DecryptControlMessage(const std::vector<char> &encryptedPacket,
                                  const std::string &preSharedKey) {
  return ReadControlPacket(DecryptPacket(encryptedPacket, preSharedKey));
}

std::vector<char> EncryptControlMessage(const std::string &message,
                                        const std::string &preSharedKey,
                                        const std::string &sessionId,
                                        uint64_t sequenceNumber) {
  return EncryptPacket(BuildControlPacket(message), preSharedKey, sessionId,
                       sequenceNumber);
}

std::string DecryptControlMessage(const std::vector<char> &encryptedPacket,
                                  const std::string &preSharedKey,
                                  const std::string &expectedSessionId,
                                  SessionMetadata *metadata) {
  return ReadControlPacket(DecryptPacket(encryptedPacket, preSharedKey,
                                         expectedSessionId, metadata));
}

} // namespace SecurePacket
