#include "tests/TestSuites.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma warning(push)
#pragma warning(disable : 4251)
#include <udt/udt.h>
#pragma warning(pop)

#include "ClientHandler.h"
#include "FileReceiver.h"
#include "Interfaces.h"
#include "SecurityHandshake.h"
#include "SecurePacket.h"
#include "ServerConfig.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr const char *kTestTransferId =
    "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";
namespace fs = std::filesystem;

class ConstantVerifier final : public IFileIntegrityVerifier {
public:
  explicit ConstantVerifier(std::string hash) : hash_(std::move(hash)) {}

  std::string calculateFileHash(const std::string &) override { return hash_; }

  std::string calculateChunkHash(const std::vector<char> &) override {
    return hash_;
  }

private:
  std::string hash_;
};

class UdtRuntime {
public:
  UdtRuntime() {
    if (UDT::startup() != 0) {
      throw std::runtime_error("UDT::startup failed: " +
                               std::string(UDT::getlasterror().getErrorMessage()));
    }
  }

  ~UdtRuntime() { UDT::cleanup(); }
};

class UdtSocket {
public:
  UdtSocket() = default;
  explicit UdtSocket(UDTSOCKET socket) : socket_(socket) {}
  ~UdtSocket() { close(); }

  UdtSocket(const UdtSocket &) = delete;
  UdtSocket &operator=(const UdtSocket &) = delete;

  UdtSocket(UdtSocket &&other) noexcept : socket_(other.socket_) {
    other.socket_ = UDT::INVALID_SOCK;
  }

  UdtSocket &operator=(UdtSocket &&other) noexcept {
    if (this != &other) {
      close();
      socket_ = other.socket_;
      other.socket_ = UDT::INVALID_SOCK;
    }
    return *this;
  }

  UDTSOCKET get() const { return socket_; }

  UDTSOCKET release() {
    const UDTSOCKET socket = socket_;
    socket_ = UDT::INVALID_SOCK;
    return socket;
  }

  void reset(UDTSOCKET socket) {
    close();
    socket_ = socket;
  }

  void close() {
    if (socket_ != UDT::INVALID_SOCK) {
      UDT::close(socket_);
      socket_ = UDT::INVALID_SOCK;
    }
  }

private:
  UDTSOCKET socket_{UDT::INVALID_SOCK};
};

void appendU32(std::vector<char> &out, uint32_t value) {
  const uint32_t network = htonl(value);
  const char *ptr = reinterpret_cast<const char *>(&network);
  out.insert(out.end(), ptr, ptr + sizeof(uint32_t));
}

void appendU64(std::vector<char> &out, uint64_t value) {
  appendU32(out, static_cast<uint32_t>(value >> 32));
  appendU32(out, static_cast<uint32_t>(value & 0xFFFFFFFF));
}

void appendString(std::vector<char> &out, const std::string &value) {
  appendU32(out, static_cast<uint32_t>(value.size()));
  out.insert(out.end(), value.begin(), value.end());
}

std::vector<char> buildChunkPacket(const std::string &filename,
                                   const std::string &directory,
                                   const std::string &serverAddress,
                                   uint32_t chunkNumber, uint32_t totalChunks,
                                   uint64_t chunkOffset,
                                   uint64_t totalFileSize,
                                   const std::string &hash,
                                   const std::vector<char> &chunkData,
                                   const std::string &transferId =
                                       kTestTransferId) {
  std::vector<char> payload;
  appendString(payload, filename);
  appendString(payload, directory);
  appendString(payload, serverAddress);
  appendString(payload, transferId);
  appendU32(payload, chunkNumber);
  appendU32(payload, totalChunks);
  appendU64(payload, chunkOffset);
  appendU64(payload, totalFileSize);
  appendString(payload, hash);
  appendU32(payload, static_cast<uint32_t>(chunkData.size()));
  payload.insert(payload.end(), chunkData.begin(), chunkData.end());

  std::vector<char> packet;
  appendU32(packet, static_cast<uint32_t>(payload.size()));
  packet.insert(packet.end(), payload.begin(), payload.end());
  return packet;
}

bool sendAll(UDTSOCKET socket, const char *data, int size) {
  int sent = 0;
  while (sent < size) {
    const int ret = UDT::send(socket, data + sent, size - sent, 0);
    if (ret <= 0) {
      return false;
    }
    sent += ret;
  }
  return true;
}

bool recvAll(UDTSOCKET socket, char *data, int size) {
  int received = 0;
  while (received < size) {
    const int ret = UDT::recv(socket, data + received, size - received, 0);
    if (ret <= 0) {
      return false;
    }
    received += ret;
  }
  return true;
}

bool sendString(UDTSOCKET socket, const std::string &value) {
  const uint32_t len = htonl(static_cast<uint32_t>(value.size()));
  if (!sendAll(socket, reinterpret_cast<const char *>(&len), sizeof(len))) {
    return false;
  }
  return value.empty() ||
         sendAll(socket, value.data(), static_cast<int>(value.size()));
}

bool recvString(UDTSOCKET socket, std::string &value) {
  uint32_t lenNetwork = 0;
  if (!recvAll(socket, reinterpret_cast<char *>(&lenNetwork),
               sizeof(lenNetwork))) {
    return false;
  }
  const uint32_t len = ntohl(lenNetwork);
  if (len > 65536) {
    return false;
  }
  std::vector<char> buffer(len);
  if (len > 0 && !recvAll(socket, buffer.data(), static_cast<int>(len))) {
    return false;
  }
  value.assign(buffer.begin(), buffer.end());
  return true;
}

struct SecureTestSession {
  std::string key;
  std::string sessionId;
  std::string greeting;
  uint64_t nextOutboundSequence = 1;
  uint64_t expectedInboundSequence = 1;
};

uint64_t maxSecureControlContentLength() {
  return SecurePacket::MaxSessionEncryptedContentLength(
      sizeof(uint32_t) + SecurePacket::kMaxControlMessageBytes);
}

bool recvControlMessage(UDTSOCKET socket, bool secure,
                        SecureTestSession *session, std::string &value) {
  if (!secure) {
    return recvString(socket, value);
  }
  if (session == nullptr) {
    return false;
  }

  uint32_t totalLengthNetwork = 0;
  if (!recvAll(socket, reinterpret_cast<char *>(&totalLengthNetwork),
               sizeof(totalLengthNetwork))) {
    return false;
  }

  const uint32_t totalLength = ntohl(totalLengthNetwork);
  if (totalLength == 0 || totalLength > maxSecureControlContentLength()) {
    return false;
  }

  std::vector<char> buffer(sizeof(uint32_t) + totalLength);
  std::memcpy(buffer.data(), &totalLengthNetwork, sizeof(totalLengthNetwork));
  if (!recvAll(socket, buffer.data() + sizeof(uint32_t),
               static_cast<int>(totalLength))) {
    return false;
  }

  try {
    SecurePacket::SessionMetadata metadata;
    value = SecurePacket::DecryptControlMessage(
        buffer, session->key, session->sessionId, &metadata);
    if (metadata.sequenceNumber != session->expectedInboundSequence) {
      return false;
    }
    ++session->expectedInboundSequence;
    return true;
  } catch (const std::exception &) {
    return false;
  }
}

bool sendControlMessage(UDTSOCKET socket, bool secure,
                        SecureTestSession *session, const std::string &value) {
  if (!secure) {
    return sendString(socket, value);
  }
  if (session == nullptr) {
    return false;
  }

  std::vector<char> encrypted = SecurePacket::EncryptControlMessage(
      value, session->key, session->sessionId, session->nextOutboundSequence++);
  return sendAll(socket, encrypted.data(), static_cast<int>(encrypted.size()));
}

UdtSocket createListener(uint16_t &port) {
  UdtSocket listener(
      UDT::socket(AF_INET, SOCK_STREAM, IPPROTO_UDP));
  require(listener.get() != UDT::INVALID_SOCK,
          "Should create UDT listener socket");

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(0);

  require(UDT::bind(listener.get(), reinterpret_cast<sockaddr *>(&address),
                    sizeof(address)) != UDT::ERROR,
          "Should bind UDT listener: " +
              std::string(UDT::getlasterror().getErrorMessage()));
  require(UDT::listen(listener.get(), 1) != UDT::ERROR,
          "Should listen on UDT socket: " +
              std::string(UDT::getlasterror().getErrorMessage()));

  sockaddr_in bound{};
  int boundLen = sizeof(bound);
  require(UDT::getsockname(listener.get(), reinterpret_cast<sockaddr *>(&bound),
                           &boundLen) != UDT::ERROR,
          "Should read listener port");
  port = ntohs(bound.sin_port);
  return listener;
}

UdtSocket connectClient(uint16_t port) {
  UdtSocket client(UDT::socket(AF_INET, SOCK_STREAM, IPPROTO_UDP));
  require(client.get() != UDT::INVALID_SOCK,
          "Should create UDT client socket");

  int timeoutMs = 5000;
  UDT::setsockopt(client.get(), 0, UDT_RCVTIMEO, &timeoutMs,
                  sizeof(timeoutMs));
  UDT::setsockopt(client.get(), 0, UDT_SNDTIMEO, &timeoutMs,
                  sizeof(timeoutMs));

  sockaddr_in server{};
  server.sin_family = AF_INET;
  server.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  server.sin_port = htons(port);

  require(UDT::connect(client.get(), reinterpret_cast<sockaddr *>(&server),
                       sizeof(server)) != UDT::ERROR,
          "Should connect UDT client: " +
              std::string(UDT::getlasterror().getErrorMessage()));
  return client;
}

bool waitForFile(const fs::path &filePath, int timeoutMs) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (std::chrono::steady_clock::now() < deadline) {
    if (fs::exists(filePath)) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return fs::exists(filePath);
}

std::string readBinaryFile(const fs::path &filePath) {
  std::ifstream input(filePath, std::ios::binary);
  if (!input.is_open()) {
    return std::string();
  }
  return std::string((std::istreambuf_iterator<char>(input)),
                     std::istreambuf_iterator<char>());
}

SecureTestSession receiveReadyAfterChallenge(UDTSOCKET socket,
                                             const std::string &preSharedKey,
                                             const std::string &clientId = "") {
  std::string firstMessage;
  require(recvString(socket, firstMessage),
          "Client should receive authentication challenge");

  std::string challenge;
  require(SecurityHandshake::TryParseChallengeMessage(firstMessage, challenge),
          "Server should send authentication challenge");
  require(sendString(socket, SecurityHandshake::BuildResponseMessage(
                                 challenge, preSharedKey, clientId)),
          "Client should send authentication response");

  SecureTestSession session;
  session.key = SecurityHandshake::DerivePskSessionSecret(
      challenge, preSharedKey, clientId);
  std::string sessionMessage;
  require(recvString(socket, sessionMessage),
          "Client should receive authenticated secure session");
  require(SecurityHandshake::TryParseSecureSessionMessage(
              sessionMessage, session.key, session.sessionId),
          "Server should send a valid authenticated secure session");

  std::string greeting;
  require(recvString(socket, greeting),
          "Client should receive READY greeting after authentication");
  session.greeting = greeting;
  return session;
}
} // namespace

void runSecureTransferIntegrationTests(TestStats &stats, Database &db,
                                       const TestEnvironment &) {
  runTest(
      "Secure transfer sends encrypted chunk through ClientHandler",
      [&]() {
        auto &config = ServerConfig::getInstance();
        require(config.isSecurityEnabled(),
                "Integration test requires security.enabled=true");
        require(config.isSecurityHandshakeEnabled(),
                "Integration test requires security.handshake.enabled=true");

        UdtRuntime udt;
        ThreadPool pool(1);
        auto verifier = std::make_unique<ConstantVerifier>("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
        auto receiver = std::make_shared<FileReceiver>(db, std::move(verifier),
                                                       config, pool);

        uint16_t port = 0;
        UdtSocket listener = createListener(port);

        std::exception_ptr serverError;
        std::atomic<bool> accepted{false};
        std::thread serverThread([&]() {
          try {
            sockaddr_in clientAddress{};
            int clientAddressLen = sizeof(clientAddress);
            UdtSocket acceptedSocket(UDT::accept(
                listener.get(), reinterpret_cast<sockaddr *>(&clientAddress),
                &clientAddressLen));
            accepted.store(true);
            if (acceptedSocket.get() == UDT::INVALID_SOCK) {
              throw std::runtime_error(
                  "UDT::accept failed: " +
                  std::string(UDT::getlasterror().getErrorMessage()));
            }

            ClientHandler handler(acceptedSocket.release(), &clientAddress,
                                  receiver);
            handler.handleClient();
          } catch (...) {
            serverError = std::current_exception();
          }
        });

        UdtSocket client;
        try {
          client = connectClient(port);

          const std::string clientId = "agent-one";
          const std::string clientNamespace = "id_agent-one";
          const std::string clientPsk =
              config.getSecurityPreSharedKeyForClient(clientId);
          SecureTestSession secureSession = receiveReadyAfterChallenge(
              client.get(), clientPsk, clientId);
          require(secureSession.greeting == "READY",
                  "Unexpected server greeting: " + secureSession.greeting);

          const std::string filename = "secure_transfer_e2e.bin";
          const std::string directory = "secure_transfer/XXX/XXXX";
          const std::string payload = "encrypted payload delivered end to end";
          const std::vector<char> data(payload.begin(), payload.end());
          auto packet = buildChunkPacket(
              filename, directory, "127.0.0.1:" + std::to_string(port), 0, 1,
              0, data.size(), "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", data);
          auto encrypted = SecurePacket::EncryptPacket(
              packet, secureSession.key, secureSession.sessionId,
              secureSession.nextOutboundSequence++);
          require(SecurePacket::IsEncryptedPacket(encrypted),
                  "Client fixture should send encrypted packet");
          require(sendAll(client.get(), encrypted.data(),
                          static_cast<int>(encrypted.size())),
                  "Client should send encrypted packet");

          std::string response;
          require(recvControlMessage(client.get(), config.isSecurityEnabled(),
                                     &secureSession, response),
                  "Client should receive completion response");
          require(response == "SUCCESS_FILE_COMPLETE",
                  "Unexpected server response: " + response);
          require(sendControlMessage(client.get(), config.isSecurityEnabled(),
                                     &secureSession, "CLOSE_NOW"),
                  "Client should acknowledge completion");

          client.close();
          listener.close();
          if (serverThread.joinable()) {
            serverThread.join();
          }
          if (serverError) {
            std::rethrow_exception(serverError);
          }
          require(accepted.load(), "Server should accept the test connection");

          pool.stop();

          const fs::path reconstructedPath =
              fs::path(config.getReconstructedPath()) / clientNamespace /
              "secure_transfer" / "XXX" / "XXXX" / filename;
          require(waitForFile(reconstructedPath, 5000),
                  "Reconstructed encrypted-transfer file should exist");
          require(readBinaryFile(reconstructedPath) == payload,
                  "Reconstructed encrypted-transfer payload should match");
          require(db.isFileReconstructed(clientNamespace + "/" + directory,
                                         filename),
                  "Database should mark encrypted transfer as reconstructed");
        } catch (...) {
          client.close();
          listener.close();
          if (serverThread.joinable()) {
            serverThread.join();
          }
          pool.stop();
          throw;
        }
      },
      stats);

  runTest(
      "Secure transfer rejects plaintext chunk when security is enabled",
      [&]() {
        auto &config = ServerConfig::getInstance();
        require(config.isSecurityEnabled(),
                "Integration test requires security.enabled=true");
        require(config.isSecurityHandshakeEnabled(),
                "Integration test requires security.handshake.enabled=true");

        UdtRuntime udt;
        ThreadPool pool(1);
        auto verifier = std::make_unique<ConstantVerifier>("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
        auto receiver = std::make_shared<FileReceiver>(db, std::move(verifier),
                                                       config, pool);

        uint16_t port = 0;
        UdtSocket listener = createListener(port);

        std::exception_ptr serverError;
        std::atomic<bool> accepted{false};
        std::thread serverThread([&]() {
          try {
            sockaddr_in clientAddress{};
            int clientAddressLen = sizeof(clientAddress);
            UdtSocket acceptedSocket(UDT::accept(
                listener.get(), reinterpret_cast<sockaddr *>(&clientAddress),
                &clientAddressLen));
            accepted.store(true);
            if (acceptedSocket.get() == UDT::INVALID_SOCK) {
              throw std::runtime_error(
                  "UDT::accept failed: " +
                  std::string(UDT::getlasterror().getErrorMessage()));
            }

            ClientHandler handler(acceptedSocket.release(), &clientAddress,
                                  receiver);
            handler.handleClient();
          } catch (...) {
            serverError = std::current_exception();
          }
        });

        UdtSocket client;
        try {
          client = connectClient(port);

          const std::string clientId = "agent-one";
          const std::string clientNamespace = "id_agent-one";
          const std::string clientPsk =
              config.getSecurityPreSharedKeyForClient(clientId);
          SecureTestSession secureSession = receiveReadyAfterChallenge(
              client.get(), clientPsk, clientId);
          require(secureSession.greeting == "READY",
                  "Unexpected server greeting: " + secureSession.greeting);

          const std::string filename = "plaintext_rejected_e2e.bin";
          const std::string directory = "secure_transfer";
          const std::string payload = "plaintext payload must be rejected";
          const std::vector<char> data(payload.begin(), payload.end());
          auto plaintext = buildChunkPacket(
              filename, directory, "127.0.0.1:" + std::to_string(port), 0, 1,
              0, data.size(), "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", data);
          require(!SecurePacket::IsEncryptedPacket(plaintext),
                  "Client fixture should send plaintext packet");
          require(sendAll(client.get(), plaintext.data(),
                          static_cast<int>(plaintext.size())),
                  "Client should send plaintext packet");

          std::string response;
          require(recvControlMessage(client.get(), config.isSecurityEnabled(),
                                     &secureSession, response),
                  "Client should receive rejection response");
          require(response.find("FAILED:") == 0,
                  "Plaintext packet should be rejected with FAILED response");
          require(response == "FAILED: request rejected",
                  "Rejection should not disclose internal parser details");

          client.close();
          listener.close();
          if (serverThread.joinable()) {
            serverThread.join();
          }
          if (serverError) {
            std::rethrow_exception(serverError);
          }
          require(accepted.load(), "Server should accept the test connection");

          pool.stop();

          const fs::path reconstructedPath =
              fs::path(config.getReconstructedPath()) / clientNamespace /
              directory / filename;
          require(!fs::exists(reconstructedPath),
                  "Rejected plaintext transfer should not create output file");
          require(!db.isFileReconstructed(clientNamespace + "/" + directory,
                                          filename),
                  "Rejected plaintext transfer should not be marked reconstructed");
        } catch (...) {
          client.close();
          listener.close();
          if (serverThread.joinable()) {
            serverThread.join();
          }
          pool.stop();
          throw;
        }
      },
      stats);

  runTest(
      "Secure transfer rejects replayed session packet",
      [&]() {
        auto &config = ServerConfig::getInstance();
        require(config.isSecurityEnabled(),
                "Integration test requires security.enabled=true");
        require(config.isSecurityHandshakeEnabled(),
                "Integration test requires security.handshake.enabled=true");

        UdtRuntime udt;
        ThreadPool pool(1);
        auto verifier = std::make_unique<ConstantVerifier>(
            "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
        auto receiver = std::make_shared<FileReceiver>(db, std::move(verifier),
                                                       config, pool);

        uint16_t port = 0;
        UdtSocket listener = createListener(port);

        std::exception_ptr serverError;
        std::atomic<bool> accepted{false};
        std::thread serverThread([&]() {
          try {
            sockaddr_in clientAddress{};
            int clientAddressLen = sizeof(clientAddress);
            UdtSocket acceptedSocket(UDT::accept(
                listener.get(), reinterpret_cast<sockaddr *>(&clientAddress),
                &clientAddressLen));
            accepted.store(true);
            if (acceptedSocket.get() == UDT::INVALID_SOCK) {
              throw std::runtime_error(
                  "UDT::accept failed: " +
                  std::string(UDT::getlasterror().getErrorMessage()));
            }

            ClientHandler handler(acceptedSocket.release(), &clientAddress,
                                  receiver);
            handler.handleClient();
          } catch (...) {
            serverError = std::current_exception();
          }
        });

        UdtSocket client;
        try {
          client = connectClient(port);

          const std::string clientId = "agent-one";
          const std::string clientNamespace = "id_agent-one";
          const std::string clientPsk =
              config.getSecurityPreSharedKeyForClient(clientId);
          SecureTestSession secureSession = receiveReadyAfterChallenge(
              client.get(), clientPsk, clientId);
          require(secureSession.greeting == "READY",
                  "Unexpected server greeting: " + secureSession.greeting);

          // Keep the replay fixture isolated across repeated local test runs;
          // the database intentionally survives between integration scenarios.
          const auto fixtureId = std::to_string(
              std::chrono::steady_clock::now().time_since_epoch().count());
          const std::string filename =
              "secure_replay_rejected_e2e_" + fixtureId + ".bin";
          const std::string directory = "secure_replay_" + fixtureId;
          const std::string payload = "replay-safe";
          const std::vector<char> data(payload.begin(), payload.end());
          const uint64_t totalFileSize = data.size() * 2ULL;
          const auto packet = buildChunkPacket(
              filename, directory, "127.0.0.1:" + std::to_string(port), 0, 2,
              0, totalFileSize,
              "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
              data);
          const auto encrypted = SecurePacket::EncryptPacket(
              packet, secureSession.key, secureSession.sessionId,
              secureSession.nextOutboundSequence++);
          require(sendAll(client.get(), encrypted.data(),
                          static_cast<int>(encrypted.size())),
                  "Client should send the first secure packet");

          std::string response;
          require(recvControlMessage(client.get(), config.isSecurityEnabled(),
                                     &secureSession, response),
                  "Client should receive first chunk acknowledgement");
          require(response == "SUCCESS",
                  "First packet should be accepted before replay attempt");

          require(sendAll(client.get(), encrypted.data(),
                          static_cast<int>(encrypted.size())),
                  "Client should send replayed secure packet");
          require(!recvControlMessage(client.get(), config.isSecurityEnabled(),
                                      &secureSession, response),
                  "Replayed secure packet should close the connection");

          client.close();
          listener.close();
          if (serverThread.joinable()) {
            serverThread.join();
          }
          if (serverError) {
            std::rethrow_exception(serverError);
          }
          require(accepted.load(), "Server should accept the test connection");

          pool.stop();

          // FileReceiver preallocates the destination as soon as the first
          // chunk is accepted; reconstruction is represented by the database
          // row and must remain false after the replay closes the session.
          require(!db.isFileReconstructed(clientNamespace + "/" + directory,
                                          filename),
                  "Replay attempt must not mark the file reconstructed");
        } catch (...) {
          client.close();
          listener.close();
          if (serverThread.joinable()) {
            serverThread.join();
          }
          pool.stop();
          throw;
        }
      },
      stats);

  runTest(
      "Secure handshake rejects invalid challenge response",
      [&]() {
        auto &config = ServerConfig::getInstance();
        require(config.isSecurityHandshakeEnabled(),
                "Integration test requires security.handshake.enabled=true");

        UdtRuntime udt;
        ThreadPool pool(1);
        auto verifier = std::make_unique<ConstantVerifier>("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
        auto receiver = std::make_shared<FileReceiver>(db, std::move(verifier),
                                                       config, pool);

        uint16_t port = 0;
        UdtSocket listener = createListener(port);

        std::exception_ptr serverError;
        std::atomic<bool> accepted{false};
        std::thread serverThread([&]() {
          try {
            sockaddr_in clientAddress{};
            int clientAddressLen = sizeof(clientAddress);
            UdtSocket acceptedSocket(UDT::accept(
                listener.get(), reinterpret_cast<sockaddr *>(&clientAddress),
                &clientAddressLen));
            accepted.store(true);
            if (acceptedSocket.get() == UDT::INVALID_SOCK) {
              throw std::runtime_error(
                  "UDT::accept failed: " +
                  std::string(UDT::getlasterror().getErrorMessage()));
            }

            ClientHandler handler(acceptedSocket.release(), &clientAddress,
                                  receiver);
            handler.handleClient();
          } catch (...) {
            serverError = std::current_exception();
          }
        });

        UdtSocket client;
        try {
          client = connectClient(port);

          std::string firstMessage;
          require(recvString(client.get(), firstMessage),
                  "Client should receive authentication challenge");

          std::string challenge;
          require(SecurityHandshake::TryParseChallengeMessage(firstMessage,
                                                              challenge),
                  "Server should send authentication challenge");
          require(sendString(client.get(),
                             SecurityHandshake::BuildResponseMessage(
                                 challenge, "wrong-shared-secret-32-chars!!",
                                 "agent-one")),
                  "Client should send invalid authentication response");

          std::string response;
          require(recvString(client.get(), response),
                  "Client should receive authentication failure response");
          require(response == SecurityHandshake::kAuthFailed,
                  "Invalid challenge response should be rejected");

          client.close();
          listener.close();
          if (serverThread.joinable()) {
            serverThread.join();
          }
          if (serverError) {
            std::rethrow_exception(serverError);
          }
          require(accepted.load(), "Server should accept the test connection");

          pool.stop();
        } catch (...) {
          client.close();
          listener.close();
          if (serverThread.joinable()) {
            serverThread.join();
          }
          pool.stop();
          throw;
        }
      },
      stats);

  runTest(
      "Secure handshake rejects global PSK for client-specific identity",
      [&]() {
        auto &config = ServerConfig::getInstance();
        require(config.isSecurityHandshakeEnabled(),
                "Integration test requires security.handshake.enabled=true");
        require(config.hasClientPreSharedKeys(),
                "Integration test requires per-client PSKs");

        UdtRuntime udt;
        ThreadPool pool(1);
        auto verifier = std::make_unique<ConstantVerifier>("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
        auto receiver = std::make_shared<FileReceiver>(db, std::move(verifier),
                                                       config, pool);

        uint16_t port = 0;
        UdtSocket listener = createListener(port);

        std::exception_ptr serverError;
        std::atomic<bool> accepted{false};
        std::thread serverThread([&]() {
          try {
            sockaddr_in clientAddress{};
            int clientAddressLen = sizeof(clientAddress);
            UdtSocket acceptedSocket(UDT::accept(
                listener.get(), reinterpret_cast<sockaddr *>(&clientAddress),
                &clientAddressLen));
            accepted.store(true);
            if (acceptedSocket.get() == UDT::INVALID_SOCK) {
              throw std::runtime_error(
                  "UDT::accept failed: " +
                  std::string(UDT::getlasterror().getErrorMessage()));
            }

            ClientHandler handler(acceptedSocket.release(), &clientAddress,
                                  receiver);
            handler.handleClient();
          } catch (...) {
            serverError = std::current_exception();
          }
        });

        UdtSocket client;
        try {
          client = connectClient(port);

          std::string firstMessage;
          require(recvString(client.get(), firstMessage),
                  "Client should receive authentication challenge");

          std::string challenge;
          require(SecurityHandshake::TryParseChallengeMessage(firstMessage,
                                                              challenge),
                  "Server should send authentication challenge");
          require(sendString(client.get(),
                             SecurityHandshake::BuildResponseMessage(
                                 challenge, config.getSecurityPreSharedKey(),
                                 "agent-one")),
                  "Client should send response signed with global PSK");

          std::string response;
          require(recvString(client.get(), response),
                  "Client should receive authentication failure response");
          require(response == SecurityHandshake::kAuthFailed,
                  "Global PSK must not authenticate a per-client identity");

          client.close();
          listener.close();
          if (serverThread.joinable()) {
            serverThread.join();
          }
          if (serverError) {
            std::rethrow_exception(serverError);
          }
          require(accepted.load(), "Server should accept the test connection");

          pool.stop();
        } catch (...) {
          client.close();
          listener.close();
          if (serverThread.joinable()) {
            serverThread.join();
          }
          pool.stop();
          throw;
        }
      },
      stats);
}
