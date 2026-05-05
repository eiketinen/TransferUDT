#include "SecurityHandshake.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <vector>

namespace {

std::string toHex(const unsigned char *data, size_t size) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(size * 2);
  for (size_t i = 0; i < size; ++i) {
    out.push_back(kHex[(data[i] >> 4) & 0x0F]);
    out.push_back(kHex[data[i] & 0x0F]);
  }
  return out;
}

bool isLowerHex(const std::string &value) {
  return std::all_of(value.begin(), value.end(), [](unsigned char c) {
    return std::isdigit(c) || (c >= 'a' && c <= 'f');
  });
}

bool constantTimeEquals(const std::string &left, const std::string &right) {
  if (left.size() != right.size()) {
    return false;
  }

  unsigned char diff = 0;
  for (size_t i = 0; i < left.size(); ++i) {
    diff |= static_cast<unsigned char>(left[i] ^ right[i]);
  }
  return diff == 0;
}

bool isValidClientId(const std::string &clientId) {
  return !clientId.empty() && clientId.size() <= 128 &&
         clientId.find_first_of(" \t\r\n") == std::string::npos;
}

std::string hmacSha256Hex(const std::string &message,
                          const std::string &preSharedKey) {
  unsigned int digestLen = 0;
  unsigned char digest[EVP_MAX_MD_SIZE] = {};

  const unsigned char *result = HMAC(
      EVP_sha256(), preSharedKey.data(), static_cast<int>(preSharedKey.size()),
      reinterpret_cast<const unsigned char *>(message.data()), message.size(),
      digest, &digestLen);
  if (result == nullptr || digestLen == 0) {
    throw std::runtime_error("Failed to calculate challenge response.");
  }

  return toHex(digest, digestLen);
}

} // namespace

namespace SecurityHandshake {

std::string CreateChallenge() {
  std::vector<unsigned char> challenge(kChallengeBytes);
  if (RAND_bytes(challenge.data(), static_cast<int>(challenge.size())) != 1) {
    throw std::runtime_error("Failed to generate authentication challenge.");
  }
  return toHex(challenge.data(), challenge.size());
}

std::string BuildChallengeMessage(const std::string &challengeHex) {
  if (challengeHex.size() != kChallengeBytes * 2 ||
      !isLowerHex(challengeHex)) {
    throw std::invalid_argument("Invalid authentication challenge.");
  }
  return std::string(kChallengePrefix) + challengeHex;
}

bool TryParseChallengeMessage(const std::string &message,
                              std::string &challengeHex) {
  const std::string prefix(kChallengePrefix);
  if (message.rfind(prefix, 0) != 0) {
    return false;
  }

  challengeHex = message.substr(prefix.size());
  return challengeHex.size() == kChallengeBytes * 2 &&
         isLowerHex(challengeHex);
}

std::string BuildResponseMessage(const std::string &challengeHex,
                                 const std::string &preSharedKey) {
  return BuildResponseMessage(challengeHex, preSharedKey, "");
}

std::string BuildResponseMessage(const std::string &challengeHex,
                                 const std::string &preSharedKey,
                                 const std::string &clientId) {
  if (challengeHex.size() != kChallengeBytes * 2 ||
      !isLowerHex(challengeHex)) {
    throw std::invalid_argument("Invalid authentication challenge.");
  }
  if (clientId.empty()) {
    return std::string(kResponsePrefix) +
           hmacSha256Hex(challengeHex, preSharedKey);
  }
  if (!isValidClientId(clientId)) {
    throw std::invalid_argument("Invalid authentication client id.");
  }
  return std::string(kResponsePrefix) + clientId + " " +
         hmacSha256Hex(challengeHex + "|" + clientId, preSharedKey);
}

bool TryExtractClientId(const std::string &responseMessage,
                        std::string &clientId) {
  clientId.clear();
  const std::string prefix(kResponsePrefix);
  if (responseMessage.rfind(prefix, 0) != 0) {
    return false;
  }

  const std::string payload = responseMessage.substr(prefix.size());
  const size_t separator = payload.find(' ');
  if (separator == std::string::npos) {
    return payload.size() == 64 && isLowerHex(payload);
  }

  clientId = payload.substr(0, separator);
  const std::string digest = payload.substr(separator + 1);
  return isValidClientId(clientId) && digest.size() == 64 &&
         isLowerHex(digest);
}

bool VerifyResponseMessage(const std::string &challengeHex,
                            const std::string &preSharedKey,
                            const std::string &responseMessage,
                           std::string *clientId) {
  const std::string prefix(kResponsePrefix);
  if (responseMessage.rfind(prefix, 0) != 0) {
    return false;
  }

  const std::string payload = responseMessage.substr(prefix.size());
  std::string actual = payload;
  std::string claimedClientId;
  std::string expected = hmacSha256Hex(challengeHex, preSharedKey);
  const size_t separator = payload.find(' ');
  if (separator != std::string::npos) {
    claimedClientId = payload.substr(0, separator);
    actual = payload.substr(separator + 1);
    expected = hmacSha256Hex(challengeHex + "|" + claimedClientId,
                             preSharedKey);
  }
  if (clientId != nullptr) {
    *clientId = claimedClientId;
  }
  return constantTimeEquals(actual, expected);
}

} // namespace SecurityHandshake
