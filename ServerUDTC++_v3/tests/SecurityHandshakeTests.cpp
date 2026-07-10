#include "tests/TestSuites.h"

#include "SecurityHandshake.h"

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

#include <filesystem>
#include <memory>

namespace {
using EvpPkeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using EvpPkeyCtxPtr =
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;
using BioPtr = std::unique_ptr<BIO, decltype(&BIO_free)>;

void writeRsaKeyPair(const std::filesystem::path &privatePath,
                     const std::filesystem::path &publicPath) {
  EvpPkeyCtxPtr ctx(EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr),
                    EVP_PKEY_CTX_free);
  require(ctx != nullptr, "RSA keygen context should be allocated");
  require(EVP_PKEY_keygen_init(ctx.get()) == 1,
          "RSA keygen should initialize");
  require(EVP_PKEY_CTX_set_rsa_keygen_bits(ctx.get(), 2048) == 1,
          "RSA key size should be configured");

  EVP_PKEY *rawKey = nullptr;
  require(EVP_PKEY_keygen(ctx.get(), &rawKey) == 1 && rawKey != nullptr,
          "RSA key pair should be generated");
  EvpPkeyPtr key(rawKey, EVP_PKEY_free);

  BioPtr privateBio(BIO_new_file(privatePath.string().c_str(), "wb"),
                    BIO_free);
  require(privateBio != nullptr, "private key PEM should open for write");
  require(PEM_write_bio_PrivateKey(privateBio.get(), key.get(), nullptr,
                                   nullptr, 0, nullptr, nullptr) == 1,
          "private key PEM should be written");

  BioPtr publicBio(BIO_new_file(publicPath.string().c_str(), "wb"), BIO_free);
  require(publicBio != nullptr, "public key PEM should open for write");
  require(PEM_write_bio_PUBKEY(publicBio.get(), key.get()) == 1,
          "public key PEM should be written");
}
} // namespace

void runSecurityHandshakeTests(TestStats &stats, const TestEnvironment &env) {
  runTest("SecurityHandshake signed mode authenticates and derives session key",
          [&]() {
            const auto clientPrivate = env.tempRoot / "agent.key";
            const auto clientPublic = env.tempRoot / "agent.pub";
            const auto serverPrivate = env.tempRoot / "server.key";
            const auto serverPublic = env.tempRoot / "server.pub";
            const auto wrongServerPrivate = env.tempRoot / "wrong-server.key";
            const auto wrongServerPublic = env.tempRoot / "wrong-server.pub";

            writeRsaKeyPair(clientPrivate, clientPublic);
            writeRsaKeyPair(serverPrivate, serverPublic);
            writeRsaKeyPair(wrongServerPrivate, wrongServerPublic);

            const auto serverEphemeral =
                SecurityHandshake::CreateEphemeralKeyPair();
            const SecurityHandshake::SignedChallenge challenge{
                SecurityHandshake::CreateChallenge(),
                serverEphemeral.publicKeyHex};

            const std::string challengeMessage =
                SecurityHandshake::BuildSignedChallengeMessage(challenge);
            SecurityHandshake::SignedChallenge parsedChallenge;
            require(SecurityHandshake::TryParseSignedChallengeMessage(
                        challengeMessage, parsedChallenge),
                    "signed challenge should parse");
            require(parsedChallenge.serverNonceHex == challenge.serverNonceHex,
                    "server nonce should round-trip");

            const auto clientEphemeral =
                SecurityHandshake::CreateEphemeralKeyPair();
            const std::string clientNonce = SecurityHandshake::CreateChallenge();
            const std::string responseMessage =
                SecurityHandshake::BuildSignedResponseMessage(
                    parsedChallenge, "agent-one", clientNonce,
                    clientEphemeral.publicKeyHex, clientPrivate.string());

            SecurityHandshake::SignedResponse response;
            require(SecurityHandshake::TryParseSignedResponseMessage(
                        responseMessage, response),
                    "signed response should parse");
            require(SecurityHandshake::VerifySignedResponseMessage(
                        parsedChallenge, response, clientPublic.string()),
                    "client signature should verify");

            auto tamperedResponse = response;
            tamperedResponse.clientId = "agent-two";
            require(!SecurityHandshake::VerifySignedResponseMessage(
                        parsedChallenge, tamperedResponse,
                        clientPublic.string()),
                    "tampered client id should fail signature verification");

            const std::string okMessage =
                SecurityHandshake::BuildSignedOkMessage(
                    parsedChallenge, response, serverPrivate.string());
            std::string serverSignature;
            require(SecurityHandshake::VerifySignedOkMessage(
                        parsedChallenge, response, okMessage,
                        serverPublic.string(), &serverSignature),
                    "server signature should verify");
            require(!SecurityHandshake::VerifySignedOkMessage(
                        parsedChallenge, response, okMessage,
                        wrongServerPublic.string(), nullptr),
                    "wrong server public key should fail verification");

            const std::string serverSession =
                SecurityHandshake::DeriveSignedSessionSecret(
                    serverEphemeral.privateKeyHex,
                    response.clientEphemeralPublicKeyHex, parsedChallenge,
                    response, serverSignature);
            const std::string clientSession =
                SecurityHandshake::DeriveSignedSessionSecret(
                    clientEphemeral.privateKeyHex,
                    parsedChallenge.serverEphemeralPublicKeyHex,
                    parsedChallenge, response, serverSignature);

            require(serverSession == clientSession,
                    "client and server should derive the same session secret");
            require(serverSession.size() == 64,
                    "session secret should be a 32-byte hex value");
          },
          stats);

  runTest("SecurityHandshake authenticates a PSK-bound transfer session",
          [&]() {
            const std::string challenge =
                "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
            const std::string psk =
                "0123456789abcdef0123456789abcdef-secure-test-key";
            const std::string sessionSecret =
                SecurityHandshake::DerivePskSessionSecret(challenge, psk,
                                                           "agent-one");
            require(sessionSecret.size() == 64,
                    "PSK session secret should be a 32-byte hex value");
            require(sessionSecret !=
                        SecurityHandshake::DerivePskSessionSecret(
                            challenge, psk, "agent-two"),
                    "Client identity must contribute to the PSK session secret");

            const std::string sessionId = SecurityHandshake::CreateSessionId();
            require(sessionId.size() == SecurityHandshake::kSessionIdBytes * 2,
                    "Session id should contain 16 random bytes encoded as hex");
            const std::string message =
                SecurityHandshake::BuildSecureSessionMessage(sessionId,
                                                              sessionSecret);
            std::string parsedSessionId;
            require(SecurityHandshake::TryParseSecureSessionMessage(
                        message, sessionSecret, parsedSessionId),
                    "Authenticated secure session message should parse");
            require(parsedSessionId == sessionId,
                    "Authenticated secure session id should round-trip");
            require(!SecurityHandshake::TryParseSecureSessionMessage(
                        message, sessionSecret + "tampered", parsedSessionId),
                    "Session message must reject an incorrect secret");
          },
          stats);
}
