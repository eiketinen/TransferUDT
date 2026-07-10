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
constexpr const char *kCertificateChallengePrefix = "AUTH_CERT_CHALLENGE_V1 ";
constexpr const char *kCertificateResponsePrefix = "AUTH_CERT_RESPONSE_V1 ";
constexpr const char *kCertificateOkPrefix = "AUTH_CERT_OK_V1 ";
constexpr const char *kSecureSessionPrefix = "SECURE_SESSION_V1 ";
constexpr const char *kAuthFailed = "AUTH_FAILED";
constexpr size_t kChallengeBytes = 32;
constexpr size_t kX25519KeyBytes = 32;
constexpr size_t kSessionIdBytes = 16;
constexpr size_t kMaxCertificateBundleBytes = 24 * 1024;
constexpr size_t kMaxCrlBundleBytes = 4 * 1024 * 1024;

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

struct CertificateChallenge {
  std::string serverNonceHex;
  std::string serverEphemeralPublicKeyHex;
  std::string serverCertificateHex;
};

struct CertificateResponse {
  std::string clientId;
  std::string clientNonceHex;
  std::string clientEphemeralPublicKeyHex;
  std::string clientCertificateHex;
  std::string signatureHex;
};

std::string CreateChallenge();
std::string CreateSessionId();
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

std::string DerivePskSessionSecret(const std::string &challengeHex,
                                   const std::string &preSharedKey,
                                   const std::string &clientId);

std::string BuildSecureSessionMessage(const std::string &sessionIdHex,
                                      const std::string &sessionSecret);
bool TryParseSecureSessionMessage(const std::string &message,
                                  const std::string &sessionSecret,
                                  std::string &sessionIdHex);

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

std::string LoadCertificateBundleHex(const std::string &certificatePath);
bool ValidateCertificateBundle(const std::string &certificateHex,
                               const std::string &caBundlePath,
                               const std::string &expectedIdentity,
                               bool serverCertificate,
                               const std::string &crlPath = "");
bool CertificateMatchesPrivateKey(const std::string &certificateHex,
                                  const std::string &privateKeyPath);
bool TryGetCertificateRemainingValidityDays(const std::string &certificateHex,
                                            int &remainingDays);

std::string
BuildCertificateChallengeMessage(const CertificateChallenge &challenge);
bool TryParseCertificateChallengeMessage(const std::string &message,
                                         CertificateChallenge &challenge);
std::string BuildCertificateResponseMessage(
    const CertificateChallenge &challenge, const std::string &clientId,
    const std::string &clientNonceHex,
    const std::string &clientEphemeralPublicKeyHex,
    const std::string &clientCertificatePath,
    const std::string &clientPrivateKeyPath);
bool TryParseCertificateResponseMessage(const std::string &message,
                                        CertificateResponse &response);
bool VerifyCertificateResponseMessage(const CertificateChallenge &challenge,
                                      const CertificateResponse &response,
                                      const std::string &caBundlePath,
                                      const std::string &crlPath = "");
std::string BuildCertificateOkMessage(
    const CertificateChallenge &challenge,
    const CertificateResponse &response,
    const std::string &serverPrivateKeyPath);
bool VerifyCertificateOkMessage(
    const CertificateChallenge &challenge,
    const CertificateResponse &response, const std::string &okMessage,
    const std::string &caBundlePath, const std::string &expectedServerIdentity,
    std::string *serverSignatureHex = nullptr,
    const std::string &crlPath = "");
std::string DeriveCertificateSessionSecret(
    const std::string &ownEphemeralPrivateKeyHex,
    const std::string &peerEphemeralPublicKeyHex,
    const CertificateChallenge &challenge,
    const CertificateResponse &response,
    const std::string &serverSignatureHex);

} // namespace SecurityHandshake
