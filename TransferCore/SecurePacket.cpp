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

constexpr uint32_t kOuterLengthSize = sizeof(uint32_t);
constexpr uint32_t kHeaderFieldsSize =
    SecurePacket::kMagicSize + sizeof(uint32_t) * 4U;

void AppendUint32(std::vector<char> &out, uint32_t value) {
  out.push_back(static_cast<char>((value >> 24) & 0xFF));
  out.push_back(static_cast<char>((value >> 16) & 0xFF));
  out.push_back(static_cast<char>((value >> 8) & 0xFF));
  out.push_back(static_cast<char>(value & 0xFF));
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

void ValidatePacketLength(const std::vector<char> &packet) {
  if (packet.size() < kOuterLengthSize) {
    throw std::runtime_error("Secure packet is too small.");
  }
  size_t offset = 0;
  const uint32_t contentLength = ReadUint32(packet, offset);
  if (packet.size() != kOuterLengthSize + contentLength) {
    throw std::runtime_error("Secure packet length mismatch.");
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

std::vector<char> Aes256GcmEncrypt(const std::vector<char> &plainText,
                                   const std::array<unsigned char, 32> &key,
                                   const std::array<unsigned char, 12> &nonce,
                                   std::array<unsigned char, 16> &tag) {
  CheckIntSize(plainText.size(), "Plaintext");

  EVP_CIPHER_CTX *rawCtx = EVP_CIPHER_CTX_new();
  if (rawCtx == nullptr) {
    throw std::runtime_error("Failed to allocate encryption context.");
  }

  std::vector<char> cipherText(plainText.size());
  int outLen = 0;
  int finalLen = 0;

  const auto cleanup = [&]() { EVP_CIPHER_CTX_free(rawCtx); };

  if (EVP_EncryptInit_ex(rawCtx, EVP_aes_256_gcm(), nullptr, nullptr,
                         nullptr) != 1 ||
      EVP_CIPHER_CTX_ctrl(rawCtx, EVP_CTRL_GCM_SET_IVLEN,
                          static_cast<int>(nonce.size()), nullptr) != 1 ||
      EVP_EncryptInit_ex(rawCtx, nullptr, nullptr, key.data(), nonce.data()) !=
          1 ||
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
                                   size_t tagSize) {
  CheckIntSize(cipherText.size(), "Ciphertext");
  CheckIntSize(nonceSize, "Nonce");
  CheckIntSize(tagSize, "Tag");

  EVP_CIPHER_CTX *rawCtx = EVP_CIPHER_CTX_new();
  if (rawCtx == nullptr) {
    throw std::runtime_error("Failed to allocate decryption context.");
  }

  std::vector<char> plainText(cipherText.size());
  int outLen = 0;
  int finalLen = 0;

  const auto cleanup = [&]() { EVP_CIPHER_CTX_free(rawCtx); };

  if (EVP_DecryptInit_ex(rawCtx, EVP_aes_256_gcm(), nullptr, nullptr,
                         nullptr) != 1 ||
      EVP_CIPHER_CTX_ctrl(rawCtx, EVP_CTRL_GCM_SET_IVLEN,
                          static_cast<int>(nonceSize), nullptr) != 1 ||
      EVP_DecryptInit_ex(rawCtx, nullptr, nullptr, key.data(), nonce) != 1 ||
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

} // namespace

namespace SecurePacket {

bool IsEncryptedPacket(const std::vector<char> &packet) {
  if (packet.size() < kOuterLengthSize + kMagicSize) {
    return false;
  }
  return std::memcmp(packet.data() + kOuterLengthSize, kMagic, kMagicSize) == 0;
}

bool TryGetNonceHex(const std::vector<char> &encryptedPacket,
                    std::string &nonceHex) {
  try {
    ValidatePacketLength(encryptedPacket);
    if (!IsEncryptedPacket(encryptedPacket)) {
      return false;
    }

    size_t offset = kOuterLengthSize + kMagicSize;
    const uint32_t version = ReadUint32(encryptedPacket, offset);
    const uint32_t nonceLen = ReadUint32(encryptedPacket, offset);
    const uint32_t tagLen = ReadUint32(encryptedPacket, offset);
    const uint32_t cipherLen = ReadUint32(encryptedPacket, offset);
    if (version != kVersion || nonceLen != kNonceSize || tagLen != kTagSize ||
        encryptedPacket.size() - offset !=
            static_cast<size_t>(nonceLen) + tagLen + cipherLen) {
      return false;
    }

    const auto *nonce = reinterpret_cast<const unsigned char *>(
        encryptedPacket.data() + offset);
    nonceHex = ToHex(nonce, nonceLen);
    return true;
  } catch (const std::exception &) {
    return false;
  }
}

std::vector<char> EncryptPacket(const std::vector<char> &plainPacket,
                                const std::string &preSharedKey) {
  ValidatePacketLength(plainPacket);

  std::array<unsigned char, kNonceSize> nonce{};
  if (RAND_bytes(nonce.data(), static_cast<int>(nonce.size())) != 1) {
    throw std::runtime_error("Failed to generate secure packet nonce.");
  }

  auto key = DeriveKey(preSharedKey);
  std::array<unsigned char, kTagSize> tag{};
  std::vector<char> cipherText =
      Aes256GcmEncrypt(plainPacket, key, nonce, tag);

  const size_t contentLength =
      kHeaderFieldsSize + nonce.size() + tag.size() + cipherText.size();
  if (contentLength > (std::numeric_limits<uint32_t>::max)()) {
    throw std::runtime_error("Secure packet exceeds maximum length.");
  }

  std::vector<char> packet;
  packet.reserve(kOuterLengthSize + contentLength);
  AppendUint32(packet, static_cast<uint32_t>(contentLength));
  packet.insert(packet.end(), kMagic, kMagic + kMagicSize);
  AppendUint32(packet, kVersion);
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

std::vector<char> DecryptPacket(const std::vector<char> &encryptedPacket,
                                const std::string &preSharedKey) {
  ValidatePacketLength(encryptedPacket);
  if (!IsEncryptedPacket(encryptedPacket)) {
    throw std::runtime_error("Packet is not encrypted.");
  }

  size_t offset = kOuterLengthSize + kMagicSize;
  const uint32_t version = ReadUint32(encryptedPacket, offset);
  if (version != kVersion) {
    throw std::runtime_error("Unsupported secure packet version.");
  }

  const uint32_t nonceLen = ReadUint32(encryptedPacket, offset);
  const uint32_t tagLen = ReadUint32(encryptedPacket, offset);
  const uint32_t cipherLen = ReadUint32(encryptedPacket, offset);

  if (nonceLen != kNonceSize || tagLen != kTagSize) {
    throw std::runtime_error("Invalid secure packet nonce or tag length.");
  }
  if (encryptedPacket.size() - offset !=
      static_cast<size_t>(nonceLen) + tagLen + cipherLen) {
    throw std::runtime_error("Secure packet payload length mismatch.");
  }

  const auto *nonce =
      reinterpret_cast<const unsigned char *>(encryptedPacket.data() + offset);
  offset += nonceLen;
  const auto *tag =
      reinterpret_cast<const unsigned char *>(encryptedPacket.data() + offset);
  offset += tagLen;

  std::vector<char> cipherText(
      encryptedPacket.begin() + static_cast<std::ptrdiff_t>(offset),
      encryptedPacket.begin() +
          static_cast<std::ptrdiff_t>(offset + cipherLen));

  auto key = DeriveKey(preSharedKey);
  std::vector<char> plainPacket =
      Aes256GcmDecrypt(cipherText, key, nonce, nonceLen, tag, tagLen);
  ValidatePacketLength(plainPacket);
  return plainPacket;
}

std::vector<char> EncryptControlMessage(const std::string &message,
                                        const std::string &preSharedKey) {
  if (message.size() > kMaxControlMessageBytes) {
    throw std::runtime_error("Secure control message exceeds maximum length.");
  }

  std::vector<char> plainPacket;
  plainPacket.reserve(kOuterLengthSize + message.size());
  AppendUint32(plainPacket, static_cast<uint32_t>(message.size()));
  plainPacket.insert(plainPacket.end(), message.begin(), message.end());
  return EncryptPacket(plainPacket, preSharedKey);
}

std::string DecryptControlMessage(const std::vector<char> &encryptedPacket,
                                  const std::string &preSharedKey) {
  std::vector<char> plainPacket = DecryptPacket(encryptedPacket, preSharedKey);
  size_t offset = 0;
  const uint32_t messageLen = ReadUint32(plainPacket, offset);
  if (messageLen > kMaxControlMessageBytes) {
    throw std::runtime_error("Secure control message exceeds maximum length.");
  }
  if (plainPacket.size() - offset != messageLen) {
    throw std::runtime_error("Secure control message length mismatch.");
  }
  return std::string(plainPacket.begin() + static_cast<std::ptrdiff_t>(offset),
                     plainPacket.end());
}

} // namespace SecurePacket
