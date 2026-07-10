#include "tests/TestSuites.h"

#include "SecurityHandshake.h"

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <filesystem>
#include <memory>

namespace {
using EvpPkeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using EvpPkeyCtxPtr =
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;
using BioPtr = std::unique_ptr<BIO, decltype(&BIO_free)>;
using X509Ptr = std::unique_ptr<X509, decltype(&X509_free)>;

EvpPkeyPtr generateRsaKey() {
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
  return EvpPkeyPtr(rawKey, EVP_PKEY_free);
}

void writePrivateKey(const std::filesystem::path &path, EVP_PKEY *key) {
  BioPtr bio(BIO_new_file(path.string().c_str(), "wb"), BIO_free);
  require(bio != nullptr, "private key PEM should open for write");
  require(PEM_write_bio_PrivateKey(bio.get(), key, nullptr, nullptr, 0,
                                   nullptr, nullptr) == 1,
          "private key PEM should be written");
}

void writeCertificate(const std::filesystem::path &path, X509 *certificate) {
  BioPtr bio(BIO_new_file(path.string().c_str(), "wb"), BIO_free);
  require(bio != nullptr, "certificate PEM should open for write");
  require(PEM_write_bio_X509(bio.get(), certificate) == 1,
          "certificate PEM should be written");
}

void addCertificateExtension(X509 *certificate, X509 *issuer, int nid,
                             const std::string &value) {
  X509V3_CTX context;
  X509V3_set_ctx_nodb(&context);
  X509V3_set_ctx(&context, issuer, certificate, nullptr, nullptr, 0);
  X509_EXTENSION *extension =
      X509V3_EXT_conf_nid(nullptr, &context, nid,
                          const_cast<char *>(value.c_str()));
  require(extension != nullptr, "X.509 extension should be created");
  require(X509_add_ext(certificate, extension, -1) == 1,
          "X.509 extension should be added");
  X509_EXTENSION_free(extension);
}

X509Ptr createCertificate(EVP_PKEY *subjectKey, const std::string &identity,
                          X509 *issuerCertificate, EVP_PKEY *issuerKey,
                          long serial, bool isCa,
                          const std::string &extendedKeyUsage) {
  X509Ptr certificate(X509_new(), X509_free);
  require(certificate != nullptr, "X.509 certificate should be allocated");
  require(X509_set_version(certificate.get(), 2) == 1,
          "X.509 version should be set");
  require(ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), serial) ==
              1,
          "X.509 serial should be set");
  require(X509_gmtime_adj(X509_getm_notBefore(certificate.get()), -60) !=
              nullptr &&
              X509_gmtime_adj(X509_getm_notAfter(certificate.get()),
                              24 * 60 * 60) != nullptr,
          "X.509 validity should be set");
  require(X509_set_pubkey(certificate.get(), subjectKey) == 1,
          "X.509 public key should be set");

  X509_NAME *subject = X509_get_subject_name(certificate.get());
  require(subject != nullptr &&
              X509_NAME_add_entry_by_txt(
                  subject, "CN", MBSTRING_ASC,
                  reinterpret_cast<const unsigned char *>(identity.c_str()),
                  -1, -1, 0) == 1,
          "X.509 subject should be set");
  X509 *issuer = issuerCertificate == nullptr ? certificate.get()
                                              : issuerCertificate;
  require(X509_set_issuer_name(certificate.get(),
                               X509_get_subject_name(issuer)) == 1,
          "X.509 issuer should be set");

  addCertificateExtension(certificate.get(), issuer, NID_basic_constraints,
                          isCa ? "critical,CA:TRUE" : "critical,CA:FALSE");
  addCertificateExtension(
      certificate.get(), issuer, NID_key_usage,
      isCa ? "critical,keyCertSign,cRLSign"
           : "critical,digitalSignature,keyEncipherment");
  if (!isCa) {
    addCertificateExtension(certificate.get(), issuer, NID_ext_key_usage,
                            extendedKeyUsage);
    addCertificateExtension(certificate.get(), issuer, NID_subject_alt_name,
                            "DNS:" + identity);
  }
  require(X509_sign(certificate.get(), issuerKey, EVP_sha256()) > 0,
          "X.509 certificate should be signed");
  return certificate;
}

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

  runTest("SecurityHandshake certificate mode validates mutual identity",
          [&]() {
            const auto caKeyPath = env.tempRoot / "cert-ca.key";
            const auto caCertPath = env.tempRoot / "cert-ca.pem";
            const auto wrongCaCertPath = env.tempRoot / "wrong-cert-ca.pem";
            const auto serverKeyPath = env.tempRoot / "cert-server.key";
            const auto serverCertPath = env.tempRoot / "cert-server.pem";
            const auto clientKeyPath = env.tempRoot / "cert-agent.key";
            const auto clientCertPath = env.tempRoot / "cert-agent.pem";
            const auto wrongKeyPath = env.tempRoot / "cert-wrong.key";
            const auto wrongEkuCertPath = env.tempRoot / "cert-agent-wrong-eku.pem";

            auto caKey = generateRsaKey();
            auto caCert = createCertificate(caKey.get(), "TransferUDT Test CA",
                                            nullptr, caKey.get(), 100, true, "");
            writePrivateKey(caKeyPath, caKey.get());
            writeCertificate(caCertPath, caCert.get());

            auto wrongCaKey = generateRsaKey();
            auto wrongCaCert = createCertificate(
                wrongCaKey.get(), "TransferUDT Wrong CA", nullptr,
                wrongCaKey.get(), 101, true, "");
            writeCertificate(wrongCaCertPath, wrongCaCert.get());

            auto serverKey = generateRsaKey();
            auto serverCert = createCertificate(
                serverKey.get(), "transfer-server.test", caCert.get(),
                caKey.get(), 200, false, "serverAuth");
            writePrivateKey(serverKeyPath, serverKey.get());
            writeCertificate(serverCertPath, serverCert.get());

            auto clientKey = generateRsaKey();
            auto clientCert = createCertificate(
                clientKey.get(), "agent-cert", caCert.get(), caKey.get(), 201,
                false, "clientAuth");
            writePrivateKey(clientKeyPath, clientKey.get());
            writeCertificate(clientCertPath, clientCert.get());

            auto wrongKey = generateRsaKey();
            writePrivateKey(wrongKeyPath, wrongKey.get());
            auto wrongEkuCert = createCertificate(
                clientKey.get(), "agent-cert", caCert.get(), caKey.get(), 202,
                false, "serverAuth");
            writeCertificate(wrongEkuCertPath, wrongEkuCert.get());

            const auto serverEphemeral =
                SecurityHandshake::CreateEphemeralKeyPair();
            const SecurityHandshake::CertificateChallenge challenge{
                SecurityHandshake::CreateChallenge(),
                serverEphemeral.publicKeyHex,
                SecurityHandshake::LoadCertificateBundleHex(
                    serverCertPath.string())};
            const std::string challengeMessage =
                SecurityHandshake::BuildCertificateChallengeMessage(challenge);
            SecurityHandshake::CertificateChallenge parsedChallenge;
            require(SecurityHandshake::TryParseCertificateChallengeMessage(
                        challengeMessage, parsedChallenge),
                    "certificate challenge should parse");
            SecurityHandshake::CertificateChallenge oversizedChallenge;
            const std::string oversizedMessage =
                std::string(SecurityHandshake::kCertificateChallengePrefix) +
                challenge.serverNonceHex + " " +
                challenge.serverEphemeralPublicKeyHex + " " +
                std::string(
                    (SecurityHandshake::kMaxCertificateBundleBytes + 1) * 2,
                    'a');
            require(!SecurityHandshake::TryParseCertificateChallengeMessage(
                        oversizedMessage, oversizedChallenge),
                    "oversized certificate bundle should be rejected");
            require(SecurityHandshake::ValidateCertificateBundle(
                        parsedChallenge.serverCertificateHex,
                        caCertPath.string(), "transfer-server.test", true),
                    "server certificate chain and identity should validate");
            require(!SecurityHandshake::ValidateCertificateBundle(
                        parsedChallenge.serverCertificateHex,
                        caCertPath.string(), "wrong-server.test", true),
                    "wrong server identity should be rejected");
            require(!SecurityHandshake::ValidateCertificateBundle(
                        parsedChallenge.serverCertificateHex,
                        wrongCaCertPath.string(), "transfer-server.test", true),
                    "wrong CA should be rejected");

            const auto clientEphemeral =
                SecurityHandshake::CreateEphemeralKeyPair();
            const std::string responseMessage =
                SecurityHandshake::BuildCertificateResponseMessage(
                    parsedChallenge, "agent-cert",
                    SecurityHandshake::CreateChallenge(),
                    clientEphemeral.publicKeyHex, clientCertPath.string(),
                    clientKeyPath.string());
            SecurityHandshake::CertificateResponse response;
            require(SecurityHandshake::TryParseCertificateResponseMessage(
                        responseMessage, response),
                    "certificate response should parse");
            require(SecurityHandshake::VerifyCertificateResponseMessage(
                        parsedChallenge, response, caCertPath.string()),
                    "client certificate and proof should verify");

            auto tamperedResponse = response;
            tamperedResponse.clientId = "agent-other";
            require(!SecurityHandshake::VerifyCertificateResponseMessage(
                        parsedChallenge, tamperedResponse,
                        caCertPath.string()),
                    "tampered client identity should fail certificate proof");

            const std::string wrongEkuHex =
                SecurityHandshake::LoadCertificateBundleHex(
                    wrongEkuCertPath.string());
            require(!SecurityHandshake::ValidateCertificateBundle(
                        wrongEkuHex, caCertPath.string(), "agent-cert", false),
                    "server-only EKU should be rejected for an Agent certificate");

            bool mismatchedKeyRejected = false;
            try {
              (void)SecurityHandshake::BuildCertificateResponseMessage(
                  parsedChallenge, "agent-cert",
                  SecurityHandshake::CreateChallenge(),
                  clientEphemeral.publicKeyHex, clientCertPath.string(),
                  wrongKeyPath.string());
            } catch (const std::exception &) {
              mismatchedKeyRejected = true;
            }
            require(mismatchedKeyRejected,
                    "mismatched certificate private key should be rejected");

            const std::string okMessage =
                SecurityHandshake::BuildCertificateOkMessage(
                    parsedChallenge, response, serverKeyPath.string());
            std::string serverSignature;
            require(SecurityHandshake::VerifyCertificateOkMessage(
                        parsedChallenge, response, okMessage,
                        caCertPath.string(), "transfer-server.test",
                        &serverSignature),
                    "server certificate proof should verify");

            const std::string serverSession =
                SecurityHandshake::DeriveCertificateSessionSecret(
                    serverEphemeral.privateKeyHex,
                    response.clientEphemeralPublicKeyHex, parsedChallenge,
                    response, serverSignature);
            const std::string clientSession =
                SecurityHandshake::DeriveCertificateSessionSecret(
                    clientEphemeral.privateKeyHex,
                    parsedChallenge.serverEphemeralPublicKeyHex,
                    parsedChallenge, response, serverSignature);
            require(serverSession == clientSession,
                    "certificate peers should derive the same session secret");
            require(serverSession.size() == 64,
                    "certificate session secret should be 32-byte hex");
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
