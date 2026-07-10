#include "ClientHandler.h"
#include "ChunkPacketParser.h"
#include "SecurityHandshake.h"
#include "SecurePacket.h"
#include "ServerConfig.h"

#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <thread>

/**
 * Constructs a ClientHandler instance to manage a client connection.
 *
 * @param clientSocket The client socket to manage.
 * @param clientAddress The client's address information.
 * @param fileReceiver A shared pointer to the FileReceiver instance for this
 * client.
 */
namespace {
constexpr const char *RESPONSE_FILE_ALREADY_EXISTS = "FILE_ALREADY_EXISTS";

uint64_t maxSecureControlContentLength() {
  return SecurePacket::MaxSessionEncryptedContentLength(
      sizeof(uint32_t) + SecurePacket::kMaxControlMessageBytes);
}

std::string buildAuthenticatedClientNamespace(const std::string &clientIp,
                                              const std::string &clientId) {
  if (clientId.empty()) {
    return clientIp;
  }

  std::string sanitized;
  sanitized.reserve(clientId.size());
  static constexpr char kHex[] = "0123456789abcdef";
  for (unsigned char ch : clientId) {
    if (std::isalnum(ch) || ch == '-' || ch == '.' || ch == '@') {
      sanitized.push_back(static_cast<char>(ch));
    } else {
      sanitized.push_back('_');
      sanitized.push_back(kHex[(ch >> 4) & 0x0F]);
      sanitized.push_back(kHex[ch & 0x0F]);
    }
  }

  if (sanitized.empty() || sanitized == "." || sanitized == "..") {
    return clientIp;
  }
  return "id_" + sanitized;
}
}

ClientHandler::ClientHandler(UDTSOCKET clientSocket, sockaddr_in *clientAddress,
                             std::shared_ptr<FileReceiver> fileReceiver)
    : fileReceiver(std::move(fileReceiver)),
      connection(clientSocket, clientAddress),
      circuitBreaker(connection.getClient()),
      connectionPreSharedKey(
          ServerConfig::getInstance().getSecurityPreSharedKey()) {
  Logger::getInstance().info("ClientHandler",
                             "Client handler created for socket {}",
                             connection.getClient());
}

ClientHandler::~ClientHandler() {
  Logger::getInstance().info("ClientHandler",
                             "Client handler shutting down for socket {}",
                             connection.getClient());
  connection.close();
}

void ClientHandler::handleClient() {
  std::string client = connection.getClient();
  std::string clientNoPort = connection.getClientNoPort();
  std::string clientNamespace = clientNoPort;

  if (!connection.isConnected()) {
    Logger::getInstance().error("ClientHandler", "Invalid socket for client {}",
                                client);
    return;
  }

  Logger::getInstance().info("ClientHandler", "Handling client on socket {}",
                             client);

  if (ServerConfig::getInstance().isSecurityHandshakeEnabled()) {
    try {
      if (ServerConfig::getInstance().isCertificateIdentityMode()) {
        const auto serverEphemeral =
            SecurityHandshake::CreateEphemeralKeyPair();
        const SecurityHandshake::CertificateChallenge challenge{
            SecurityHandshake::CreateChallenge(), serverEphemeral.publicKeyHex,
            SecurityHandshake::LoadCertificateBundleHex(
                ServerConfig::getInstance()
                    .getSecurityServerCertificatePath())};
        int serverRemainingDays = 0;
        const int expiryWarningDays =
            ServerConfig::getInstance()
                .getSecurityCertificateExpiryWarningDays();
        if (expiryWarningDays > 0 &&
            SecurityHandshake::TryGetCertificateRemainingValidityDays(
                challenge.serverCertificateHex, serverRemainingDays) &&
            serverRemainingDays <= expiryWarningDays) {
          Logger::getInstance().warning(
              "HandleClient", "Server certificate expires in {} day(s)",
              serverRemainingDays);
        }
        if (!connection.sendString(
                SecurityHandshake::BuildCertificateChallengeMessage(
                    challenge))) {
          Logger::getInstance().error(
              "HandleClient",
              "Failed to send certificate authentication challenge");
          circuitBreaker.reportFailure();
          return;
        }

        std::string responseMessage;
        SecurityHandshake::CertificateResponse response;
        if (!connection.recvString(responseMessage) ||
            !SecurityHandshake::TryParseCertificateResponseMessage(
                responseMessage, response) ||
            !ServerConfig::getInstance().isClientIdentityAllowed(
                response.clientId)) {
          Logger::getInstance().warning(
              "HandleClient",
              "Certificate authentication rejected client identity '{}' for {}",
              response.clientId, client);
          (void)connection.sendString(SecurityHandshake::kAuthFailed);
          circuitBreaker.reportFailure();
          return;
        }

        if (!SecurityHandshake::VerifyCertificateResponseMessage(
                challenge, response,
                ServerConfig::getInstance().getSecurityCaBundlePath(),
                ServerConfig::getInstance().getSecurityCrlPath())) {
          Logger::getInstance().warning(
              "HandleClient",
              "Certificate authentication failed for client identity '{}'",
              response.clientId);
          (void)connection.sendString(SecurityHandshake::kAuthFailed);
          circuitBreaker.reportFailure();
          return;
        }
        int clientRemainingDays = 0;
        if (expiryWarningDays > 0 &&
            SecurityHandshake::TryGetCertificateRemainingValidityDays(
                response.clientCertificateHex, clientRemainingDays) &&
            clientRemainingDays <= expiryWarningDays) {
          Logger::getInstance().warning(
              "HandleClient",
              "Certificate for client identity '{}' expires in {} day(s)",
              response.clientId, clientRemainingDays);
        }

        const std::string okMessage =
            SecurityHandshake::BuildCertificateOkMessage(
                challenge, response,
                ServerConfig::getInstance()
                    .getSecurityServerPrivateKeyPath());
        const std::string serverSignature = okMessage.substr(
            std::string(SecurityHandshake::kCertificateOkPrefix).size());
        connectionPreSharedKey =
            SecurityHandshake::DeriveCertificateSessionSecret(
                serverEphemeral.privateKeyHex,
                response.clientEphemeralPublicKeyHex, challenge, response,
                serverSignature);
        clientNamespace =
            buildAuthenticatedClientNamespace(clientNoPort, response.clientId);
        Logger::getInstance().info(
            "HandleClient",
            "Certificate authentication succeeded for client identity '{}'",
            response.clientId);

        if (!connection.sendString(okMessage)) {
          Logger::getInstance().error(
              "HandleClient", "Failed to send certificate server proof");
          circuitBreaker.reportFailure();
          return;
        }
      } else if (ServerConfig::getInstance().isSignedIdentityMode()) {
        const auto serverEphemeral =
            SecurityHandshake::CreateEphemeralKeyPair();
        const SecurityHandshake::SignedChallenge challenge{
            SecurityHandshake::CreateChallenge(),
            serverEphemeral.publicKeyHex};
        if (!connection.sendString(
                SecurityHandshake::BuildSignedChallengeMessage(challenge))) {
          Logger::getInstance().error(
              "HandleClient",
              "Failed to send signed authentication challenge to client");
          circuitBreaker.reportFailure();
          return;
        }

        std::string responseMessage;
        SecurityHandshake::SignedResponse response;
        if (!connection.recvString(responseMessage) ||
            !SecurityHandshake::TryParseSignedResponseMessage(
                responseMessage, response) ||
            !ServerConfig::getInstance().isClientIdentityAllowed(
                response.clientId)) {
          Logger::getInstance().warning(
              "HandleClient",
              "Signed authentication rejected client identity '{}' for {}",
              response.clientId, client);
          (void)connection.sendString(SecurityHandshake::kAuthFailed);
          circuitBreaker.reportFailure();
          return;
        }

        const std::string clientPublicKeyPath =
            ServerConfig::getInstance().getClientPublicKeyPath(
                response.clientId);
        if (!SecurityHandshake::VerifySignedResponseMessage(
                challenge, response, clientPublicKeyPath)) {
          Logger::getInstance().warning(
              "HandleClient",
              "Signed authentication failed for client {}", client);
          (void)connection.sendString(SecurityHandshake::kAuthFailed);
          circuitBreaker.reportFailure();
          return;
        }

        const std::string signedOkMessage =
            SecurityHandshake::BuildSignedOkMessage(
                challenge, response,
                ServerConfig::getInstance().getSecurityServerPrivateKeyPath());
        const std::string serverSignature = signedOkMessage.substr(
            std::string(SecurityHandshake::kSignedOkPrefix).size());
        connectionPreSharedKey =
            SecurityHandshake::DeriveSignedSessionSecret(
                serverEphemeral.privateKeyHex,
                response.clientEphemeralPublicKeyHex, challenge, response,
                serverSignature);
        clientNamespace =
            buildAuthenticatedClientNamespace(clientNoPort, response.clientId);
        Logger::getInstance().info(
            "HandleClient",
            "Signed authentication succeeded for client identity '{}'",
            response.clientId);

        if (!connection.sendString(signedOkMessage)) {
          Logger::getInstance().error("HandleClient",
                                      "Failed to send signed server proof");
          circuitBreaker.reportFailure();
          return;
        }
      } else {
        const std::string challenge = SecurityHandshake::CreateChallenge();
        if (!connection.sendString(
                SecurityHandshake::BuildChallengeMessage(challenge))) {
          Logger::getInstance().error(
              "HandleClient",
              "Failed to send authentication challenge to client");
          circuitBreaker.reportFailure();
          return;
        }

        std::string response;
        std::string claimedClientIdentity;
        std::string clientIdentity;
        if (!connection.recvString(response) ||
            !SecurityHandshake::TryExtractClientId(response,
                                                   claimedClientIdentity) ||
            !ServerConfig::getInstance().isClientIdentityAllowed(
                claimedClientIdentity)) {
          Logger::getInstance().warning(
              "HandleClient",
              "Authentication handshake rejected client identity '{}' for {}",
              claimedClientIdentity, client);
          (void)connection.sendString(SecurityHandshake::kAuthFailed);
          circuitBreaker.reportFailure();
          return;
        }

        const std::string clientPreSharedKey =
            ServerConfig::getInstance().getSecurityPreSharedKeyForClient(
                claimedClientIdentity);
        if (!SecurityHandshake::VerifyResponseMessage(
                challenge, clientPreSharedKey, response, &clientIdentity) ||
            clientIdentity != claimedClientIdentity) {
          Logger::getInstance().warning(
              "HandleClient",
              "Authentication handshake failed for client {}", client);
          (void)connection.sendString(SecurityHandshake::kAuthFailed);
          circuitBreaker.reportFailure();
          return;
        }
        connectionPreSharedKey = SecurityHandshake::DerivePskSessionSecret(
            challenge, clientPreSharedKey, clientIdentity);
        clientNamespace =
            buildAuthenticatedClientNamespace(clientNoPort, clientIdentity);
      }
    } catch (const std::exception &e) {
      Logger::getInstance().error("HandleClient",
                                  "Authentication handshake error for {}: {}",
                                  client, e.what());
      circuitBreaker.reportFailure();
      return;
    }
  }

  if (ServerConfig::getInstance().isSecurityEnabled()) {
    try {
      connectionSessionId = SecurityHandshake::CreateSessionId();
      if (!connection.sendString(SecurityHandshake::BuildSecureSessionMessage(
              connectionSessionId, connectionPreSharedKey))) {
        Logger::getInstance().error(
            "HandleClient",
            "Failed to send authenticated secure session to client");
        circuitBreaker.reportFailure();
        return;
      }
    } catch (const std::exception &e) {
      Logger::getInstance().error(
          "HandleClient", "Failed to establish secure session for {}: {}",
          client, e.what());
      circuitBreaker.reportFailure();
      return;
    }
  }

  if (!connection.sendString("READY")) {
    Logger::getInstance().error("HandleClient",
                                "Failed to send ready message to client");
    circuitBreaker.reportFailure();
    return;
  }

  while (running) {
    try {
      if (!circuitBreaker.shouldAttempt()) {
        Logger::getInstance().warning(
            "ClientHandler",
            "Circuit breaker prevented processing for client {}", client);
        break;
      }

      uint32_t totalLengthNetwork = 0;
      if (!connection.recvAll(reinterpret_cast<char *>(&totalLengthNetwork),
                              sizeof(totalLengthNetwork))) {
        throw std::runtime_error("Failed to receive total length.");
      }

      const uint32_t totalLength = ntohl(totalLengthNetwork);
      if (totalLength == 0) {
        throw std::runtime_error("Invalid total length received");
      }
      uint64_t maxAllowedPacketSize = ChunkPacketParser::kMaxPacketSizeBytes;
      if (ServerConfig::getInstance().isSecurityEnabled()) {
        maxAllowedPacketSize = SecurePacket::MaxSessionEncryptedContentLength(
            ChunkPacketParser::kMaxPacketSizeBytes);
      }
      if (totalLength > maxAllowedPacketSize) {
        throw std::runtime_error("Packet exceeds maximum allowed size");
      }

      std::vector<char> buffer;
      buffer.resize(sizeof(uint32_t) + totalLength);
      std::memcpy(buffer.data(), &totalLengthNetwork, sizeof(uint32_t));

      if (!connection.recvAll(buffer.data() + sizeof(uint32_t), totalLength)) {
        throw std::runtime_error("Failed to receive DATA");
      }

      if (ServerConfig::getInstance().isSecurityEnabled()) {
        if (!SecurePacket::IsEncryptedPacket(buffer)) {
          throw std::runtime_error(
              "Unencrypted packet rejected while security is enabled.");
        }
        if (connectionSessionId.empty()) {
          connection.close();
          running = false;
          throw std::runtime_error("Secure session was not established.");
        }
        try {
          SecurePacket::SessionMetadata metadata;
          buffer = SecurePacket::DecryptPacket(
              buffer, connectionPreSharedKey, connectionSessionId, &metadata);
          if (expectedSecureInboundSequence ==
                  (std::numeric_limits<uint64_t>::max)() ||
              metadata.sequenceNumber != expectedSecureInboundSequence) {
            connection.close();
            running = false;
            throw std::runtime_error(
                "Replay or out-of-order secure packet rejected.");
          }
          ++expectedSecureInboundSequence;
        } catch (const std::exception &) {
          if (connection.isConnected()) {
            connection.close();
          }
          running = false;
          throw;
        }
      }

      ChunkMetadata chunk;
      if (!parseChunkMessage(buffer, chunk)) {
        throw std::runtime_error("Failed to parse chunk message");
      }
      chunk.setClientAddress(clientNamespace);

      auto receiveResult = fileReceiver->receiveChunk(chunk, chunk.getData());
      if (receiveResult == FileReceiver::ReceiveResult::Failed) {
        Logger::getInstance().error(
            "HandleClient",
            "FileReceiver failed to process chunk{} for '{}'.Problem might be "
            "hash or storage.",
            chunk.getChunkNumber(), chunk.getFilename());
        throw std::runtime_error("Failed proccess chunk. Problem might be hash "
                                 "or storage in Server.");
      }

      if (receiveResult == FileReceiver::ReceiveResult::AlreadyCompleted) {
        Logger::getInstance().infoC(
            "HandleClient", chunk.getFilename(),
            "File already reconstructed for '{}'. Informing client and "
            "stopping further chunk reception.",
            chunk.getFilename());

        if (!sendControlMessage(RESPONSE_FILE_ALREADY_EXISTS)) {
          Logger::getInstance().errorC(
              "HandleClient", chunk.getFilename(),
              "Failed to send FILE_ALREADY_EXISTS to client {}", client);
        }

        std::string clientCommand;
        if (receiveControlMessage(clientCommand)) {
          if (clientCommand != "CLOSE_NOW") {
            Logger::getInstance().warningC(
                "HandleClient", chunk.getFilename(),
                "Unexpected command '{}' after FILE_ALREADY_EXISTS from {}.",
                clientCommand, client);
          }
        } else {
          Logger::getInstance().warningC(
              "HandleClient", chunk.getFilename(),
              "Client did not acknowledge FILE_ALREADY_EXISTS for {}.", client);
        }
        running = false;
        continue;
      }

      Logger::getInstance().info(
          "HandleClient",
          "Chunk processed " + std::to_string(chunk.getChunkNumber()) + " of " +
              std::to_string(chunk.getTotalChunk() - 1) + " for file " +
              chunk.getFilename() + " from client " + client);

      const int simulatedAckDelayMillis =
          ServerConfig::getInstance().getSimulatedAckDelayMillis(
              static_cast<size_t>(chunk.getChunkNumber()));
      if (simulatedAckDelayMillis > 0) {
        Logger::getInstance().infoC(
            "HandleClient", chunk.getFilename(),
            "Applying simulated ACK delay of {} ms for chunk {}.",
            simulatedAckDelayMillis, chunk.getChunkNumber());
        std::this_thread::sleep_for(
            std::chrono::milliseconds(simulatedAckDelayMillis));
      }

      if (receiveResult == FileReceiver::ReceiveResult::JustCompleted) {
        Logger::getInstance().infoC("HandleClient", chunk.getFilename(),
                                    "All chunks received for file: {}",
                                    chunk.getFilename());
        if (!sendControlMessage("SUCCESS_FILE_COMPLETE")) {
          Logger::getInstance().errorC(
              "HandleClient", chunk.getFilename(),
              "Failed to send success message to client {}", client);
        }

        std::string clientCommand;
        if (receiveControlMessage(clientCommand)) {
          if (clientCommand != "CLOSE_NOW") {
            Logger::getInstance().warningC(
                "HandleClient", chunk.getFilename(),
                "Unexpected command '{}' after file complete from {}.",
                clientCommand, client);
          }
        } else {
          Logger::getInstance().warningC(
              "HandleClient", chunk.getFilename(),
              "Client did not acknowledge completion for {}.", client);
        }
        running = false;
      } else {
        if (!sendControlMessage("SUCCESS")) {
          Logger::getInstance().errorC(
              "HandleClient", chunk.getFilename(),
              "Failed to send success message to client {}", client);
        }
      }
    } catch (const std::exception &e) {
      Logger::getInstance().error("HandleClient",
                                  "Exception in client handler {}: {}", client,
                                  e.what());
      circuitBreaker.reportFailure();
      const bool sent = sendErrorToClient("FAILED: request rejected");
      if (!sent || !connection.isConnected()) {
        running = false;
      }
    } catch (...) {
      Logger::getInstance().error(
          "HandleClient", "Unknown exception in client handler {}", client);
      circuitBreaker.reportFailure();
      const bool sent = sendErrorToClient("FAILED: Unknown");
      if (!sent || !connection.isConnected()) {
        running = false;
      }
    }
  }

  Logger::getInstance().info("ClientHandler",
                             "Client handler finished for socket {}", client);
}

bool ClientHandler::sendControlMessage(const std::string &message) {
  if (!ServerConfig::getInstance().isSecurityEnabled()) {
    return connection.sendString(message);
  }

  try {
    if (connectionSessionId.empty() ||
        nextSecureOutboundSequence == (std::numeric_limits<uint64_t>::max)()) {
      throw std::runtime_error("Secure control session sequence is unavailable.");
    }
    std::vector<char> encrypted = SecurePacket::EncryptControlMessage(
        message, connectionPreSharedKey, connectionSessionId,
        nextSecureOutboundSequence++);
    return connection.sendAll(encrypted.data(), static_cast<int>(encrypted.size()));
  } catch (const std::exception &e) {
    Logger::getInstance().error("SendControlMessage",
                                "Failed to encrypt control message: {}",
                                e.what());
    return false;
  }
}

bool ClientHandler::receiveControlMessage(std::string &message) {
  if (!ServerConfig::getInstance().isSecurityEnabled()) {
    return connection.recvString(message);
  }

  uint32_t totalLengthNetwork = 0;
  if (!connection.recvAll(reinterpret_cast<char *>(&totalLengthNetwork),
                          sizeof(totalLengthNetwork))) {
    return false;
  }

  const uint32_t totalLength = ntohl(totalLengthNetwork);
  if (totalLength == 0 || totalLength > maxSecureControlContentLength()) {
    Logger::getInstance().warning("ReceiveControlMessage",
                                  "Invalid secure control message length");
    connection.close();
    return false;
  }

  std::vector<char> buffer(sizeof(uint32_t) + totalLength);
  std::memcpy(buffer.data(), &totalLengthNetwork, sizeof(totalLengthNetwork));
  if (!connection.recvAll(buffer.data() + sizeof(uint32_t), totalLength)) {
    return false;
  }

  if (connectionSessionId.empty()) {
    Logger::getInstance().warning("ReceiveControlMessage",
                                  "Secure control received without session state");
    connection.close();
    return false;
  }

  try {
    SecurePacket::SessionMetadata metadata;
    message = SecurePacket::DecryptControlMessage(
        buffer, connectionPreSharedKey, connectionSessionId, &metadata);
    if (expectedSecureInboundSequence ==
            (std::numeric_limits<uint64_t>::max)() ||
        metadata.sequenceNumber != expectedSecureInboundSequence) {
      Logger::getInstance().warning("ReceiveControlMessage",
                                    "Replayed or out-of-order secure control packet");
      connection.close();
      return false;
    }
    ++expectedSecureInboundSequence;
    return true;
  } catch (const std::exception &e) {
    Logger::getInstance().warning("ReceiveControlMessage",
                                  "Failed to decrypt secure control message: {}",
                                  e.what());
    connection.close();
    return false;
  }
}

bool ClientHandler::sendErrorToClient(const std::string &message) {
  if (!sendControlMessage(message)) {
    Logger::getInstance().error("SendErrorToClient",
                                "Failed to send error message to client");
    circuitBreaker.reportFailure();
    return false;
  }
  return true;
}

bool ClientHandler::parseChunkMessage(const std::vector<char> &buffer,
                                      ChunkMetadata &chunk) {
  std::string parseError;
  if (!ChunkPacketParser::Parse(buffer, chunk, &parseError)) {
    if (parseError.empty()) {
      parseError = "Unknown parse error";
    }
    Logger::getInstance().error("ParseChunkMessage", parseError);
    return false;
  }
  return true;
}
