#pragma once

#include <cstddef>
#include <string>

namespace SecurityHandshake {

constexpr const char *kChallengePrefix = "AUTH_CHALLENGE_V1 ";
constexpr const char *kResponsePrefix = "AUTH_RESPONSE_V1 ";
constexpr const char *kAuthFailed = "AUTH_FAILED";
constexpr size_t kChallengeBytes = 32;

std::string CreateChallenge();
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

} // namespace SecurityHandshake
