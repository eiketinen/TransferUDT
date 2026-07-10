#include "SecurityHandshake.h"

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/kdf.h>
#include <openssl/pem.h>
#include <openssl/rand.h>

#include <algorithm>
#include <cctype>
#include <memory>
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
  if (value.empty()) {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [](unsigned char c) {
    return std::isdigit(c) || (c >= 'a' && c <= 'f');
  });
}

std::vector<unsigned char> fromHex(const std::string &value,
                                   size_t expectedBytes,
                                   const char *fieldName) {
  if (value.size() != expectedBytes * 2 || !isLowerHex(value)) {
    throw std::invalid_argument(std::string("Invalid ") + fieldName + ".");
  }

  std::vector<unsigned char> out;
  out.reserve(expectedBytes);
  for (size_t i = 0; i < value.size(); i += 2) {
    const auto high = static_cast<unsigned char>(value[i]);
    const auto low = static_cast<unsigned char>(value[i + 1]);
    const auto decode = [](unsigned char c) -> unsigned char {
      if (c >= '0' && c <= '9') {
        return static_cast<unsigned char>(c - '0');
      }
      return static_cast<unsigned char>(10 + c - 'a');
    };
    out.push_back(static_cast<unsigned char>((decode(high) << 4) |
                                             decode(low)));
  }
  return out;
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

bool isValidSessionId(const std::string &sessionId) {
  return sessionId.size() == SecurityHandshake::kSessionIdBytes * 2 &&
         isLowerHex(sessionId);
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

using EvpPkeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using EvpMdCtxPtr = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
using EvpPkeyCtxPtr = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;
using BioPtr = std::unique_ptr<BIO, decltype(&BIO_free)>;

EvpPkeyPtr loadPrivateKey(const std::string &path) {
  BioPtr bio(BIO_new_file(path.c_str(), "rb"), BIO_free);
  if (!bio) {
    throw std::runtime_error("Failed to open private key: " + path);
  }
  EVP_PKEY *raw = PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr);
  if (raw == nullptr) {
    throw std::runtime_error("Failed to read private key: " + path);
  }
  return EvpPkeyPtr(raw, EVP_PKEY_free);
}

EvpPkeyPtr loadPublicKey(const std::string &path) {
  BioPtr bio(BIO_new_file(path.c_str(), "rb"), BIO_free);
  if (!bio) {
    throw std::runtime_error("Failed to open public key: " + path);
  }
  EVP_PKEY *raw = PEM_read_bio_PUBKEY(bio.get(), nullptr, nullptr, nullptr);
  if (raw == nullptr) {
    throw std::runtime_error("Failed to read public key: " + path);
  }
  return EvpPkeyPtr(raw, EVP_PKEY_free);
}

std::string signMessageHex(const std::string &message,
                           const std::string &privateKeyPath) {
  auto key = loadPrivateKey(privateKeyPath);
  EvpMdCtxPtr ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
  if (!ctx) {
    throw std::runtime_error("Failed to allocate signing context.");
  }
  if (EVP_DigestSignInit(ctx.get(), nullptr, EVP_sha256(), nullptr,
                         key.get()) != 1 ||
      EVP_DigestSignUpdate(ctx.get(), message.data(), message.size()) != 1) {
    throw std::runtime_error("Failed to initialize signature.");
  }
  size_t signatureLen = 0;
  if (EVP_DigestSignFinal(ctx.get(), nullptr, &signatureLen) != 1 ||
      signatureLen == 0) {
    throw std::runtime_error("Failed to size signature.");
  }
  std::vector<unsigned char> signature(signatureLen);
  if (EVP_DigestSignFinal(ctx.get(), signature.data(), &signatureLen) != 1) {
    throw std::runtime_error("Failed to sign handshake transcript.");
  }
  signature.resize(signatureLen);
  return toHex(signature.data(), signature.size());
}

bool verifyMessageSignatureHex(const std::string &message,
                               const std::string &signatureHex,
                               const std::string &publicKeyPath) {
  if (signatureHex.empty() || signatureHex.size() % 2 != 0 ||
      !isLowerHex(signatureHex)) {
    return false;
  }
  std::vector<unsigned char> signature;
  try {
    signature = fromHex(signatureHex, signatureHex.size() / 2, "signature");
  } catch (const std::exception &) {
    return false;
  }

  auto key = loadPublicKey(publicKeyPath);
  EvpMdCtxPtr ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
  if (!ctx) {
    throw std::runtime_error("Failed to allocate verification context.");
  }
  if (EVP_DigestVerifyInit(ctx.get(), nullptr, EVP_sha256(), nullptr,
                           key.get()) != 1 ||
      EVP_DigestVerifyUpdate(ctx.get(), message.data(), message.size()) != 1) {
    throw std::runtime_error("Failed to initialize signature verification.");
  }
  return EVP_DigestVerifyFinal(ctx.get(), signature.data(),
                               signature.size()) == 1;
}

std::string buildClientTranscript(const SecurityHandshake::SignedChallenge &challenge,
                                  const std::string &clientId,
                                  const std::string &clientNonceHex,
                                  const std::string &clientPublicKeyHex) {
  return "TransferUDT signed handshake v1|client|" + clientId + "|" +
         challenge.serverNonceHex + "|" +
         challenge.serverEphemeralPublicKeyHex + "|" + clientNonceHex + "|" +
         clientPublicKeyHex;
}

std::string buildServerTranscript(const SecurityHandshake::SignedChallenge &challenge,
                                  const SecurityHandshake::SignedResponse &response) {
  return "TransferUDT signed handshake v1|server|" + response.clientId + "|" +
         challenge.serverNonceHex + "|" +
         challenge.serverEphemeralPublicKeyHex + "|" + response.clientNonceHex +
         "|" + response.clientEphemeralPublicKeyHex + "|" +
         response.signatureHex;
}

std::string buildKdfTranscript(const SecurityHandshake::SignedChallenge &challenge,
                               const SecurityHandshake::SignedResponse &response,
                               const std::string &serverSignatureHex) {
  return "TransferUDT signed handshake v1|kdf|" + response.clientId + "|" +
         challenge.serverNonceHex + "|" +
         challenge.serverEphemeralPublicKeyHex + "|" + response.clientNonceHex +
         "|" + response.clientEphemeralPublicKeyHex + "|" +
         response.signatureHex + "|" + serverSignatureHex;
}

void validateSignedChallenge(const SecurityHandshake::SignedChallenge &challenge) {
  (void)fromHex(challenge.serverNonceHex, SecurityHandshake::kChallengeBytes,
                "server nonce");
  (void)fromHex(challenge.serverEphemeralPublicKeyHex,
                SecurityHandshake::kX25519KeyBytes,
                "server ephemeral public key");
}

void validateSignedResponse(const SecurityHandshake::SignedResponse &response) {
  if (!isValidClientId(response.clientId)) {
    throw std::invalid_argument("Invalid signed handshake client id.");
  }
  (void)fromHex(response.clientNonceHex, SecurityHandshake::kChallengeBytes,
                "client nonce");
  (void)fromHex(response.clientEphemeralPublicKeyHex,
                SecurityHandshake::kX25519KeyBytes,
                "client ephemeral public key");
  if (response.signatureHex.empty() || response.signatureHex.size() % 2 != 0 ||
      !isLowerHex(response.signatureHex)) {
    throw std::invalid_argument("Invalid signed handshake signature.");
  }
}

std::vector<unsigned char> deriveX25519SharedSecret(
    const std::string &privateKeyHex, const std::string &publicKeyHex) {
  auto privateBytes = fromHex(privateKeyHex, SecurityHandshake::kX25519KeyBytes,
                              "ephemeral private key");
  auto publicBytes = fromHex(publicKeyHex, SecurityHandshake::kX25519KeyBytes,
                             "ephemeral public key");

  EvpPkeyPtr privateKey(
      EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr,
                                   privateBytes.data(), privateBytes.size()),
      EVP_PKEY_free);
  EvpPkeyPtr publicKey(
      EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr, publicBytes.data(),
                                  publicBytes.size()),
      EVP_PKEY_free);
  if (!privateKey || !publicKey) {
    throw std::runtime_error("Failed to create X25519 key objects.");
  }

  EvpPkeyCtxPtr ctx(EVP_PKEY_CTX_new(privateKey.get(), nullptr),
                    EVP_PKEY_CTX_free);
  if (!ctx || EVP_PKEY_derive_init(ctx.get()) <= 0 ||
      EVP_PKEY_derive_set_peer(ctx.get(), publicKey.get()) <= 0) {
    throw std::runtime_error("Failed to initialize X25519 derivation.");
  }

  size_t secretLen = 0;
  if (EVP_PKEY_derive(ctx.get(), nullptr, &secretLen) <= 0 ||
      secretLen == 0) {
    throw std::runtime_error("Failed to size X25519 shared secret.");
  }
  std::vector<unsigned char> secret(secretLen);
  if (EVP_PKEY_derive(ctx.get(), secret.data(), &secretLen) <= 0) {
    throw std::runtime_error("Failed to derive X25519 shared secret.");
  }
  secret.resize(secretLen);
  return secret;
}

std::string hkdfSessionSecretHex(const std::vector<unsigned char> &sharedSecret,
                                 const std::string &transcript) {
  std::vector<unsigned char> key(32);
  EvpPkeyCtxPtr ctx(EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr),
                    EVP_PKEY_CTX_free);
  if (!ctx) {
    throw std::runtime_error("Failed to allocate signed session HKDF context.");
  }
  const unsigned char salt[] = "TransferUDT signed handshake v1";
  const unsigned char *info =
      reinterpret_cast<const unsigned char *>(transcript.data());
  size_t keyLen = key.size();
  if (EVP_PKEY_derive_init(ctx.get()) <= 0 ||
      EVP_PKEY_CTX_set_hkdf_md(ctx.get(), EVP_sha256()) <= 0 ||
      EVP_PKEY_CTX_set1_hkdf_salt(ctx.get(), salt,
                                  static_cast<int>(sizeof(salt) - 1)) <= 0 ||
      EVP_PKEY_CTX_set1_hkdf_key(ctx.get(), sharedSecret.data(),
                                 static_cast<int>(sharedSecret.size())) <= 0 ||
      EVP_PKEY_CTX_add1_hkdf_info(ctx.get(), info,
                                  static_cast<int>(transcript.size())) <= 0 ||
      EVP_PKEY_derive(ctx.get(), key.data(), &keyLen) <= 0 ||
      keyLen != key.size()) {
    throw std::runtime_error("Failed to derive signed session secret.");
  }
  return toHex(key.data(), key.size());
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

std::string CreateSessionId() {
  std::vector<unsigned char> sessionId(kSessionIdBytes);
  if (RAND_bytes(sessionId.data(), static_cast<int>(sessionId.size())) != 1) {
    throw std::runtime_error("Failed to generate secure session id.");
  }
  return toHex(sessionId.data(), sessionId.size());
}

EphemeralKeyPair CreateEphemeralKeyPair() {
  EvpPkeyCtxPtr ctx(EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, nullptr),
                    EVP_PKEY_CTX_free);
  if (!ctx || EVP_PKEY_keygen_init(ctx.get()) <= 0) {
    throw std::runtime_error("Failed to initialize X25519 key generation.");
  }
  EVP_PKEY *rawKey = nullptr;
  if (EVP_PKEY_keygen(ctx.get(), &rawKey) <= 0 || rawKey == nullptr) {
    throw std::runtime_error("Failed to generate X25519 key pair.");
  }
  EvpPkeyPtr key(rawKey, EVP_PKEY_free);

  std::vector<unsigned char> privateKey(kX25519KeyBytes);
  std::vector<unsigned char> publicKey(kX25519KeyBytes);
  size_t privateLen = privateKey.size();
  size_t publicLen = publicKey.size();
  if (EVP_PKEY_get_raw_private_key(key.get(), privateKey.data(),
                                   &privateLen) != 1 ||
      EVP_PKEY_get_raw_public_key(key.get(), publicKey.data(), &publicLen) !=
          1 ||
      privateLen != privateKey.size() || publicLen != publicKey.size()) {
    throw std::runtime_error("Failed to export X25519 key pair.");
  }

  return {toHex(privateKey.data(), privateKey.size()),
          toHex(publicKey.data(), publicKey.size())};
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

std::string DerivePskSessionSecret(const std::string &challengeHex,
                                   const std::string &preSharedKey,
                                   const std::string &clientId) {
  if (challengeHex.size() != kChallengeBytes * 2 ||
      !isLowerHex(challengeHex)) {
    throw std::invalid_argument("Invalid authentication challenge.");
  }
  if (preSharedKey.empty()) {
    throw std::invalid_argument("Cannot derive a session secret from an empty PSK.");
  }
  if (!clientId.empty() && !isValidClientId(clientId)) {
    throw std::invalid_argument("Invalid authentication client id.");
  }
  return hmacSha256Hex("TransferUDT PSK session v1|" + challengeHex + "|" +
                           clientId,
                       preSharedKey);
}

std::string BuildSecureSessionMessage(const std::string &sessionIdHex,
                                      const std::string &sessionSecret) {
  if (!isValidSessionId(sessionIdHex)) {
    throw std::invalid_argument("Invalid secure session id.");
  }
  if (sessionSecret.empty()) {
    throw std::invalid_argument("Cannot authenticate a session with an empty secret.");
  }
  const std::string transcript =
      "TransferUDT secure session v1|" + sessionIdHex;
  return std::string(kSecureSessionPrefix) + sessionIdHex + " " +
         hmacSha256Hex(transcript, sessionSecret);
}

bool TryParseSecureSessionMessage(const std::string &message,
                                  const std::string &sessionSecret,
                                  std::string &sessionIdHex) {
  sessionIdHex.clear();
  if (sessionSecret.empty()) {
    return false;
  }

  const std::string prefix(kSecureSessionPrefix);
  if (message.rfind(prefix, 0) != 0) {
    return false;
  }

  const std::string payload = message.substr(prefix.size());
  const size_t separator = payload.find(' ');
  if (separator == std::string::npos ||
      payload.find(' ', separator + 1) != std::string::npos) {
    return false;
  }

  const std::string candidateSessionId = payload.substr(0, separator);
  const std::string signature = payload.substr(separator + 1);
  if (!isValidSessionId(candidateSessionId) || signature.size() != 64 ||
      !isLowerHex(signature)) {
    return false;
  }

  const std::string expected = hmacSha256Hex(
      "TransferUDT secure session v1|" + candidateSessionId, sessionSecret);
  if (!constantTimeEquals(signature, expected)) {
    return false;
  }

  sessionIdHex = candidateSessionId;
  return true;
}

std::string BuildSignedChallengeMessage(const SignedChallenge &challenge) {
  validateSignedChallenge(challenge);
  return std::string(kSignedChallengePrefix) + challenge.serverNonceHex + " " +
         challenge.serverEphemeralPublicKeyHex;
}

bool TryParseSignedChallengeMessage(const std::string &message,
                                    SignedChallenge &challenge) {
  const std::string prefix(kSignedChallengePrefix);
  if (message.rfind(prefix, 0) != 0) {
    return false;
  }
  const std::string payload = message.substr(prefix.size());
  const size_t separator = payload.find(' ');
  if (separator == std::string::npos) {
    return false;
  }
  challenge.serverNonceHex = payload.substr(0, separator);
  challenge.serverEphemeralPublicKeyHex = payload.substr(separator + 1);
  try {
    validateSignedChallenge(challenge);
    return true;
  } catch (const std::exception &) {
    return false;
  }
}

std::string BuildSignedResponseMessage(const SignedChallenge &challenge,
                                       const std::string &clientId,
                                       const std::string &clientNonceHex,
                                       const std::string &clientPublicKeyHex,
                                       const std::string &clientPrivateKeyPath) {
  validateSignedChallenge(challenge);
  SignedResponse response{clientId, clientNonceHex, clientPublicKeyHex, ""};
  validateSignedResponse({clientId, clientNonceHex, clientPublicKeyHex, "00"});
  const std::string transcript =
      buildClientTranscript(challenge, clientId, clientNonceHex,
                            clientPublicKeyHex);
  response.signatureHex = signMessageHex(transcript, clientPrivateKeyPath);
  return std::string(kSignedResponsePrefix) + response.clientId + " " +
         response.clientNonceHex + " " + response.clientEphemeralPublicKeyHex +
         " " + response.signatureHex;
}

bool TryParseSignedResponseMessage(const std::string &message,
                                   SignedResponse &response) {
  const std::string prefix(kSignedResponsePrefix);
  if (message.rfind(prefix, 0) != 0) {
    return false;
  }
  const std::string payload = message.substr(prefix.size());
  std::vector<std::string> parts;
  size_t start = 0;
  while (start <= payload.size()) {
    const size_t separator = payload.find(' ', start);
    if (separator == std::string::npos) {
      parts.push_back(payload.substr(start));
      break;
    }
    parts.push_back(payload.substr(start, separator - start));
    start = separator + 1;
  }
  if (parts.size() != 4) {
    return false;
  }
  response = {parts[0], parts[1], parts[2], parts[3]};
  try {
    validateSignedResponse(response);
    return true;
  } catch (const std::exception &) {
    return false;
  }
}

bool VerifySignedResponseMessage(const SignedChallenge &challenge,
                                 const SignedResponse &response,
                                 const std::string &clientPublicKeyPath) {
  validateSignedChallenge(challenge);
  validateSignedResponse(response);
  const std::string transcript =
      buildClientTranscript(challenge, response.clientId,
                            response.clientNonceHex,
                            response.clientEphemeralPublicKeyHex);
  return verifyMessageSignatureHex(transcript, response.signatureHex,
                                   clientPublicKeyPath);
}

std::string BuildSignedOkMessage(const SignedChallenge &challenge,
                                 const SignedResponse &response,
                                 const std::string &serverPrivateKeyPath) {
  validateSignedChallenge(challenge);
  validateSignedResponse(response);
  const std::string transcript = buildServerTranscript(challenge, response);
  const std::string signatureHex =
      signMessageHex(transcript, serverPrivateKeyPath);
  return std::string(kSignedOkPrefix) + signatureHex;
}

bool VerifySignedOkMessage(const SignedChallenge &challenge,
                           const SignedResponse &response,
                           const std::string &signedOkMessage,
                           const std::string &serverPublicKeyPath,
                           std::string *serverSignatureHex) {
  const std::string prefix(kSignedOkPrefix);
  if (signedOkMessage.rfind(prefix, 0) != 0) {
    return false;
  }
  const std::string signatureHex = signedOkMessage.substr(prefix.size());
  validateSignedChallenge(challenge);
  validateSignedResponse(response);
  const std::string transcript = buildServerTranscript(challenge, response);
  const bool valid =
      verifyMessageSignatureHex(transcript, signatureHex, serverPublicKeyPath);
  if (valid && serverSignatureHex != nullptr) {
    *serverSignatureHex = signatureHex;
  }
  return valid;
}

std::string SignMessageHex(const std::string &message,
                           const std::string &privateKeyPath) {
  return signMessageHex(message, privateKeyPath);
}

bool VerifyMessageSignatureHex(const std::string &message,
                               const std::string &signatureHex,
                               const std::string &publicKeyPath) {
  return verifyMessageSignatureHex(message, signatureHex, publicKeyPath);
}

std::string DeriveSignedSessionSecret(
    const std::string &ownEphemeralPrivateKeyHex,
    const std::string &peerEphemeralPublicKeyHex,
    const SignedChallenge &challenge, const SignedResponse &response,
    const std::string &serverSignatureHex) {
  validateSignedChallenge(challenge);
  validateSignedResponse(response);
  if (serverSignatureHex.empty() || serverSignatureHex.size() % 2 != 0 ||
      !isLowerHex(serverSignatureHex)) {
    throw std::invalid_argument("Invalid signed handshake server signature.");
  }
  const auto sharedSecret =
      deriveX25519SharedSecret(ownEphemeralPrivateKeyHex,
                               peerEphemeralPublicKeyHex);
  return hkdfSessionSecretHex(
      sharedSecret, buildKdfTranscript(challenge, response, serverSignatureHex));
}

} // namespace SecurityHandshake
