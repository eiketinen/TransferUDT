#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace SecurityHandshake {

constexpr const char *kChallengePrefix = "AUTH_CHALLENGE_V1 ";
constexpr const char *kResponsePrefix = "AUTH_RESPONSE_V1 ";
constexpr const char *kSignedChallengePrefix = "AUTH_SIGNED_CHALLENGE_V1 ";
constexpr const char *kSignedResponsePrefix = "AUTH_SIGNED_RESPONSE_V1 ";
constexpr const char *kSignedOkPrefix = "AUTH_SIGNED_OK_V1 ";
constexpr const char *kAuthFailed = "AUTH_FAILED";
constexpr size_t kChallengeBytes = 32;
constexpr size_t kX25519KeyBytes = 32;

struct EphemeralKeyPair {
  std::string privateKeyHex;
  std::string publicKeyHex;
};

struct SignedChallenge {
  std::string serverNonceHex;
  std::string serverEphemeralPublicKeyHex;
};

struct SignedResponse {
  std::string clientId;
  std::string clientNonceHex;
  std::string clientEphemeralPublicKeyHex;
  std::string signatureHex;
};

std::string CreateChallenge();
EphemeralKeyPair CreateEphemeralKeyPair();
std::string BuildChallengeMessage(const std::string &challengeHex);
bool TryParseChallengeMessage(const std::string &message,
                              std::string &challengeHex);
bool TryExtractClientId(const std::string &responseMessage,
                        std::string &clientId);

std::string BuildResponseMessage(const std::string &challengeHex,
                                  const std::string &preSharedKey);
std::string BuildResponseMessage(const std::string &challengeHex,
                                  const std::string &preSharedKey,
                                  const std::string &clientId);
bool VerifyResponseMessage(const std::string &challengeHex,
                            const std::string &preSharedKey,
                            const std::string &responseMessage,
                            std::string *clientId = nullptr);

std::string BuildSignedChallengeMessage(const SignedChallenge &challenge);
bool TryParseSignedChallengeMessage(const std::string &message,
                                    SignedChallenge &challenge);

std::string BuildSignedResponseMessage(const SignedChallenge &challenge,
                                       const std::string &clientId,
                                       const std::string &clientNonceHex,
                                       const std::string &clientPublicKeyHex,
                                       const std::string &clientPrivateKeyPath);
bool TryParseSignedResponseMessage(const std::string &message,
                                   SignedResponse &response);
bool VerifySignedResponseMessage(const SignedChallenge &challenge,
                                 const SignedResponse &response,
                                 const std::string &clientPublicKeyPath);

std::string BuildSignedOkMessage(const SignedChallenge &challenge,
                                 const SignedResponse &response,
                                 const std::string &serverPrivateKeyPath);
bool VerifySignedOkMessage(const SignedChallenge &challenge,
                           const SignedResponse &response,
                           const std::string &signedOkMessage,
                           const std::string &serverPublicKeyPath,
                           std::string *serverSignatureHex = nullptr);

std::string SignMessageHex(const std::string &message,
                           const std::string &privateKeyPath);
bool VerifyMessageSignatureHex(const std::string &message,
                               const std::string &signatureHex,
                               const std::string &publicKeyPath);

std::string DeriveSignedSessionSecret(
    const std::string &ownEphemeralPrivateKeyHex,
    const std::string &peerEphemeralPublicKeyHex,
    const SignedChallenge &challenge, const SignedResponse &response,
    const std::string &serverSignatureHex);

} // namespace SecurityHandshake
